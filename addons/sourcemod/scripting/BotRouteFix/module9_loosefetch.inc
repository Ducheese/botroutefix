#if defined _botroutefix_module9_loosefetch_included
    #endinput
#endif
#define _botroutefix_module9_loosefetch_included

//========================================================================================
// MODULE 9: LOOSE-BOMB TOP3 FETCH ROSTER + OPPO GUARD
// (NoticeLooseBomb pre-hook / CCSBot::SetState post-hook / AttackState::OnUpdate pre-hook)
//
// 业务背景与引擎逆向分析：
//
// 1. 机制分析 (cs_bot.cpp:428-444):
//    NoticeLooseBomb() = GetLooseBomb() != NULL 即返回 true，注释写 "for T's only"
//    原生虽在现有调用点做了阵营包裹，但 NoticeLooseBomb 查询原语本身缺少阵营限制。
//    两个调用点（IdleState:228、HuntState:74）。
// 2. 卡门：只钩这一个查询函数，两条路全堵；本钩顺带补齐阵营门控作为纵深防御兜底：CT 一律 Supercede + false。
//    CT 剩余感知（视力 CanSeeLooseBomb、抢包 1000 听觉）均在其它函数，不受影响。
// 3. 花名册（与模块 7 同构，全事件驱动，无 timer）：
//    - bomb_dropped：递延一帧（等归属落定）后以掉包点（无主 weapon_c4 实体坐标，零耦合不读模块 7）为圆心，
//      真实地面行走距离（BotNavWalkDistance 无损实测）取最近 3 存活 T bot，不可达跳过；
//      当选/回补时强制解蹲一次：task 19 蹲守承诺者必踢（蹲坑 Hide 里到不了 NoticeLooseBomb，
//      Idle 的 loose 检查又排在狙击手守点/架枪掷骰之后，不踢就瘫痪），外加一切正处 HideState 者
//      无论 task 皆踢（残血 Hide、换弹 Hide、跟丢 Ambush Hide 等——Hide 不改 task，task 判不全，
//      真值以 m_state 虚表指纹为准，见 Fetch_IsHiding）；Hunt 落 SEEK_AND_DESTROY，M12 不认；
//      选举失败（找不到掉包）或测距未就绪 → 空名册 = 全体放行回原生；
//    - player_death / client_disconnect：除名 + 按掉包点就近回补（共用同一函数；名单有效时 C4 必躺掉包点，
//      按掉包点招人即按 C4 位置招人）——必选项，否则减员到全灭即全员 false，包烂地上没人捡（比原版更糟的死锁）；
//    - item_pickup(c4) / round_start / OnMapStart：清（FetchBombState:58 有人抢包自己散伙；换图重置）。
// 4. 空名册放行原则：钩子只会把 true 掐成 false，且仅在名册有效时掐；名册空即原生 no-op。
// 全程事件驱动，无 timer 轮询。
// 5. 名册抢包 OPPO 守卫（SetState Post + AttackState OnUpdate 双面 + 选举/递补即时兜底，事项 ITEM_M9_FETCH 第二部分）：
//    对当前在册成员（不再卡 m_state == FetchBombState），只要当前非 OPPO（仍为 ENGAGE），
//    直接改写 m_disposition 为 OPPORTUNITY_FIRE，消除 Hunt/MoveTo/Follow 等在途与非退避 Hide 途中的
//    听声分心与 0.1s 脱视 Ambush Hide 蹲死；不经 CCSBot::SetDisposition，完整保留 M5 后撤与
//    近敌换弹携带的 m_ignoreEnemiesTimer（GetDisposition() 优先返回 IGNORE_ENEMIES，到期自然落回 OPPO）。
//    AttackState 战斗面封堵：引擎 Attack() 并发直入 m_attackState.OnEnter（不走 SetState，
//    cs_bot_statemachine.cpp），交火全程 SetState Post 面零执行；模块中途武装或名册在途
//    带 ENGAGE 接战时由此面就地补写，不必等下一次状态迁移。
//    只动抢包，不管下包（下包是 M6 的事）不管 GUARD；补写计入遥测。
//========================================================================================

//========================================================================================
// MODULE 9 DEFINES & GLOBALS
//========================================================================================

#define FETCH_ROSTER_SIZE 3

// 蹲守承诺任务（与 module12 的 BOT_TASK_MOVE_TO_SNIPER_SPOT 同值 19；module9 先于
// module12 被 include，不能复用它的宏，只能本地再定一遍，改值时两处同步）
#define FETCH_CAMP_TASK 19

// 抢包 OPPO 守卫值（与 module6 的 DISP_OPPORTUNITY 同值（同为 1）：同编译单元
// 重复 #define 会报错，两边都用 #if !defined 守卫，改值时两处同步）
#if !defined DISP_OPPORTUNITY
#define DISP_OPPORTUNITY          1       // OPPORTUNITY_FIRE
#endif

// 本模块私有状态（static，名册与句柄不出本文件）
static bool g_bModule9_Active = false;      // 业务开启态
static bool g_bLooseHooked = false;         // per-hook 真实启用态
static bool g_bFetchGuardHooked = false;    // per-hook 真实启用态（SetState Post）
static bool g_bAttackFetchGuardHooked = false;  // per-hook 真实启用态（AttackState OnUpdate Pre）
static Handle g_hLooseDetour = null;
static Handle g_hFetchGuardDetour = null;
static Handle g_hAttackFetchGuardDetour = null; // AttackState::OnUpdate 交火期兜底载体
static int g_iFetchRoster[FETCH_ROSTER_SIZE] = {-1, -1, -1};
static bool g_bFetchHasRoster = false;
static float g_fDropPos[3];

// 遥测统计计数器
static int g_iTotalFetchGated = 0;
static int g_iTotalFetchElections = 0;
static int g_iTotalFetchGuarded = 0;

//========================================================================================
// MODULE 9 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：掉包抢包花名册 + 名册抢包 OPPO（ITEM_M9_FETCH）
 *
 * 只创建 Detour、绑定签名、校验测距子系统、探测守卫偏移与直调后端，
 * 不启用钩子（启用由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把抢包事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 抢包事项后端就绪
 */
static bool PrepModule9_FetchBackend()
{
    g_hLooseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hLooseDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb detour creation failed; M9 fetch roster + OPPO guard item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hLooseDetour, g_hGameConf, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb signature missing; M9 fetch roster + OPPO guard item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    if (!BotNavReady())
    {
        LogError("[BotRouteFix] [Module 9] NavMeasure unavailable; M9 fetch roster + OPPO guard item FAILED (no fail-open roster).");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }
    // NoticeLooseBomb(void)：无参数，this 即 bot 本体

    // 守卫与解蹲前置探测：state/disposition 偏移缺一即事项 FAILED（Enforce 直写 m_disposition，不走 SetDisposition）
    //（Enforce 运行时的同条件自守是纵深防御，不代替此处的一次性探测结论）
    if (g_iOffset_State <= 0 || g_iOffset_Disposition <= 0)
    {
        LogError("[BotRouteFix] [Module 9] State/disposition offsets missing; M9 fetch roster + OPPO guard item FAILED.");
        if (g_hLooseDetour != null)
        {
            delete g_hLooseDetour;
            g_hLooseDetour = null;
        }
        return false;
    }

    if (!MountFetchGuardDetour())
    {
        LogError("[BotRouteFix] [Module 9] SetState hook unavailable; M9 fetch roster + OPPO guard item FAILED.");
        if (g_hLooseDetour != null)
        {
            delete g_hLooseDetour;
            g_hLooseDetour = null;
        }
        return false;
    }

    if (!MountAttackFetchGuardDetour())
    {
        LogError("[BotRouteFix] [Module 9] AttackState hook unavailable; M9 fetch roster + OPPO guard item FAILED.");
        if (g_hLooseDetour != null)
        {
            delete g_hLooseDetour;
            g_hLooseDetour = null;
        }
        if (g_hFetchGuardDetour != null)
        {
            delete g_hFetchGuardDetour;
            g_hFetchGuardDetour = null;
        }
        return false;
    }

    return true;
}

/**
 * 挂载 CCSBot::SetState Post Detour（事项 ITEM_M9_FETCH 的抢包守卫触发点）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，事项 ITEM_M9_FETCH（M9 fetch roster + OPPO guard）FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountFetchGuardDetour()
{
    g_hFetchGuardDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hFetchGuardDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] Failed to create DHook detour instance for CCSBot_SetState!");
        return false;
    }

    if (!DHookSetFromConf(g_hFetchGuardDetour, g_hGameConf, SDKConf_Signature, "CCSBot_SetState"))
    {
        LogError("[BotRouteFix] [Module 9] Failed to set CCSBot_SetState signature from gamedata!");
        delete g_hFetchGuardDetour;
        g_hFetchGuardDetour = null;
        return false;
    }

    // SetState(BotState*) 有一个指针参数；漏加参数会让 DHook trampoline 崩溃（c0000005）
    DHookAddParam(g_hFetchGuardDetour, HookParamType_Int);
    return true;
}

/**
 * 挂载 AttackState::OnUpdate Pre Detour（事项 ITEM_M9_FETCH 的抢包守卫交火期兜底载体）
 *
 * 引擎 Attack() 并发直入 m_attackState.OnEnter（不走 SetState，cs_bot_statemachine.cpp），
 * 交火全程 SetState Post 面零执行；此钩子在攻击 tick 内补位，模块中途武装或名册成员
 * 在途（Hunt/MoveTo 等）带 ENGAGE 接战时就地补写 OPPO，掐死脱视 0.1s 的 Ambush Hide 蹲死。
 * 与 M5 后撤、M6 下包守卫的 AttackState_OnUpdate 同目标同签名（gamedata 共条目），
 * detour 独立互不干扰（与 M6/M9 双 SetState 钩子同惯例）。
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，事项 ITEM_M9_FETCH FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountAttackFetchGuardDetour()
{
    g_hAttackFetchGuardDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hAttackFetchGuardDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] Failed to create DHook detour instance for AttackState_OnUpdate!");
        return false;
    }

    if (!DHookSetFromConf(g_hAttackFetchGuardDetour, g_hGameConf, SDKConf_Signature, "AttackState_OnUpdate"))
    {
        LogError("[BotRouteFix] [Module 9] Failed to set AttackState_OnUpdate signature from gamedata!");
        delete g_hAttackFetchGuardDetour;
        g_hAttackFetchGuardDetour = null;
        return false;
    }

    // AttackState::OnUpdate(CCSBot *me) 的 me 以 CBaseEntity 参数直取客户端索引
    DHookAddParam(g_hAttackFetchGuardDetour, HookParamType_CBaseEntity);
    return true;
}

//========================================================================================
// MODULE 9 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 9 的唯一事项：掉包抢包花名册（ITEM_M9_FETCH），挂 NoticeLooseBomb Pre + SetState Post + AttackState OnUpdate Pre。
 * 签名双架构现成，缺失即 fail-fast（与待机模块不同：这里缺 = gamedata 坏了）。
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 */
bool InitModule9_Fetch()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 9] GameConf missing; M9 fetch roster + OPPO guard item FAILED.");
        Item_SetReady(ITEM_M9_FETCH, false);
        return false;
    }

    Item_SetReady(ITEM_M9_FETCH, PrepModule9_FetchBackend());
    EnableModule9_Fetch(true);
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        LogModuleAllFailed(9);
        return false;
    }

    LogModuleArmed(9, "Fetch", "1");
    return true;
}

/**
 * 动态启用或停用模块 9（事项 ITEM_M9_FETCH 的调度入口；幂等；停用即清空名册 = 全放行回原生）。
 *
 * 只做调度：只武装 Ready 的事项；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 */
void EnableModule9_Fetch(bool enable)
{
    if (enable == g_bModule9_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule9_FetchBackend();
        g_bModule9_Active = false;
        // 借钩宿主：Hide 虚表指纹依赖模块 15 的 OnEnter 钩子，本模块关闭也要重算宿主意愿
        SyncHideDurationDetourState();
        return;
    }

    g_bModule9_Active = ApplyModule9_FetchBackend();
    // 借钩宿主：武装后确保 OnEnter 宿主钩子处于启用态（虚表捕获的前提）
    SyncHideDurationDetourState();
}

/**
 * 还原模块 9（事项 ITEM_M9_FETCH；插件卸载：摘钩并释放句柄）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule9_Fetch()
{
    EnableModule9_Fetch(false);

    if (g_hLooseDetour != null)
    {
        delete g_hLooseDetour;
        g_hLooseDetour = null;
    }

    if (g_hFetchGuardDetour != null)
    {
        delete g_hFetchGuardDetour;
        g_hFetchGuardDetour = null;
    }

    if (g_hAttackFetchGuardDetour != null)
    {
        delete g_hAttackFetchGuardDetour;
        g_hAttackFetchGuardDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 9] Fetch hooks safely restored.");
}

//========================================================================================
// MODULE 9 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M9 抢包花名册的名单查询（模块 5 后撤档位裁决复用）
 *
 * 无效索引（client <= 0）直接返回 false；纯内存判断。
 */
bool Module9_IsInRoster(int client)
{
    if (client <= 0)
    {
        return false;
    }

    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] == client)
        {
            return true;
        }
    }
    return false;
}

/**
 * 获取当前名册有效人数
 */
int Module9_GetRosterCount()
{
    int count = 0;
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] > 0 && IsTeamBot(g_iFetchRoster[i], 2, true))
        {
            count++;
        }
    }
    return count;
}

/**
 * 获取抢包花名册容量上限
 */
int Module9_GetRosterSize()
{
    return FETCH_ROSTER_SIZE;
}

/**
 * 获取累计选举次数
 */
int Module9_GetTotalElections()
{
    return g_iTotalFetchElections;
}

/**
 * 获取累计拦截非名册哄抢次数
 */
int Module9_GetTotalGated()
{
    return g_iTotalFetchGated;
}

/**
 * 查询累计为名册抢包人补写的 OPPO 次数
 *
 * 跨模块只读 getter，不参与事项状态。
 */
int Module9_GetTotalFetchGuarded()
{
    return g_iTotalFetchGuarded;
}

/**
 * 获取当前抢包花名册 Bot 列表与掉落 C4 坐标 (供可视化子系统渲染牵引光束)
 *
 * @param roster        输出候选人 client 索引数组 (容量 FETCH_ROSTER_SIZE)
 * @param c4Pos         输出掉落 C4 的世界坐标
 * @return              实际有效候选人数量
 */
int Module9_GetFetchRoster(int roster[FETCH_ROSTER_SIZE], float c4Pos[3])
{
    if (!g_bFetchHasRoster)
    {
        return 0;
    }

    if (!Fetch_FindDropPos(c4Pos))
    {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        int bot = g_iFetchRoster[i];
        if (IsValidClient(bot, true))
        {
            roster[count++] = bot;
        }
    }
    return count;
}

//========================================================================================
// MODULE 9 EVENT HANDLERS
//========================================================================================

/**
 * 掉包事件：递延一帧再选举（等 C4 归属落定，模块 7 FrameRefresh 同款；直接查必 miss）
 */
void Module9_OnBombDropped()
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    RequestFrame(Module9_FrameElect);
}

/**
 * 递延选举帧（RequestFrame 回调）
 */
public void Module9_FrameElect(any data)
{
    // 递延帧可能在停用之后才到，必须二次校验业务态
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    Fetch_ElectRoster();
}

/**
 * 死亡事件：成员阵亡递补下家（防死锁必选项）
 */
void Module9_OnPlayerDeath(int victim)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    Fetch_RemoveAndBackfill(victim, "fallen member");
}

/**
 * 客户端掉线 (OnClientDisconnect)：无死亡事件，槽位会烂掉——同递补逻辑
 */
void Module9_OnClientDisconnect(int client)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    Fetch_RemoveAndBackfill(client, "disconnect");
}

/**
 * 抢包事件：有人捡起 C4 即解散（FetchBombState 自己散伙）
 */
void Module9_OnItemPickup(int client, const char[] item)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    if (!g_bFetchHasRoster)
    {
        return;
    }

    // 冻结期系统发包不触发（与 module2/6/7 同守卫；此时名册恒空，本是 no-op，防后人加逻辑踩坑）
    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Bomb picked up by %N, roster disbanded", client);
    }
}

/**
 * 回合开始：清名册
 */
void Module9_OnRoundStart()
{
    Fetch_ClearRoster();
}

/**
 * 地图加载 (OnMapStart)：名单索引随实体全废，必须清（round_start 也会清，显式正确）
 */
void Module9_OnMapStart()
{
    Fetch_ClearRoster();
}

//========================================================================================
// MODULE 9 ROSTER PRIMITIVES
//========================================================================================

/**
 * 事项 M9 抢包花名册的清空：no-op 放行（无名册 = 全体放行回原生）
 *
 * 停用 / 抢包 / 回合重置 / 换图共用；只清名单不动钩子。
 */
static void Fetch_ClearRoster()
{
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        g_iFetchRoster[i] = -1;
    }
    g_bFetchHasRoster = false;
}

/**
 * 事项 M9 抢包花名册的掉包定位：找无主掉包（m_hOwnerEntity == -1 的 weapon_c4）
 *
 * 找不到返回 false，调用方按 no-op 回原生放行（不等、不猜）。
 */
static bool Fetch_FindDropPos(float dropPos[3])
{
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_c4")) != -1)
    {
        if (GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") == -1)
        {
            GetEntPropVector(ent, Prop_Send, "m_vecOrigin", dropPos);
            return true;
        }
    }
    return false;
}

/**
 * 事项 M9 抢包花名册的蹲守承诺判定：task == 19 即视为蹲坑中
 *
 * task 是任务不是姿态：狙击手进 Hide 前 task 先落 19，所以这里能抓到"还没进 Hide 的蹲守承诺"
 * （MOVE_TO_SNIPER_SPOT，M12 自己也认这个值）；已经进 Hide 的不限 task 者由 Fetch_IsHiding 兜底。
 * 偏移缺失/实体无效一律按"非蹲守"处理（宁可漏踢，不错杀）。
 *
 * @param bot           客户端索引
 * @return              true = 正蹲守承诺中，抢包指望不上它
 */
static bool Fetch_IsCampCommitted(int bot)
{
    if (g_iOffset_Task <= 0)
    {
        return false;
    }

    Address pBot = GetEntityAddress(bot);
    if (pBot == Address_Null)
    {
        return false;
    }

    return LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == FETCH_CAMP_TASK;
}

/**
 * 事项 M9 抢包花名册的 Hide 真值判定：m_state 虚表指纹比对
 *
 * 不限 task——残血 Hide、换弹 Hide、跟丢 Ambush Hide、狙击蹲坑 Hide 全中。
 * 指纹由 M15 共享 HideState::OnEnter 钩子首次目击时捕获（g_pHideStateVTable）；
 * 捕获前返回 false 物理上恒成立：无任何 Hide 进入即无任何存活 Bot 处 Hide，无漏判。
 * M9 是该宿主钩子的借钩方（Enable/Disable 时同步 SyncHideDurationDetourState），
 * 即使 M12/M15 关闭宿主也不会被卸载。
 * m_state 恒指有效 BotState 对象，读虚表安全；偏移缺失/实体无效/哨兵按"非 Hide"处理。
 *
 * @param bot           客户端索引
 * @return              true = 正处 HideState，抢包指望不上它
 */
static bool Fetch_IsHiding(int bot)
{
    if (g_pHideStateVTable == Address_Null || g_iOffset_State <= 0)
    {
        return false;
    }

    Address pBot = GetEntityAddress(bot);
    if (!IsValidAddress(pBot))
    {
        return false;
    }

    Address state = LoadAddressFromAddress(pBot + view_as<Address>(g_iOffset_State));
    if (!IsValidAddress(state))
    {
        return false;
    }

    return LoadAddressFromAddress(state) == g_pHideStateVTable;
}

/**
 * 事项 M9 抢包花名册的蹲守者解蹲：对名册里 task 19 的，一人一次强制 Hunt
 *
 * 蹲坑者 Hide 里永远到不了 NoticeLooseBomb（Idle 的 loose 检查排在狙击手
 * 守点/架枪掷骰之后，M12 再把 task 19 的 Hide 留守），不踢出来名册就是摆设。
 * Hunt 进去 task 落 SEEK_AND_DESTROY，M12 不认（只认 19），交接干净；
 * Hunt SDKCall 缺失则跳过（名册照转被动门禁，不拓宽 FAILED 面）。
 * 调用点保证帧上下文（选举/回补都在 RequestFrame 或事件里，不在钩子里），直调安全。
 *
 * @param bot           名册成员客户端索引
 * @return              true = 已发起强制 Hunt（调用方以此跳过 Hide 复检，避免连踢两次）
 */
static bool Fetch_ForceHuntIfCamping(int bot)
{
    if (!Fetch_IsCampCommitted(bot))
    {
        return false;
    }

    if (BotHunt(bot))
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N was camping (task 19), forced Hunt to fetch", bot);
    }
    else
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N camping but Hunt SDKCall unavailable, left passive", bot);
    }
    return true;
}

/**
 * 事项 M9 抢包花名册的 Hide 解蹲：对名册里一切正处 HideState 者强制 Hunt
 *
 * task 19 快道抓不到的（残血 Hide、换弹 Hide、跟丢 Ambush Hide……）全在这里兜底：
 * Hide 里到不了 NoticeLooseBomb（Idle/Hunt 的 loose 检查都在各自状态流里，Hide 自成一国），
 * 不踢出来名册就是摆设。未处 Hide 时 no-op（捕获前返回 false 物理恒成立，无漏判）。
 *
 * @param bot           名册成员客户端索引
 */
static void Fetch_ForceHuntIfHiding(int bot)
{
    if (!Fetch_IsHiding(bot))
    {
        return;
    }

    if (BotHunt(bot))
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N was hiding (any task), forced Hunt to fetch", bot);
    }
    else
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N hiding but Hunt SDKCall unavailable, left passive", bot);
    }
}

/**
 * 事项 M9 抢包花名册的选举：掉包点真实地面行走距离最近 3 存活 T bot
 *
 * 测距未就绪 / 找不到掉包 / 无存活 T bot 即空名册回原生（不拿欧氏混分，不可达跳过）。
 * 选完强制解蹲：task 19 蹲守承诺先踢（快道，含尚未进 Hide 者），一切正处 Hide 者再踢（虚表真值）。
 * 踢过一次的不踢第二次。
 */
static void Fetch_ElectRoster()
{
    Fetch_ClearRoster();

    float dropPos[3];
    if (!Fetch_FindDropPos(dropPos))
    {
        DebugLog("[BotRouteFix] [Module 9] Bomb dropped but loose C4 not found, no-op to native");
        return;
    }
    g_fDropPos = dropPos;

    // 测距未就绪：无名单，全员放行回原生（不拿欧氏混分）
    if (!BotNavReady())
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] NavMeasure standby, no-op to native");
        return;
    }

    float bestDist[FETCH_ROSTER_SIZE];
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        bestDist[i] = 9999999.0;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true))
        {
            continue;
        }

        // 真实地面行走距离选人（ROUTE_SAFEST 贴合原生捡包避险路线）；不可达跳过（跨楼层够不着的不选）
        float dist = BotNavWalkDistance(i, dropPos, ROUTE_SAFEST);
        if (dist < 0.0)
        {
            continue;
        }

        for (int slot = 0; slot < FETCH_ROSTER_SIZE; slot++)
        {
            if (dist < bestDist[slot])
            {
                for (int shift = FETCH_ROSTER_SIZE - 1; shift > slot; shift--)
                {
                    g_iFetchRoster[shift] = g_iFetchRoster[shift - 1];
                    bestDist[shift] = bestDist[shift - 1];
                }
                g_iFetchRoster[slot] = i;
                bestDist[slot] = dist;
                break;
            }
        }
    }

    if (g_iFetchRoster[0] == -1)
    {
        DebugLog("[BotRouteFix] [Module 9] No alive T bots to elect, no-op to native");
        return;
    }

    g_bFetchHasRoster = true;
    g_iTotalFetchElections++;
    DebugLog("[BotRouteFix] [Module 9] Elected fetch roster: %N / %N / %N",
        g_iFetchRoster[0],
        (g_iFetchRoster[1] != -1) ? g_iFetchRoster[1] : g_iFetchRoster[0],
        (g_iFetchRoster[2] != -1) ? g_iFetchRoster[2] : g_iFetchRoster[0]);

    // 蹲守者解蹲 + 立即切换名册 OPPO（未触发 BotHunt 者同样即刻从 ENGAGE 收拢为 OPPORTUNITY_FIRE）
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] != -1)
        {
            if (!Fetch_ForceHuntIfCamping(g_iFetchRoster[i]))
            {
                Fetch_ForceHuntIfHiding(g_iFetchRoster[i]);
            }
            Module9_EnforceFetchDisposition(g_iFetchRoster[i]);
        }
    }
}

/**
 * 除名 + 按掉包点就近回补 + 全灭解散放行（死亡与掉线共用）
 * 名单有效时 C4 必躺在掉包点（谁捡起名单即散），按掉包点招人即按 C4 位置招人
 */
static void Fetch_RemoveAndBackfill(int client, const char[] reason)
{
    if (!g_bFetchHasRoster || !Module9_IsInRoster(client))
    {
        return;
    }

    // 腾出槽位
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] == client)
        {
            g_iFetchRoster[i] = -1;
        }
    }

    int best = -1;
    float bestDist = 9999999.0;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true) || Module9_IsInRoster(i))
        {
            continue;
        }

        float dist = BotNavWalkDistance(i, g_fDropPos, ROUTE_SAFEST);
        if (dist < 0.0)
        {
            continue;
        }
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }

    if (best != -1)
    {
        for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
        {
            if (g_iFetchRoster[i] == -1)
            {
                g_iFetchRoster[i] = best;
                break;
            }
        }
        g_iTotalFetchElections++;
        DebugLog("[BotRouteFix] [Module 9] Roster backfill (%s): %N joins", reason, best);
        if (!Fetch_ForceHuntIfCamping(best))
        {
            Fetch_ForceHuntIfHiding(best);
        }
        Module9_EnforceFetchDisposition(best);
    }

    // 全灭则解散回原生（no-op 放行）
    if (Module9_GetRosterCount() == 0)
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Roster wiped, no-op to native");
    }
}

//========================================================================================
// MODULE 9 DETOUR & HOOKS (NOTICELOOSEBOMB::PRE / SETSTATE::POST / ATTACKSTATE::ONUPDATE::PRE)
//========================================================================================

/**
 * 事项 M9 抢包花名册的唯一入口钩子（NoticeLooseBomb Pre）
 *
 * ITEM_M9_FETCH 武装且名册有效时才掐非名册者；只会把 true 掐成 false，
 * 且 CT 一律 Supercede（阵营门补漏）；名册空即全体放行回原生。
 * 失败语义：回调内不判定事项成败；Supercede 仅用于强制 false（非名册者/CT 阵营门），其余一律 MRES_Ignored。
 *
 * 契约注记：底层 DHooks 在 0 参数 non-void 钩子下槽 2 实推 rHndl（DHookSetReturn 内部双认
 * g_HookReturnHandle 与 g_HookParamsHandle），此处保留 hParams 惯用标签防误调 DHookGetReturn，勿再改名。
 */
public MRESReturn Hook_NoticeLooseBomb_Pre(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    // 阵营门补漏：原生 NoticeLooseBomb 注释写 "for T's only"（虽底层调用点均有阵营包裹，此处作为纵深防御兜底）
    if (!IsTeamBot(client, 2, true))
    {
        DHookSetReturn(hParams, false);
        return MRES_Supercede;
    }

    if (!g_bFetchHasRoster || Module9_IsInRoster(client))
    {
        return MRES_Ignored;
    }

    DHookSetReturn(hParams, false);
    g_iTotalFetchGated++;
    // DebugLog("[BotRouteFix] [Module 9] Bot %N denied loose-bomb fetch (off-roster)", client);
    return MRES_Supercede;
}

/**
 * 事项 ITEM_M9_FETCH（M9 fetch roster + OPPO guard）的名册抢包质量守卫原语
 *
 * 按名册身份（Module9_IsInRoster）统一守护，不卡状态机指针、也不排除 HideState：
 * 只要当前 m_disposition 仍为 ENGAGE，直接内存覆写为 DISP_OPPORTUNITY（1），
 * 不经 CCSBot::SetDisposition，确保 M5 后撤与近敌换弹携带的 m_ignoreEnemiesTimer
 * （及队友阵亡 1s 切路保护）完全不被提前清零；计时到期自然落回 OPPO，
 * 同时顺带兜住 Follow 停步/电台 Hold 等无 IgnoreEnemies 进 Hide 的在途段。
 * 只动抢包，不管下包不管 GUARD。
 *
 * 失败语义：本函数只做守卫补写、不判定事项成败；非名册/偏移缺失时直接返回。
 */
static void Module9_EnforceFetchDisposition(int client)
{
    if (!IsBot(client, true) || !Module9_IsInRoster(client))
    {
        return;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return;
    }

    if (g_iOffset_Disposition <= 0)
    {
        return;
    }

    int disposition = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Disposition), NumberType_Int32);
    if (disposition != DISP_ENGAGE)
    {
        return;
    }

    // 直写 m_disposition，不动 m_ignoreEnemiesTimer（cs_bot.cpp:685-704）
    StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), DISP_OPPORTUNITY, NumberType_Int32);
    g_iTotalFetchGuarded++;
    DebugLog("[BotRouteFix] [Module 9] Bot %N fetch disposition -> OPPORTUNITY_FIRE", client);
}

/**
 * 事项 ITEM_M9_FETCH（M9 fetch roster + OPPO guard）的 CCSBot::SetState Post 钩子
 * （与 M6 的同目标钩子以后缀区分，两个 Post 回调都会被 DHooks 依次调用）。
 *
 * OnEnter/OnExit 的 disposition 重置发生在 SetState 内部，Post 在这里统一覆盖。
 * 只处理名册成员，其余一律放行。名册按槽位直查，比全客户端扫描便宜。
 *
 * 失败语义：回调内不判定事项成败；未武装时首行自守，直接返回 MRES_Ignored。
 */
public MRESReturn Hook_SetState_Post_Fetch(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M9_FETCH) || pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        int bot = g_iFetchRoster[i];
        if (bot != -1 && IsClientInGame(bot) && GetEntityAddress(bot) == pThis)
        {
            Module9_EnforceFetchDisposition(bot);
            break;
        }
    }
    return MRES_Ignored;
}

/**
 * 事项 ITEM_M9_FETCH（M9 fetch roster + OPPO guard）的 AttackState::OnUpdate Pre 钩子
 * （与 M5 后撤、M6 下包守卫的同目标钩子以后缀区分，各自 detour 独立回调）。
 *
 * 引擎 Attack() 并发直入 m_attackState.OnEnter（不经 SetState），交火全程
 * SetState Post 面零执行；此面在攻击 tick 内兜底，名册成员无论处 FetchBombState
 * 或在途状态交战时皆就地补写 OPPO，掐死脱视 0.1s 的 Ambush Hide 蹲死。
 * 业务判定全部下沉 EnforceFetchDisposition（名册/现行值双重自守）。
 *
 * 失败语义：回调内不判定事项成败；未武装时首行自守，直接返回 MRES_Ignored。
 */
public MRESReturn Hook_AttackState_OnUpdate_Pre_Fetch(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    Module9_EnforceFetchDisposition(client);
    return MRES_Ignored;
}

//========================================================================================
// MODULE 9 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用抢包事项：启用 NoticeLooseBomb Pre + SetState Post + AttackState OnUpdate Pre 钩子
 *
 * 事项 ITEM_M9_FETCH 的应用入口；不做 Detour 创建与签名绑定（那是 Prep 的职责）。
 * 三个钩子同属一个事项：任一失败都回滚，避免只开半边；失败只清 Armed 保留 Ready。
 * 失败语义：任一钩子启用失败则回滚已启用的，只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = 抢包三钩子已驻留
 */
static bool ApplyModule9_FetchBackend()
{
    if (!Item_IsReady(ITEM_M9_FETCH)
        || g_hLooseDetour == null || g_hFetchGuardDetour == null
        || g_hAttackFetchGuardDetour == null)
    {
        Item_SetArmed(ITEM_M9_FETCH, false);
        return false;
    }

    bool looseOk = SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked,
        false, Hook_NoticeLooseBomb_Pre, true, "Module 9");
    bool fetchOk = SyncDetourEnabled(g_hFetchGuardDetour, g_bFetchGuardHooked,
        true, Hook_SetState_Post_Fetch, true, "Module 9");
    bool attackOk = SyncDetourEnabled(g_hAttackFetchGuardDetour, g_bAttackFetchGuardHooked,
        false, Hook_AttackState_OnUpdate_Pre_Fetch, true, "Module 9");

    if (!looseOk || !fetchOk || !attackOk)
    {
        // 挂钩失败属动态环境抖动，不证伪探测结论：只清 Armed 保留 Ready，下次 Enable 重试
        SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked,
            false, Hook_NoticeLooseBomb_Pre, false, "Module 9");
        SyncDetourEnabled(g_hFetchGuardDetour, g_bFetchGuardHooked,
            true, Hook_SetState_Post_Fetch, false, "Module 9");
        SyncDetourEnabled(g_hAttackFetchGuardDetour, g_bAttackFetchGuardHooked,
            false, Hook_AttackState_OnUpdate_Pre_Fetch, false, "Module 9");
        Item_SetArmed(ITEM_M9_FETCH, false);
        LogError("[BotRouteFix] [Module 9] Fetch detour enable failed; M9 fetch roster + OPPO guard item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M9_FETCH, true);
    return true;
}

/**
 * 还原抢包事项（ITEM_M9_FETCH）：摘三钩、清空名册（= 全体放行回原生）并解除武装。
 *
 * 失败语义：无（幂等；摘钩 + 清空名册 + Armed(false)）。
 */
static void RestoreModule9_FetchBackend()
{
    SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked, false, Hook_NoticeLooseBomb_Pre, false, "Module 9");
    SyncDetourEnabled(g_hFetchGuardDetour, g_bFetchGuardHooked, true, Hook_SetState_Post_Fetch, false, "Module 9");
    SyncDetourEnabled(g_hAttackFetchGuardDetour, g_bAttackFetchGuardHooked, false, Hook_AttackState_OnUpdate_Pre_Fetch, false, "Module 9");
    Fetch_ClearRoster();
    Item_SetArmed(ITEM_M9_FETCH, false);
}
