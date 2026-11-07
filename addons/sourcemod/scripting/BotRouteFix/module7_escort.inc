#if defined _botroutefix_module7_escort_included
    #endinput
#endif
#define _botroutefix_module7_escort_included

//========================================================================================
// MODULE 7: T C4 CARRIER ESCORT & BODYGUARD ROSTER
//
// 花名册制贴身保镖：按士气配 3-Morale 名保镖 Follow 包匪，绝境抱团、顺风放飞。
//   - 距离一律欧氏 3D 平方（含 Z，跨楼层天然排后）
//   - 全量重算只做 diff；减员按调用时实时士气重算编制，多退少补
//   - 纯事件驱动，无 timer；运包时钟归 module2，本模块永不写时钟
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

// 本模块私有状态：花名册（不出本文件，对外只暴露 Escort_* 与 Module7_*）
static bool g_bModule7_Active = false;   // 业务开启态
static int g_iEscortCarrier = 0;
static int g_iBodyguardList[MAXPLAYERS + 1];
static int g_iBodyguardCount = 0;
static int g_iEscortTarget = 0;

// Meme 灭活补丁：BotFollowMeme::Interpret 入口 ret（64 位 1 字节 C3 / 32 位 3 字节 C2 08 00）
static MemoryPatch g_PatchFollowMeme7;
static int g_FollowMeme7PatchBytes[3];

enum struct EscortCandidate
{
    int client;
    float dist;
}

//========================================================================================
// MODULE 7 ITEM BACKEND: ESCORT ROSTER (Follow/StopFollowing SDKCalls + carrier morale offset)
//========================================================================================

/**
 * 事项后端：保镖花名册事项（ITEM_M7_ESCORT）
 *
 * 校验 Follow/StopFollowing SDKCall 句柄、包匪士气偏移与 meme 灭活补丁三端，
 * 不做任何名单操作。任一后端缺失返回 false，Init 会把护送事项标记为 FAILED。
 * meme 补丁缺席时名册会被原生开局 meme 冲垮，故与 SDKCall 同等对待、不降级。
 *
 * @return              true = 护送事项后端就绪
 */
static bool PrepModule7_EscortBackend()
{
    bool sdkOk = (g_hSDKCall_Follow != null && g_hSDKCall_StopFollowing != null);
    bool moraleOk = (g_iOffset_Morale > 0);
    bool memeOk = PrepModule7_FollowMemeBackend();

    if (!sdkOk)
    {
        LogError("[BotRouteFix] [Module 7] Follow SDKCall missing; escort item FAILED.");
    }
    if (!moraleOk)
    {
        LogError("[BotRouteFix] [Module 7] Morale offset missing; escort item FAILED.");
    }

    if (sdkOk && moraleOk && memeOk)
    {
        LogMessage("[BotRouteFix] [Module 7] Carrier escort backend verified (follow SDKCall, morale=%d, meme ret).",
            g_iOffset_Morale);
    }
    return (sdkOk && moraleOk && memeOk);
}

/**
 * 事项后端：BotFollowMeme::Interpret 入口 ret 灭活
 *
 * 只定位/校验/备份并把构造好的 ret 字节写入缓冲，不写代码段。
 * 补丁语义：meme 解释器入口直接返回，原生开局 "Going to plant the bomb"
 * 全员盲从链（1000 码内 receiver->Follow(sender) + CoveringFriend 应答）
 * 物理灭活；M7 自己的 BotFollow SDKCall 走直调，不受影响。
 * 架构差异：64 位调用方清栈用 1 字节 C3；32 位 thiscall 被调方清 8 字节
 * 参数用 3 字节 C2 08 00——裸 C3 会坏栈，两种架构不可混用。
 *
 * @return              true = 灭活补丁后端就绪
 */
static bool PrepModule7_FollowMemeBackend()
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
 * 初始化模块 7（校验 Follow 调用就绪）
 */
bool InitModule7_Escort()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 7] GameConf missing; escort item FAILED.");
        Item_SetReady(ITEM_M7_ESCORT, false);
        return false;
    }

    Item_SetReady(ITEM_M7_ESCORT, PrepModule7_EscortBackend());
    EnableModule7_Escort(true);

    LogModuleArmed(7, "Carrier escort (Euclidean bodyguards)",
        Item_IsArmed(ITEM_M7_ESCORT) ? "1" : "0");
    return g_bModule7_Active;
}

/**
 * 动态启用或停用模块 7（幂等；停用即解除全队跟随）
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
 * 还原模块 7（插件卸载：解除跟随链并复位业务态）
 */
void RestoreModule7_Escort()
{
    EnableModule7_Escort(false);
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
        if (IsValidClient(bot, true))
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
 */
void Module7_OnBombDropped()
{
    DebugLog("[BotRouteFix] [Module 7] C4 dropped! Disbanding bodyguard roster (%d guards released)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 安放事件 (bomb_planted)：释放全体保镖跟随，立即转入模块 6 守点与掩体架枪任务 (GUARD_TICKING_BOMB / HideState)
 */
void Module7_OnBombPlanted()
{
    DebugLog("[BotRouteFix] [Module 7] C4 planted! Disbanding bodyguard roster (%d guards released to site defense)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 拆除/爆炸事件：释放名单，确保无残留跟随
 */
void Module7_OnBombDefused()
{
    Escort_ReleaseAll();
}

/**
 * 事项 M7 护送花名册的 C4 爆炸处理：释放名单，确保无残留跟随
 *
 * 与 Defused 同策：爆炸后保镖无保护目标，全部恢复自由身。
 */
void Module7_OnBombExploded()
{
    Escort_ReleaseAll();
}

/**
 * 玩家死亡事件 (player_death)
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
// MODULE 7 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用护送事项：先落 meme 灭活补丁，再武装并重配名单
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
        || g_hSDKCall_Follow == null || g_hSDKCall_StopFollowing == null)
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

    Item_SetArmed(ITEM_M7_ESCORT, true);
    Escort_Refresh();
    return true;
}

/**
 * 还原护送事项：先恢复 meme 解释器字节，再解除武装并解散名单（停用即全员放行回原生）
 */
static void RestoreModule7_EscortBackend()
{
    g_PatchFollowMeme7.Restore();
    Item_SetArmed(ITEM_M7_ESCORT, false);
    Escort_ReleaseAll();
}

//========================================================================================
// MODULE 7 ROSTER PRIMITIVES
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
    if (!IsValidClient(bot, false))
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
static float Escort_DistSq(float a[3], float b[3])
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
        BotFollow(picked[k], carrier);
        g_iBodyguardList[g_iBodyguardCount++] = picked[k];
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

    // 按 desired 一次配平
    Escort_Rebalance(carrier, anchor, desired);

    char moraleStr[16];
    GetMoraleName(morale, moraleStr, sizeof(moraleStr));
    DebugLog("[BotRouteFix] [Module 7] Carrier %N (Morale: %s%d %s) roster %d/%d",
        carrier, (morale >= 0) ? "+" : "", morale, moraleStr, g_iBodyguardCount, desired);
}

/**
 * 事项 M7 护送花名册的解散：释放全部保镖（bomb_dropped / 包匪丢失 / 停用）
 *
 * 快照隔离 + 只停存活名单内的人 + 清空花名册；空名单时调用即 no-op。
 */
static void Escort_ReleaseAll()
{
    int count = g_iBodyguardCount;
    if (count <= 0)
    {
        g_iBodyguardCount = 0;
        g_iEscortCarrier = 0;
        g_iEscortTarget = 0;
        return;
    }

    int tempList[MAXPLAYERS + 1];
    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        tempList[i] = g_iBodyguardList[i];
    }

    // 立即清零全局计数与包匪记录，阻断重入
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;

    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        int bot = tempList[i];
        BotStopFollowing(bot);
    }
}

/**
 * 事项 M7 护送花名册的静默清空：round_start / map_start 用
 *
 * 新回合与换图实体索引重置，不发 SDKCall（旧索引对应的跟随早已随实体失效）。
 */
static void Escort_ClearSilent()
{
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;
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
 * 名单外的人调用即 no-op；包匪无效（没包匪）只除名不重补。
 */
static void Escort_OnRosterLost(int bot, bool logIt = true)
{
    Escort_ClampRoster();
    if (Escort_Find(bot) == -1)
        return;

    Escort_RemoveFromRoster(bot);

    int carrier = g_iEscortCarrier;
    if (!IsValidClient(carrier, true))
        return;

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
}

/**
 * 事项 M7 护送花名册的掉线处理：包匪掉线解散名册；保镖掉线除名+多退少补
 *
 * 掉包事件随后接管残局；保镖掉线走减员回补。
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
