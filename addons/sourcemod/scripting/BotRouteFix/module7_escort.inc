#if defined _botroutefix_module7_escort_included
    #endinput
#endif
#define _botroutefix_module7_escort_included

//========================================================================================
// MODULE 7: T C4 CARRIER ESCORT & BODYGUARD ROSTER
//
// 花名册制贴身保镖：按士气配 3-Morale 名保镖，绝境抱团、顺风放飞。
//   - 人类包匪：CARRIER 模式，全队保镖直接 Follow 包匪（人类自主导航）。
//   - Bot 包匪：
//     * 若保镖队伍中存在比包匪更靠近目标包点的人，选最近者为先锋（POINTMAN 模式），
//       先锋直奔包点，包匪与其余保镖 Follow 先锋；
//     * 若包匪自己才是最近的，不存在先锋，全体保镖 Follow 包匪（CARRIER 模式）。
//   - 先锋代表包匪意志：只去目标包点（M2 lock 优先，否则原生最近），不等待下包时机。
//   - 先锋到场交接：先锋到达包点（<= 400.0 码）完成领跑使命，平滑转入 CARRIER 模式，
//     包匪回原生下包，全体保镖（含前先锋）改为跟随并保护包匪。
//   - 动态补员：保镖阵亡补保镖；先锋阵亡从剩余保镖中择优替补先锋，无更近者则包匪领跑；
//     包匪转移/掉包彻底释放旧队伍，新包匪从 0 组织。
//   - 传染源切断：BotFollowMeme::Interpret 入口写 ret（64=C3 / 32=C2 08 00），
//     原生开局 "Going to plant the bomb" meme 全员盲从链物理灭活，名册不再被冲垮
//========================================================================================

//========================================================================================
// MODULE 7 DEFINES & GLOBALS
//========================================================================================

// 编制公式（单点定义）：保镖数 = ESCORT_BASE_COUNT - Morale，钳制 [0, ESCORT_MAX_COUNT]
//   Morale -3 → 6 保镖抱团推点；Morale +3 → 0 保镖放飞控图；默认 +1 → 2 保镖
#define ESCORT_BASE_COUNT 3
#define ESCORT_MAX_COUNT 6

// 先锋到场判定与自愈（距离均为 3D 欧氏）
#define ESCORT_ARRIVE_RADIUS     400.0   // 距目标包点中心 <= 该值视为先锋到场
#define ESCORT_REISSUE_INTERVAL  1.0     // 重发 MoveTo 最小间隔（防不可达目标瞬时风暴）

// 护送链路三态（唯一权威状态，所有迁移经 Escort_SwitchTo* / Escort_ReleaseAll）
enum EscortMode
{
    Escort_None = 0,     // 无队伍（未武装 / 已释放）
    Escort_Carrier,      // 保镖 Follow 包匪（人类包匪 / 先锋到场交接后 / 目标不可用回退）
    Escort_Pointman      // Bot 包匪：先锋领跑，包匪与其余保镖 Follow 先锋
};

// 先锋目标子状态（仅 Escort_Pointman 生效）
enum struct PointmanObjective
{
    bool  active;        // 目标是否已下发
    float goal[3];       // 目标包点中心
    float lastIssue;     // 最近一次下发 MoveTo 的时刻（秒）
}

// 本模块私有状态：花名册（不出本文件，对外只暴露 Escort_* 与 Module7_*）
static bool g_bModule7_Active = false;   // 业务开启态
static EscortMode g_iEscortMode = Escort_None;
static int g_iEscortCarrier = 0;
static int g_iEscortPointman = 0;        // 仅 Pointman 模式非 0
static int g_iBodyguardList[MAXPLAYERS + 1];
static int g_iBodyguardCount = 0;
static int g_iEscortTarget = 0;
static bool g_bPointmanArrived = false;   // 先锋到场交接锁（到场后不再重复选拔先锋，换包/换局复位）
static PointmanObjective g_PointmanObjective;
static int g_iEscortGeneration = 0;      // 延迟帧代际，任何迁移/释放 +1
static bool g_bSuperviseScheduled = false;

// 先锋监督：CCSBot::Idle 入口 Detour（感知 MoveTo 完成/放弃、Hide/Hunt 结束）
static Handle g_hIdleDetour = null;
static bool g_bIdleHooked = false;

// Meme 灭活补丁：BotFollowMeme::Interpret 入口 ret（64 位 1 字节 C3 / 32 位 3 字节 C2 08 00）
static MemoryPatch g_PatchFollowMeme7;
static int g_FollowMeme7PatchBytes[3];

enum struct EscortCandidate
{
    int client;
    float dist;
}

//========================================================================================
// MODULE 7 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：保镖花名册事项（ITEM_M7_ESCORT）
 *
 * 校验 Follow/StopFollowing/MoveTo/Idle SDKCall 句柄、包匪士气偏移、meme 灭活补丁
 * 与 CCSBot::Idle 监督 Detour，不做任何名单操作。任一后端缺失返回 false，Init 会把
 * 护送事项标记为 FAILED。meme 补丁缺席时名册会被原生开局 meme 冲垮，不降级。
 *
 * @return              true = 护送事项后端就绪
 */
static bool PrepModule7_EscortBackend()
{
    bool sdkOk = (g_hSDKCall_Follow != null && g_hSDKCall_StopFollowing != null
        && g_hSDKCall_MoveTo != null && g_hSDKCall_Idle != null);
    bool moraleOk = (g_iOffset_Morale > 0);
    bool memeOk = PrepFollowMemeRetPatch();
    bool hookOk = MountPointmanIdleDetour();

    if (!sdkOk)
    {
        LogError("[BotRouteFix] [Module 7] Follow/StopFollowing/MoveTo/Idle SDKCall missing; escort item FAILED.");
    }
    if (!moraleOk)
    {
        LogError("[BotRouteFix] [Module 7] Morale offset missing; escort item FAILED.");
    }
    if (!hookOk)
    {
        LogError("[BotRouteFix] [Module 7] CCSBot_Idle detour missing; escort item FAILED.");
    }

    if (sdkOk && moraleOk && memeOk && hookOk)
    {
        LogMessage("[BotRouteFix] [Module 7] Carrier escort backend verified (follow/moveto/idle SDKCall, morale=%d, meme ret, idle detour).",
            g_iOffset_Morale);
    }
    return (sdkOk && moraleOk && memeOk && hookOk);
}

/**
 * 挂载先锋监督 CCSBot::Idle Detour（事项 ITEM_M7_ESCORT 监督载体，紧跟 Prep）
 *
 * 同一签名既用于 SDKCall（BotIdle 取消 MoveToState），也用于 Detour（感知先锋转入 Idle）。
 * 失败语义：句柄创建或签名绑定失败返回 false，护送事项 FAILED。
 *
 * @return              true = Detour 句柄就绪
 */
static bool MountPointmanIdleDetour()
{
    if (g_hIdleDetour != null)
    {
        return true;
    }

    g_hIdleDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_CBaseEntity);
    if (g_hIdleDetour == null)
    {
        LogError("[BotRouteFix] [Module 7] CCSBot_Idle detour creation failed; escort item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hIdleDetour, g_hGameConf, SDKConf_Signature, "CCSBot_Idle"))
    {
        LogError("[BotRouteFix] [Module 7] CCSBot_Idle signature missing; escort item FAILED.");
        delete g_hIdleDetour;
        g_hIdleDetour = null;
        return false;
    }

    return true;
}

/**
 * 准备 BotFollowMeme::Interpret 入口 ret 灭活补丁（事项 ITEM_M7_ESCORT 补丁载体，紧跟 Prep）
 *
 * 只定位/校验/备份并把构造好的 ret 字节写入缓冲，不写代码段。
 * 补丁语义：meme 解释器入口直接返回，原生开局 "Going to plant the bomb"
 * 全员盲从链（1000 码内 receiver->Follow(sender) + CoveringFriend 应答）
 * 物理灭活；M7 自己的 BotFollow SDKCall 走直调，不受影响。
 * 架构差异：64 位调用方清栈用 1 字节 C3；32 位 thiscall 被调方清 8 字节
 * 参数用 3 字节 C2 08 00——裸 C3 会坏栈，两种架构不可混用。
 * 失败语义：签名缺失、特征码失配或备份失败返回 false，护送事项 FAILED。
 *
 * @return              true = 灭活补丁后端就绪
 */
static bool PrepFollowMemeRetPatch()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "FollowMeme_Interpret");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 7] Failed to locate signature for BotFollowMeme_Interpret!");
        return false;
    }

    int patchSize = g_bIsWin64 ? 1 : 3;
    g_PatchFollowMeme7.Init("FollowMeme7_Ret", sigAddr, patchSize);

    int b0 = LoadFromAddress(sigAddr, NumberType_Int8);
    int b1 = LoadFromAddress(sigAddr + view_as<Address>(1), NumberType_Int8);

    // 幂等性检测：入口已是 ret 即自家补丁；有备份=保持基线，无备份=外来残留记 false
    bool alreadyRet = g_bIsWin64 ? (b0 == 0xC3)
        : (b0 == 0xC2 && b1 == 0x08
            && LoadFromAddress(sigAddr + view_as<Address>(2), NumberType_Int8) == 0x00);
    if (alreadyRet)
    {
        if (!g_PatchFollowMeme7.hasBackup)
        {
            g_PatchFollowMeme7.isPatched = false;
            LogError("[BotRouteFix] [Module 7] FollowMeme already ret without baseline @ %X; escort item FAILED.", sigAddr);
            return false;
        }
        LogMessage("[BotRouteFix] [Module 7] FollowMeme ret already present @ %X, skipping", sigAddr);
        return true;
    }

    // 预检：入口必须是标准帧 prologue（32: 55 8B EC；64: 48 89 开头）
    bool prologueOk = g_bIsWin64 ? (b0 == 0x48 && b1 == 0x89)
        : (b0 == 0x55 && b1 == 0x8B
            && LoadFromAddress(sigAddr + view_as<Address>(2), NumberType_Int8) == 0xEC);
    if (!prologueOk)
    {
        LogError("[BotRouteFix] [Module 7] FollowMeme entry opcode mismatch (got %02X %02X), patch aborted!",
            b0, b1);
        return false;
    }

    if (!g_PatchFollowMeme7.BackupOriginal())
    {
        LogError("[BotRouteFix] [Module 7] FollowMeme baseline backup failed; escort item FAILED.");
        return false;
    }

    // 构造 ret：64 = C3；32 = C2 08 00（清 8 字节栈参数）
    if (g_bIsWin64)
    {
        g_FollowMeme7PatchBytes[0] = 0xC3;
    }
    else
    {
        g_FollowMeme7PatchBytes[0] = 0xC2;
        g_FollowMeme7PatchBytes[1] = 0x08;
        g_FollowMeme7PatchBytes[2] = 0x00;
    }

    LogMessage("[BotRouteFix] [Module 7] FollowMeme ret verified @ %X [%s]",
        sigAddr, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

//========================================================================================
// MODULE 7 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 7 护送事项（ITEM_M7_ESCORT）
 *
 * 失败语义：GameConf 缺失或 Prep 失败时标记 FAILED 并返回 false。
 *
 * @return              true = 护送事项已武装
 */
bool InitModule7_Escort()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 7] Gamedata missing; escort item FAILED.");
        Item_SetReady(ITEM_M7_ESCORT, false);
        return false;
    }

    bool escortReady = PrepModule7_EscortBackend();
    Item_SetReady(ITEM_M7_ESCORT, escortReady);

    EnableModule7_Escort(true);
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        LogModuleAllFailed(7);
        return false;
    }

    LogModuleArmed(7, "Carrier escort", "Euclidean bodyguards + pointman");
    return true;
}

/**
 * 动态启用或停用模块 7（属于事项 ITEM_M7_ESCORT）
 *
 * 调度 Apply 或 Restore 后端；只武装 Ready 的事项；停用即解散名单解除全员跟随。
 *
 * @param enable        true = 武装事项；false = 解除武装并还原资源
 */
void EnableModule7_Escort(bool enable)
{
    if (enable == g_bModule7_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule7_EscortBackend();
        g_bModule7_Active = false;
        return;
    }

    g_bModule7_Active = ApplyModule7_EscortBackend();
}

/**
 * 还原模块 7（插件卸载，属于事项 ITEM_M7_ESCORT）
 *
 * 解除跟随链、还原 meme 补丁、脱钩并释放 Detour 句柄。
 */
void RestoreModule7_Escort()
{
    EnableModule7_Escort(false);

    if (g_hIdleDetour != null)
    {
        delete g_hIdleDetour;
        g_hIdleDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 7] Escort hooks and memory patches safely restored.");
}

//========================================================================================
// MODULE 7 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M7 护送花名册的遥测读取：当前保镖数（供 status / 可视化）
 */
int Module7_GetBodyguardCount()
{
    return g_iBodyguardCount;
}

/**
 * 事项 M7 护送花名册的遥测读取：当前编制目标（供 status / 可视化）
 */
int Module7_GetTargetCount()
{
    return g_iEscortTarget;
}

/**
 * 事项 M7 护送花名册的遥测读取：当前包匪（供 status / 可视化；无包匪返回 0）
 */
int Module7_GetCarrier()
{
    return g_iEscortCarrier;
}

/**
 * 事项 M7 护送花名册的遥测读取：当前开路先锋（供 status / 可视化；无先锋返回 0）
 */
int Module7_GetPointman()
{
    return g_iEscortPointman;
}

/**
 * 获取当前存活保镖列表（供可视化子系统渲染护航连线）
 *
 * @param bodyguards    输出保镖 client 索引数组
 * @param maxCount      输出数组容量
 * @return              实际有效保镖数量
 */
int Module7_GetBodyguards(int[] bodyguards, int maxCount)
{
    int count = 0;
    for (int i = 0; i < g_iBodyguardCount && count < maxCount; i++)
    {
        int bot = g_iBodyguardList[i];
        if (IsBot(bot, true))
        {
            bodyguards[count++] = bot;
        }
    }
    return count;
}

/**
 * 事项 M7 护送花名册的保镖身份查询（供模块 5 后撤档位裁决用）
 *
 * @return              client 是否在保镖名单
 */
bool Module7_IsBodyguard(int client)
{
    return Escort_Find(client) != -1;
}

//========================================================================================
// MODULE 7 EVENT HANDLERS
//========================================================================================

/**
 * 回合重置 (round_start)
 */
void Module7_OnRoundStart()
{
    Escort_Reset();
}

/**
 * 地图加载 (OnMapStart)：名单索引随实体全废，新 bot 复用索引会被误认，必须清
 */
void Module7_OnMapStart()
{
    Escort_Reset();
}

/**
 * 冻结时间结束 (round_freeze_end)：开局配一次保镖（时钟归 module2，本模块不碰）
 */
void Module7_OnRoundFreezeEnd()
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    Escort_Refresh();
}

/**
 * 道具拾取事件 (item_pickup)：中途换包匪递延一帧重配（等槽位归属落定）
 */
void Module7_OnItemPickup(int client, const char[] item)
{
    #pragma unused client
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    // 冻结期内开局发包不触发（买枪期跟随会打断 BuyState）
    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        RequestFrame(Module7_FrameRefresh);
    }
}

/**
 * 中途捡包递延帧（RequestFrame 回调）
 */
public void Module7_FrameRefresh(any data)
{
    // 递延帧可能在停用之后才到，必须二次校验事项武装态
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    Escort_Refresh();
}

/**
 * C4 掉落事件 (bomb_dropped)：释放名单，残局交给 module2 的原生选点与 module9 的捡包
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 */
void Module7_OnBombDropped()
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    DebugLog("[BotRouteFix] [Module 7] C4 dropped! Disbanding bodyguard roster (%d guards released)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 安放事件 (bomb_planted)：释放全体保镖跟随，立即转入模块 6 守点与掩体架枪任务 (GUARD_TICKING_BOMB / HideState)
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 */
void Module7_OnBombPlanted()
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    DebugLog("[BotRouteFix] [Module 7] C4 planted! Disbanding bodyguard roster (%d guards released to site defense)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 拆除事件：释放名单，确保无残留跟随
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 */
void Module7_OnBombDefused()
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    Escort_ReleaseAll();
}

/**
 * 事项 M7 护送花名册的 C4 爆炸处理：释放名单，确保无残留跟随
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 * 与 Defused 同策：爆炸后保镖无保护目标，全部恢复自由身。
 */
void Module7_OnBombExploded()
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    Escort_ReleaseAll();
}

/**
 * 玩家死亡事件 (player_death)
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 * 包匪阵亡当场解散名册；保镖/先锋阵亡触发除名并补员对账。
 */
void Module7_OnPlayerDeath(int victim)
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    // 包匪阵亡：当场彻底释放保镖名单，保镖立即恢复自由身可就近捡包、自由迎敌或防守
    if (Escort_IsCarrier(victim))
    {
        Escort_ReleaseAll();
        return;
    }

    // 保镖阵亡：除名 + 按当前士气重算编制，多退少补
    Escort_OnRosterLost(victim, true);
}

/**
 * 客户端掉线处理（主文件 OnClientDisconnect 直调，game event 到不了这里）
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) 直接返回。
 * 包匪掉线解散名册；保镖/先锋掉线除名并补员对账。
 */
void Module7_OnClientDisconnect(int client)
{
    if (!Item_IsArmed(ITEM_M7_ESCORT))
    {
        return;
    }

    Escort_OnClientDisconnect(client);
}

//========================================================================================
// MODULE 7 DETOUR & HOOKS (CCSBOT::IDLE DETOUR)
//========================================================================================

/**
 * CCSBot::Idle 前置回调：感知先锋非正常停步
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 门禁：!Item_IsArmed(ITEM_M7_ESCORT) || g_iEscortMode != Escort_Pointman 则返回 MRES_Ignored。
 * 只做过滤与延迟，不在 detour 内改状态；真正决策延后至 Escort_FrameSupervise。
 * 目的：Follow 角色靠 IdleState::IsFollowing -> ContinueFollowing 自愈，先锋不是 Follow，
 * 必须由本回调在每次收口时复核目标，到场交接、未到场重发。
 *
 * @param client        客户端索引
 * @return              MRES_Ignored
 */
public MRESReturn Hook_CCSBotIdle_Pre(int client)
{
    if (!Item_IsArmed(ITEM_M7_ESCORT)
        || g_iEscortMode != Escort_Pointman
        || !g_PointmanObjective.active
        || client != g_iEscortPointman
        || g_bSuperviseScheduled)
    {
        return MRES_Ignored;
    }

    g_bSuperviseScheduled = true;
    RequestFrame(Escort_FrameSupervise, g_iEscortGeneration);
    return MRES_Ignored;
}

/**
 * 重发节流用一次性定时器（属于事项 ITEM_M7_ESCORT）
 *
 * 与 RequestFrame 共用同一套监督逻辑；执行后返回 Plugin_Stop 自动自毁。
 */
public Action Escort_TimerSupervise(Handle timer, any data)
{
    Escort_FrameSupervise(data);
    return Plugin_Stop;
}

/**
 * 先锋监督延后帧：到场交接，或未到场重发 MoveTo（属于事项 ITEM_M7_ESCORT）
 *
 * 门禁：代际失配或 !Item_IsArmed(ITEM_M7_ESCORT) 直接放弃。
 *
 * @param data          下发时的代际（与 g_iEscortGeneration 不一致即作废）
 */
public void Escort_FrameSupervise(any data)
{
    g_bSuperviseScheduled = false;

    if (view_as<int>(data) != g_iEscortGeneration)
    {
        return;
    }

    if (!Item_IsArmed(ITEM_M7_ESCORT)
        || g_iEscortMode != Escort_Pointman
        || !g_PointmanObjective.active)
    {
        return;
    }

    int carrier = g_iEscortCarrier;
    int pointman = g_iEscortPointman;

    if (!IsValidClient(carrier, true))
    {
        Escort_ReleaseAll();
        return;
    }
    if (!IsTeamBot(pointman, 2, true))
    {
        Escort_Reconcile();
        return;
    }

    float pos[3];
    float goal[3];
    GetClientAbsOrigin(pointman, pos);
    for (int i = 0; i < 3; i++)
    {
        goal[i] = g_PointmanObjective.goal[i];
    }

    float distSq = Escort_DistSq(pos, goal);
    float arriveSq = ESCORT_ARRIVE_RADIUS * ESCORT_ARRIVE_RADIUS;

    float carrierPos[3];
    GetClientAbsOrigin(carrier, carrierPos);
    float carrierDistSq = Escort_DistSq(carrierPos, goal);

    // 规则 4：到场交接（先锋到场 或 包匪自身已到场，任一达成即交接回包匪）
    if (distSq <= arriveSq || carrierDistSq <= arriveSq)
    {
        g_bPointmanArrived = true;
        DebugLog("[BotRouteFix] [Module 7] Arrived bombsite (pointman dist %.0f, carrier dist %.0f); handoff to carrier %N",
            GetVectorDistance(pos, goal), GetVectorDistance(carrierPos, goal), carrier);
        Escort_SwitchToCarrier(carrier);
        return;
    }

    float now = GetGameTime();

    // 重发节流：不可达目标会每帧 Idle，必须限制为每 ESCORT_REISSUE_INTERVAL 一次
    float sinceIssue = now - g_PointmanObjective.lastIssue;
    if (sinceIssue < ESCORT_REISSUE_INTERVAL)
    {
        if (!g_bSuperviseScheduled)
        {
            g_bSuperviseScheduled = true;
            CreateTimer(ESCORT_REISSUE_INTERVAL - sinceIssue, Escort_TimerSupervise,
                g_iEscortGeneration, TIMER_FLAG_NO_MAPCHANGE);
        }
        return;
    }

    g_PointmanObjective.lastIssue = now;
    DebugLog("[BotRouteFix] [Module 7] Pointman %N idle (dist %.0f); reissue MoveTo",
        pointman, GetVectorDistance(pos, goal));

    BotStopFollowing(pointman);
    BotMoveTo(pointman, goal);
}

//========================================================================================
// MODULE 7 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用护送事项：先落 meme 灭活补丁，挂载先锋 Idle detour，再武装并重配名单
 *
 * 事项 ITEM_M7_ESCORT 的应用入口；不做句柄与偏移校验（那是 Prep 的职责）。
 * 补丁写入失败证伪了 Prep 的模板校验，必须同步清 Ready 并回滚，不留半吊子。
 * 失败时该事项 FAILED。
 *
 * @return              true = 名单已武装
 */
static bool ApplyModule7_EscortBackend()
{
    if (!Item_IsReady(ITEM_M7_ESCORT)
        || g_hSDKCall_Follow == null || g_hSDKCall_StopFollowing == null
        || g_hSDKCall_MoveTo == null || g_hSDKCall_Idle == null
        || g_hIdleDetour == null)
    {
        Item_SetArmed(ITEM_M7_ESCORT, false);
        return false;
    }

    // 先落 meme 灭活补丁：补丁不在位，Escort_Refresh 配出的名册会被原生开局 meme 瞬间冲垮
    if (!g_PatchFollowMeme7.Apply(g_FollowMeme7PatchBytes))
    {
        g_PatchFollowMeme7.Restore();
        Item_SetReady(ITEM_M7_ESCORT, false);
        Item_SetArmed(ITEM_M7_ESCORT, false);
        LogError("[BotRouteFix] [Module 7] FollowMeme ret write failed; escort item FAILED.");
        return false;
    }

    // 挂载先锋监督 Detour：挂载失败只清 Armed（句柄有效，待下次全关全开重试）
    if (!SyncDetourEnabled(g_hIdleDetour, g_bIdleHooked, false, Hook_CCSBotIdle_Pre, true, "Module 7"))
    {
        g_PatchFollowMeme7.Restore();
        Item_SetArmed(ITEM_M7_ESCORT, false);
        LogError("[BotRouteFix] [Module 7] CCSBot_Idle detour enable failed; escort item FAILED.");
        return false;
    }

    Item_SetArmed(ITEM_M7_ESCORT, true);
    Escort_Refresh();
    return true;
}

/**
 * 还原护送事项：脱下先锋监督 Detour，先恢复 meme 解释器字节，再解除武装并解散名单（停用即全员放行回原生）
 */
static void RestoreModule7_EscortBackend()
{
    // 先摘监督钩子，再释放（释放里的 BotIdle 不再回调 M7）
    SyncDetourEnabled(g_hIdleDetour, g_bIdleHooked, false, Hook_CCSBotIdle_Pre, false, "Module 7");
    g_PatchFollowMeme7.Restore();
    Item_SetArmed(ITEM_M7_ESCORT, false);
    Escort_ReleaseAll();
}

//========================================================================================
// MODULE 7 ROSTER ENGINE & INTERNAL PRIMITIVES
//========================================================================================

/**
 * 事项 M7 护送花名册的编制公式：保镖数 = ESCORT_BASE_COUNT - Morale，钳制 [0, ESCORT_MAX_COUNT]
 *
 * 纯计算（Morale -3 → 6 保镖抱团；Morale +3 → 0 保镖放飞；默认 +1 → 2 保镖），无失败分支。
 *
 * @param morale        包匪（或 T 均值）士气
 * @return              本回合应配保镖数
 */
static int Escort_DesiredCount(int morale)
{
    int n = ESCORT_BASE_COUNT - morale;
    if (n < 0) n = 0;
    if (n > ESCORT_MAX_COUNT) n = ESCORT_MAX_COUNT;
    return n;
}

/**
 * 事项 M7 护送花名册的名单查找辅助
 *
 * 纯内存查找，不触碰游戏状态；client 无效或不在名单返回 -1。
 *
 * @param bot           客户端索引
 * @return              名单下标，不在名单返回 -1
 */
static int Escort_Find(int bot)
{
    if (!IsBot(bot, false))
    {
        return -1;
    }

    for (int i = 0; i < g_iBodyguardCount; i++)
    {
        if (g_iBodyguardList[i] == bot)
            return i;
    }
    return -1;
}

/**
 * 事项 M7 护送花名册的防腐钳制
 *
 * 正常流程 count 远小于 MAXPLAYERS；一旦离谱直接清零并报错，绝不让循环失控。
 */
static void Escort_ClampRoster()
{
    if (g_iBodyguardCount < 0 || g_iBodyguardCount > MAXPLAYERS)
    {
        LogError("[BotRouteFix] [Module 7] insane roster count (%d), resetting to 0", g_iBodyguardCount);
        g_iBodyguardCount = 0;
    }
}

/**
 * 事项 M7 护送花名册的按下标删除（swap-remove）
 *
 * 只动数组不发 SDKCall；调用方决定要不要给被删者发 Stop。
 *
 * @param idx           名单下标，越界直接返回
 */
static void Escort_RemoveAt(int idx)
{
    if (idx < 0 || idx >= g_iBodyguardCount)
    {
        return;
    }
    g_iBodyguardList[idx] = g_iBodyguardList[g_iBodyguardCount - 1];
    g_iBodyguardCount--;
}

/**
 * 事项 M7 护送花名册的按 client 删除
 *
 * 先查找后删除；不在名单即 no-op。只动数组不发 SDKCall。
 *
 * @param bot           客户端索引
 */
static void Escort_RemoveFromRoster(int bot)
{
    int idx = Escort_Find(bot);
    if (idx != -1)
    {
        Escort_RemoveAt(idx);
    }
}

/**
 * 事项 M7 护送花名册的排序用距离：欧氏 3D 平方（含 Z，跨楼层天然排后）
 *
 * 纯计算；排序只比大小，开方省掉。
 */
static float Escort_DistSq(const float a[3], const float b[3])
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

/**
 * 事项 M7 护送花名册的补员挑选：在名单之外按 anchor 挑最近的 need 个
 *
 * carrier 本人恒排除；只选存活 T bot；result 容量上限 maxOut，人不够就少挑不硬凑。
 *
 * @param anchor        排序基准，一般取包匪坐标
 * @param carrier       包匪，本人恒排除
 * @param need          想补的人数
 * @param result        输出数组
 * @param maxOut        输出数组容量上限
 * @return              实际挑到数
 */
static int Escort_PickClosest(float anchor[3], int carrier, int need, int result[MAXPLAYERS + 1], int maxOut)
{
    EscortCandidate pool[MAXPLAYERS + 1];
    int poolCount = 0;

    for (int i = 1; i <= MaxClients; i++)
    {
        if (i == carrier || !IsTeamBot(i, 2, true) || Escort_Find(i) != -1)
        {
            continue;
        }

        float pos[3];
        GetClientAbsOrigin(i, pos);

        pool[poolCount].client = i;
        pool[poolCount].dist = Escort_DistSq(pos, anchor);
        poolCount++;
    }

    for (int i = 0; i < poolCount - 1; i++)
    {
        for (int j = 0; j < poolCount - i - 1; j++)
        {
            if (pool[j].dist > pool[j + 1].dist)
            {
                EscortCandidate temp;
                temp.client = pool[j].client;
                temp.dist = pool[j].dist;
                pool[j].client = pool[j + 1].client;
                pool[j].dist = pool[j + 1].dist;
                pool[j + 1].client = temp.client;
                pool[j + 1].dist = temp.dist;
            }
        }
    }

    int n = (need < poolCount) ? need : poolCount;
    if (n > maxOut) n = maxOut;
    for (int k = 0; k < n; k++)
        result[k] = pool[k].client;
    return n;
}

/**
 * 事项 M7 护送花名册的超编裁剪：从远到近停掉多余的，只留最近的 desired 个
 *
 * 无效/非 T/阵亡成员优先剔除；被裁者发 StopFollowing。
 */
static void Escort_TrimToTarget(float anchor[3], int desired)
{
    while (g_iBodyguardCount > desired)
    {
        int farIdx = -1;
        float farD = -1.0;
        for (int i = 0; i < g_iBodyguardCount; i++)
        {
            int bot = g_iBodyguardList[i];

            // 编制未清零时保护存活先锋：即使他离包匪最远也不裁（死亡/转队的照常剔除）
            if (desired > 0 && bot == g_iEscortPointman && IsTeamBot(bot, 2, true))
            {
                continue;
            }

            if (!IsTeamBot(bot, 2, true))
            {
                // 无效、非 T 或阵亡成员优先剔除
                farIdx = i;
                break;
            }

            float pos[3];
            GetClientAbsOrigin(bot, pos);
            float d = Escort_DistSq(pos, anchor);
            if (d > farD)
            {
                farD = d;
                farIdx = i;
            }
        }

        if (farIdx == -1)
        {
            break;
        }

        int targetBot = g_iBodyguardList[farIdx];
        BotStopFollowing(targetBot);
        if (targetBot == g_iEscortPointman)
        {
            // desired == 0 才会走到这里：撤掉目标并取消 MoveTo
            g_PointmanObjective.active = false;
            g_iEscortPointman = 0;
            g_iEscortGeneration++;
            if (IsBot(targetBot, true))
            {
                BotIdle(targetBot);
            }
        }
        Escort_RemoveAt(farIdx);
    }
}

/**
 * 事项 M7 护送花名册的缺编补充：只补缺口，不碰现有人
 *
 * 名单已达 desired 直接返回；补进来的人立即发 Follow。
 */
static void Escort_FillToTarget(int carrier, float anchor[3], int desired)
{
    if (g_iBodyguardCount >= desired)
        return;

    int need = desired - g_iBodyguardCount;
    int picked[MAXPLAYERS + 1];
    int n = Escort_PickClosest(anchor, carrier, need, picked, MAXPLAYERS + 1);
    for (int k = 0; k < n; k++)
    {
        g_iBodyguardList[g_iBodyguardCount++] = picked[k];
    }
}

/**
 * 解析先锋目标包点（代表包匪意志：优先 M2 锁定包点，未锁时原生就近）
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 先锋只继承包匪去哪个包点的意志，不继承何时去（开局第 0 秒即出发，不等待运包时钟）。
 *
 * @param carrier       包匪索引（用于计算原生就近起点）
 * @param goal          输出目标包点世界坐标 (3D)
 * @param locked        输出布尔：true = 命中 M2 锁定包点，false = 原生就近
 * @return              true = 成功解析有效包点，false = 地图无可用包点
 */
static bool Escort_ResolveGoal(int carrier, float goal[3], bool &locked)
{
    // 1. 优先读取模块 2 的锁定包点
    if (Module2_GetTargetBombsitePosition(goal))
    {
        locked = true;
        return true;
    }

    // 2. 未锁定时，以包匪当前坐标为基准获取原生就近包点
    float carrierPos[3];
    GetClientAbsOrigin(carrier, carrierPos);
    locked = false;
    return Module2_GetNativeClosestBombsitePosition(carrierPos, goal);
}

/**
 * 从保镖队伍中挑选比包匪更靠近目标包点的存活 T bot 担任先锋
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 【核心设计 2】：
 * 1. 计算包匪到目标的距离 carrierDist；
 * 2. 遍历当前保镖名册，找出离目标最近的保镖 bestDist；
 * 3. 只有当 bestDist < carrierDist（保镖比包匪更近）时，该保镖当选先锋；
 * 4. 若包匪自己才是最近的（carrierDist <= bestDist），返回 0（不设先锋，包匪领跑）。
 *
 * @param carrier       包匪客户端索引
 * @param goal          目标包点世界坐标
 * @param exclude       排除的客户端索引（如已牺牲或失效的先锋）
 * @return              当选先锋客户端索引；0 = 无先锋（包匪自己领跑）
 */
static int Escort_PickPointmanTowardGoal(int carrier, const float goal[3], int exclude = 0)
{
    if (!IsValidClient(carrier, true))
    {
        return 0;
    }

    float carrierPos[3];
    GetClientAbsOrigin(carrier, carrierPos);
    float carrierDist = Escort_DistSq(carrierPos, goal);

    int best = 0;
    float bestDist = 0.0;

    for (int i = 0; i < g_iBodyguardCount; i++)
    {
        int bot = g_iBodyguardList[i];
        if (bot == exclude || !IsTeamBot(bot, 2, true))
        {
            continue;
        }

        float pos[3];
        GetClientAbsOrigin(bot, pos);
        float d = Escort_DistSq(pos, goal);
        if (best == 0 || d < bestDist)
        {
            best = bot;
            bestDist = d;
        }
    }

    // 严格比较：只有保镖比包匪更靠近包点，才当选先锋
    if (best != 0 && bestDist < carrierDist)
    {
        return best;
    }

    // 包匪自身比所有保镖更近或距离相等：返回 0，无先锋
    return 0;
}

/**
 * 按当前模式重挂跟随链（不重发 MoveTo，不做状态迁移）
 *
 * 属于事项 ITEM_M7_ESCORT。
 * - Pointman 模式：包匪 + 其余保镖 Follow 先锋；先锋本人不跟随任何人。
 * - Carrier 模式：全部保镖 Follow 包匪；包匪保持原生自主导航。
 * - None 模式：no-op。
 */
static void Escort_ReassertFollows()
{
    int carrier = g_iEscortCarrier;
    if (!IsValidClient(carrier, true))
    {
        return;
    }

    if (g_iEscortMode == Escort_Pointman)
    {
        int pointman = g_iEscortPointman;
        if (!IsTeamBot(pointman, 2, true))
        {
            return;
        }

        // 包匪反向跟随先锋（仅对 Bot 包匪生效）
        if (IsBot(carrier))
        {
            BotFollow(carrier, pointman);
        }

        // 其余保镖跟随先锋
        for (int i = 0; i < g_iBodyguardCount; i++)
        {
            int bot = g_iBodyguardList[i];
            if (bot != pointman && IsTeamBot(bot, 2, true))
            {
                BotFollow(bot, pointman);
            }
        }
        return;
    }

    if (g_iEscortMode == Escort_Carrier)
    {
        for (int i = 0; i < g_iBodyguardCount; i++)
        {
            int bot = g_iBodyguardList[i];
            if (IsTeamBot(bot, 2, true))
            {
                BotFollow(bot, carrier);
            }
        }
    }
}

/**
 * 切换到 CARRIER 模式：保镖重新 Follow 包匪
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 适用场景：人类包匪 / 先锋到场交接 / 目标包点不可用回退。
 * 若此前处于 Pointman 模式，包匪曾被强制反向跟随先锋，此处先 StopFollowing 再 Idle 拉回原生自主下包；
 * 前先锋（仍在保镖名册内）由后续 ReassertFollows 改为跟随包匪。
 *
 * @param carrier       包匪客户端索引
 */
static void Escort_SwitchToCarrier(int carrier)
{
    if (!IsValidClient(carrier, true))
    {
        Escort_ReleaseAll();
        return;
    }

    bool wasPointman = (g_iEscortMode == Escort_Pointman);

    g_iEscortMode = Escort_Carrier;
    g_iEscortPointman = 0;
    g_PointmanObjective.active = false;
    g_PointmanObjective.lastIssue = 0.0;
    g_bSuperviseScheduled = false;
    g_iEscortGeneration++;

    // 先锋交接：包匪从 FollowState 回到原生（自己进点/下包，M2 时钟重新生效）
    // 注意：Idle 不清 m_isFollowing/m_leader，必须先 StopFollowing 再 Idle
    if (wasPointman && IsBot(carrier))
    {
        BotStopFollowing(carrier);
        BotIdle(carrier);
    }

    Escort_ReassertFollows();
}

/**
 * 切换到 POINTMAN 模式：解析目标包点、选拔先锋、下发 MoveTo、建立跟随链
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 已在 POINTMAN 且先锋仍有效时只重挂跟随链，不重发 MoveTo（避免打断正在进行的寻路）。
 *
 * @param carrier       包匪客户端索引
 * @return              true = 先锋成功上电；false = 目标不可用 / 无先锋（应回退 CARRIER 模式）
 */
static bool Escort_SwitchToPointman(int carrier)
{
    if (!IsBot(carrier, true) || g_iBodyguardCount <= 0)
    {
        return false;
    }

    // 到场锁：一旦先锋已到场交接回包匪，本局后续绝不在此包点反复二次选拔先锋
    if (g_bPointmanArrived)
    {
        return false;
    }

    // 维持现状：同包匪、先锋仍有效、目标仍在
    if (g_iEscortMode == Escort_Pointman
        && g_iEscortCarrier == carrier
        && IsTeamBot(g_iEscortPointman, 2, true)
        && Escort_Find(g_iEscortPointman) != -1
        && g_PointmanObjective.active)
    {
        Escort_ReassertFollows();
        return true;
    }

    float goal[3];
    bool locked = false;
    if (!Escort_ResolveGoal(carrier, goal, locked))
    {
        DebugLog("[BotRouteFix] [Module 7] Bombsite goal unavailable; falling back to carrier escort.");
        return false;
    }

    // 核心规则 2：只有保镖比包匪更靠近目标才当选先锋；包匪自身更近返回 0 回退 CARRIER
    int pointman = Escort_PickPointmanTowardGoal(carrier, goal, 0);
    if (pointman == 0)
    {
        return false;
    }

    g_iEscortMode = Escort_Pointman;
    g_iEscortPointman = pointman;
    g_PointmanObjective.active = true;
    for (int i = 0; i < 3; i++)
    {
        g_PointmanObjective.goal[i] = goal[i];
    }
    g_PointmanObjective.lastIssue = GetGameTime();
    g_bSuperviseScheduled = false;
    g_iEscortGeneration++;

    // 先锋脱离跟随态，直奔目标（MoveTo 不改 m_isFollowing，必须先停跟随）
    BotStopFollowing(pointman);
    BotMoveTo(pointman, goal);

    Escort_ReassertFollows();

    DebugLog("[BotRouteFix] [Module 7] Pointman %N -> (%.0f, %.0f, %.0f) [%s]",
        pointman, goal[0], goal[1], goal[2], locked ? "M2 lock" : "native");
    return true;
}

/**
 * 状态对账：校验当前模式与先锋/名册，维持或迁移
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 只由名册事件（死亡/掉线/裁剪/补员）与监督回调调用；不重算目标，不写时钟。
 */
static void Escort_Reconcile()
{
    if (g_iEscortMode == Escort_None)
    {
        return;
    }

    int carrier = g_iEscortCarrier;
    if (!IsValidClient(carrier, true))
    {
        Escort_ReleaseAll();
        return;
    }

    if (g_iEscortMode == Escort_Carrier)
    {
        Escort_ReassertFollows();
        return;
    }

    // POINTMAN：先锋流失则清目标并晋升；仍有效则只重挂跟随链
    if (!IsTeamBot(g_iEscortPointman, 2, true) || Escort_Find(g_iEscortPointman) == -1)
    {
        g_PointmanObjective.active = false;
        g_iEscortPointman = 0;
        g_iEscortGeneration++;

        if (!IsBot(carrier, true) || g_iBodyguardCount <= 0)
        {
            Escort_SwitchToCarrier(carrier);
            return;
        }

        if (!Escort_SwitchToPointman(carrier))
        {
            Escort_SwitchToCarrier(carrier);
        }
        return;
    }

    Escort_ReassertFollows();
}

/**
 * 当前护送模式名（DebugLog / 遥测用）
 */
static void Escort_ModeName(char[] buffer, int maxlen)
{
    if (g_iEscortMode == Escort_Pointman)
    {
        strcopy(buffer, maxlen, "POINTMAN");
    }
    else if (g_iEscortMode == Escort_Carrier)
    {
        strcopy(buffer, maxlen, "CARRIER");
    }
    else
    {
        strcopy(buffer, maxlen, "NONE");
    }
}

/**
 * 事项 M7 护送花名册的一次配平：超编从远到近停，缺编从近到远补
 *
 * Refresh 与 OnRosterLost 共用；配平本身不读士气，desired 由调用方定。
 */
static void Escort_Rebalance(int carrier, float anchor[3], int desired)
{
    Escort_TrimToTarget(anchor, desired);
    Escort_FillToTarget(carrier, anchor, desired);
}

/**
 * 事项 M7 护送花名册的全量重算：开局 / 换包匪 / 中途捡包 / 热重载 / 重启用
 *
 * 只做 diff，不扫全队；换包匪先解散旧名单；死/飞/转队/变成包匪本人的一律除名。
 * 永不写运包时钟（那是模块 2 的事项）。
 */
static void Escort_Refresh()
{
    Escort_ClampRoster();
    int carrier = GetC4Carrier();
    if (!IsValidClient(carrier, true))
    {
        Escort_ReleaseAll();
        return;
    }

    // 换了包匪：先解散旧名单（只停名单内的人；空名单时 ReleaseAll 本就是 no-op）
    if (g_iEscortCarrier != carrier)
        Escort_ReleaseAll();
    g_iEscortCarrier = carrier;

    int morale = GetEffectiveCarrierMorale(carrier);
    int desired = Escort_DesiredCount(morale);
    g_iEscortTarget = desired;

    float anchor[3];
    GetClientAbsOrigin(carrier, anchor);

    // 名单 pruning：死/飞/转队/变成包匪本人的一律除名（死人不再发 Stop）
    for (int i = g_iBodyguardCount - 1; i >= 0; i--)
    {
        int bot = g_iBodyguardList[i];
        if (bot == carrier || !IsTeamBot(bot, 2, true))
        {
            Escort_RemoveFromRoster(bot);
        }
    }

    // 按 desired 一次配平；模式由包匪类型与目标可用性决定
    Escort_Rebalance(carrier, anchor, desired);

    if (!IsBot(carrier, true) || desired <= 0)
    {
        // 人类包匪或 0 编制：直接走 CARRIER 模式
        Escort_SwitchToCarrier(carrier);
    }
    else if (!Escort_SwitchToPointman(carrier))
    {
        // 无先锋（包匪自己最近）或包点不可达：回退 CARRIER 模式
        Escort_SwitchToCarrier(carrier);
    }

    char moraleStr[16];
    GetMoraleName(morale, moraleStr, sizeof(moraleStr));
    char modeName[12];
    Escort_ModeName(modeName, sizeof(modeName));
    char pointmanStr[64];
    if (IsBot(g_iEscortPointman))
    {
        Format(pointmanStr, sizeof(pointmanStr), "%N", g_iEscortPointman);
    }
    else
    {
        strcopy(pointmanStr, sizeof(pointmanStr), "none");
    }
    DebugLog("[BotRouteFix] [Module 7] Carrier %N (Morale: %s%d %s) roster %d/%d mode=%s pointman: %s",
        carrier, (morale >= 0) ? "+" : "", morale, moraleStr, g_iBodyguardCount, desired,
        modeName, pointmanStr);
}

/**
 * 事项 M7 护送花名册的解散：释放全部保镖（bomb_dropped / 包匪丢失 / 停用）
 *
 * 快照隔离 + 只停存活名单内的人 + 清空花名册；空名单时调用即 no-op。
 */
static void Escort_ReleaseAll()
{
    g_bPointmanArrived = false;
    int count = g_iBodyguardCount;
    int carrier = g_iEscortCarrier;
    int pointman = g_iEscortPointman;

    // 先阻断重入与延迟帧，再发 SDKCall
    g_iEscortMode = Escort_None;
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;
    g_iEscortPointman = 0;
    g_PointmanObjective.active = false;
    g_PointmanObjective.lastIssue = 0.0;
    g_bSuperviseScheduled = false;
    g_iEscortGeneration++;

    if (count <= 0 && carrier == 0 && pointman == 0)
    {
        return;
    }

    int tempList[MAXPLAYERS + 1];
    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        tempList[i] = g_iBodyguardList[i];
    }

    // 释放所有保镖的 Follow 链
    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        BotStopFollowing(tempList[i]);
    }

    // 先锋可能停在 MoveToState：StopFollowing 停不掉，必须 Idle 拉回
    if (IsBot(pointman, true))
    {
        BotIdle(pointman);
    }

    // 包匪若被反向跟随（Bot 包匪），先停跟随再 Idle 拉回原生
    if (IsBot(carrier, true))
    {
        BotStopFollowing(carrier);
        BotIdle(carrier);
    }
}

/**
 * 事项 M7 护送花名册的静默清空：round_start / map_start 用
 *
 * 新回合与换图实体索引重置，不发 SDKCall（旧索引对应的跟随早已随实体失效）。
 */
static void Escort_ClearSilent()
{
    g_bPointmanArrived = false;
    g_iEscortMode = Escort_None;
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;
    g_iEscortPointman = 0;
    g_PointmanObjective.active = false;
    g_PointmanObjective.lastIssue = 0.0;
    g_bSuperviseScheduled = false;
    g_iEscortGeneration++;
}

/**
 * 事项 M7 护送花名册的回合重置入口：Escort_ClearSilent 的对外别名
 */
static void Escort_Reset()
{
    Escort_ClearSilent();
}

/**
 * 事项 M7 护送花名册的包匪身份查询（供 player_death 用）
 *
 * @return              client 是否为当前包匪
 */
static bool Escort_IsCarrier(int client)
{
    return client > 0 && client == g_iEscortCarrier;
}

/**
 * 事项 M7 护送花名册的减员处理：阵亡/掉线除名 + 按当前士气重算编制多退少补
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 失败语义：名单外的人调用即 no-op；包匪无效只除名不重补；
 * 若死者为先锋，清除先锋目标并触发 Escort_Reconcile 晋升新先锋或回退包匪领队。
 *
 * @param bot           牺牲/流失的保镖或先锋
 * @param logIt         是否打补员遥测日志
 */
static void Escort_OnRosterLost(int bot, bool logIt = true)
{
    Escort_ClampRoster();
    if (Escort_Find(bot) == -1)
    {
        return;
    }

    bool wasPointman = (bot == g_iEscortPointman);
    if (wasPointman)
    {
        g_PointmanObjective.active = false;
        g_iEscortPointman = 0;
        g_iEscortGeneration++;
    }

    Escort_RemoveFromRoster(bot);

    int carrier = g_iEscortCarrier;
    if (!IsValidClient(carrier, true))
    {
        return;
    }

    // 人类包匪同样按调用时的 T 均值士气重算编制
    int morale = GetEffectiveCarrierMorale(carrier);
    g_iEscortTarget = Escort_DesiredCount(morale);

    float anchor[3];
    GetClientAbsOrigin(carrier, anchor);

    int before = g_iBodyguardCount;
    Escort_Rebalance(carrier, anchor, g_iEscortTarget);

    if (logIt && g_iBodyguardCount > before)
    {
        DebugLog("[BotRouteFix] [Module 7] Replenished bodyguard %N for carrier %N (%d/%d)",
            g_iBodyguardList[g_iBodyguardCount - 1], carrier, g_iBodyguardCount, g_iEscortTarget);
    }

    // 对账：若先锋阵亡，从保镖中重新选拔先锋（若比包匪近），否则回退包匪领跑；
    // 若普通保镖阵亡，确保新补入的保镖重挂到当前领队的跟随链
    Escort_Reconcile();
}

/**
 * 事项 M7 护送花名册的掉线处理：包匪掉线解散名册；保镖掉线除名+多退少补
 *
 * 属于事项 ITEM_M7_ESCORT。
 * 失败语义：无效 client 即 no-op；包匪掉线当场解散名册；保镖/先锋掉线触发减员回补与对账。
 */
static void Escort_OnClientDisconnect(int client)
{
    if (client <= 0 || client > MaxClients)
    {
        return;
    }

    if (client == g_iEscortCarrier)
    {
        Escort_ReleaseAll();
        return;
    }
    Escort_OnRosterLost(client, false);
}
