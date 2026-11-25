#if defined _botroutefix_module6_noise_included
    #endinput
#endif
#define _botroutefix_module6_noise_included

//========================================================================================
// [模块 6] 噪声出击统一梯度与下包 OPPO 守卫 (Noise Interception Gradient & Plant OPPO Guard)
//
// 业务背景与引擎逆向分析：
//
// 原生引擎在远端枪响（2000码广播）时，Bot 有 30%~60% 概率触发 InvestigateNoise
// 调头去凑热闹；而 Defuse/Plant/Move 等关键任务在 C++ 源码里根本不调
// InvestigateNoise，进钩子的只有巡逻推进（HuntState）与掩体架枪（HideState）
// 两类 Bot。因此钩子内部不需要任务黑白名单、不需要阵营门禁、不需要下包状态门禁：
// 进来的 Bot 一律按实时士气走同一套梯度（下包前/后、T/CT 全员一致）。
//
// 统一契约：
//    - 负士气 (Morale <= -1)：直接拦截（等效强制 OPPO），一律不调查；
//    - 非负士气 (Morale >= 0)：按士气阶梯约束最大可调查声音距离：
//        Morale  0 -> maxNoiseDist =  500.0 码
//        Morale +1 -> maxNoiseDist = 1000.0 码
//        Morale +2 -> maxNoiseDist = 2000.0 码
//        Morale +3及以上 -> maxNoiseDist = 3000.0 码
//    - 超距或线性距离概率 (1.0 - dist / maxNoiseDist) 未命中时，清空噪声时间戳并直接
//      MRES_Supercede 拦截，阻断战火黑洞效应；命中则放行。
// 噪声路径零内存篡改（纯入口拦截）；拦截/放行均计入遥测。
//
// 事项 ITEM_M6_NOISE（M6 noise + OPPO guard）：噪声拦截 + 下包与先锋 OPPO 守卫同属一件事
// （归属原则：谁执行最终改写，谁拥有事项；两个写点都在 M6）。
// 下包与先锋守卫（SetState Post + AttackState OnUpdate 双面）：对 M7 开路先锋（Module7_GetPointman()，如果有）、
// plantDelay 到期后的持包者（client == GetC4Carrier() 且 earliest <= 0.0 || GetGameTime() >= earliest），
// 以及 task == BOT_TASK_PLANT_BOMB（涵盖到期前提前进点触发的下包任务）
// 且当前 disposition 仍为 ENGAGE 的补写 OPPORTUNITY_FIRE，对标 CT 方 DEFUSE_BOMB 的原生 OPPO；
// SELF_DEFENSE / IGNORE 原样保留。
// 战术缺口封堵：引擎 Attack() 并发直入 m_attackState.OnEnter（不经 SetState，cs_bot_statemachine.cpp），
// 交火全程 SetState Post 面零执行；而出生直写 ENGAGE（cs_bot_init.cpp）与任务授予时机不保证
// 伴随状态迁移，包匪/先锋可能带着 ENGAGE 入战，脱视 0.1s 即遭 33.3% Ambush Hide 拐进坑 3~15 秒。
// 故守卫同时挂 AttackState::OnUpdate Pre：交火 tick 内发现被洗回的 ENGAGE 就地补写 OPPO，
// 敌人缩掩体时引擎读到 OPPO 走原生 StopAttacking 顺畅脱战回路线。
// GUARD_TICKING_BOMB 不强制：T 方守包听到枪声必须能出击，一刀切不合理。
// 补写计入遥测（Module6_GetTotalOppoGuarded）。
//========================================================================================

//========================================================================================
// MODULE 6 DEFINES & GLOBALS
//========================================================================================

// 守包听觉出击距离梯度宏定义 (不同士气等级下允许调查声音的最大距离，单位：单位/码)
#define NOISE_DIST_MORALE_0     500.0   // 士气 0 (均势): 仅探查包点近身 500 码
#define NOISE_DIST_MORALE_1     1000.0  // 士气 +1 (微优): 允许探查中近距离 1000 码
#define NOISE_DIST_MORALE_2     2000.0  // 士气 +2 (优势): 允许探查主干道 2000 码
#define NOISE_DIST_MORALE_3     3000.0  // 士气 +3 (大优): 全图大范围出击 3000 码

// 士气门限宏定义
#define MORALE_HOLD_THRESHOLD   -1      // 保守拦截士气阈值 (<= -1 一律不调查)

// 下包 OPPO 守卫宏（引擎 DispositionType 原值；下包任务值见 global.inc；DISP_OPPORTUNITY 与 module9
// 共用值（同为 1），两边都用 #if !defined 守卫防重定义，改值时两处同步）
#define DISP_ENGAGE               0       // ENGAGE_AND_INVESTIGATE
#if !defined DISP_OPPORTUNITY
#define DISP_OPPORTUNITY          1       // OPPORTUNITY_FIRE
#endif

// 模块 Detour 句柄与统计
// 注：本模块不读 g_bBombPlanted（g_bBombPlanted 仍由 events.inc 维护，供 M5/M7/M10 消费）。
static Handle g_hInvestigateNoiseDetour = null;
static Handle g_hSetStateDetour = null;
static Handle g_hAttackGuardDetour = null;      // AttackState::OnUpdate 交火期兜底载体
static bool g_bNoiseHooked = false;             // per-hook 真实启用态（InvestigateNoise Pre）
static bool g_bSetStateHooked = false;          // per-hook 真实启用态（SetState Post）
static bool g_bAttackGuardHooked = false;       // per-hook 真实启用态（AttackState OnUpdate Pre）
static int g_iTotalNoiseIntercepted = 0;
static int g_iTotalNoiseAllowed = 0;
static int g_iTotalOppoGuarded = 0;

// 模块内部启用缓存（仅用于 Enable 幂等短路；业务判据统一用 Item_IsArmed(ITEM_M6_NOISE)）
static bool g_bModule6_Active = false;

// 模块内部专用偏移缓存（Task/Disposition 上收 global.inc，本文件只用不存）
static int g_iOffset_NoisePosition = -1;
static int g_iOffset_NoiseTimestamp = -1;

//========================================================================================
// MODULE 6 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：噪声控制 + 下包 OPPO 守卫（三 dhook 载体）
 *
 * 条件 = morale/noise/task/disposition 偏移（上收 global.inc，OnPluginStart 一次解析，
 * 这里只做 > 0 门禁）+ 三 Detour 挂载（OPPO 守卫直写 m_disposition 字段，不动 m_ignoreEnemiesTimer）。
 * 噪声路径纯入口拦截；下包守卫只在目标处于 ENGAGE 时补写 OPPO
 * （可视化腰环仍经由 Module6_GetBotDisposition 只读引擎原生值，那是另一条只读链路）。
 *
 * 失败语义：任一条件（morale/noise/task/disposition 偏移、三 Detour）缺失返回 false，
 * Init 置 Ready=false 即事项 FAILED。
 *
 * @return              true = 三后端就绪
 */
static bool PrepModule6_NoiseBackend()
{
    g_iOffset_NoisePosition  = GameConfGetOffset(g_hGameConf, "CCSBot_NoisePosition_Offset");
    g_iOffset_NoiseTimestamp = GameConfGetOffset(g_hGameConf, "CCSBot_NoiseTimestamp_Offset");

    bool offsetsOk = (g_iOffset_Morale > 0
        && g_iOffset_NoisePosition > 0 && g_iOffset_NoiseTimestamp > 0
        && g_iOffset_Task > 0 && g_iOffset_Disposition > 0);
    if (!offsetsOk)
    {
        LogError("[BotRouteFix] [Module 6] Morale/noise/task/disposition offsets missing; M6 noise + OPPO guard item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 6] Noise offsets verified (morale=%d, noisepos=%d, noisets=%d, task=%d, disp=%d).",
        g_iOffset_Morale, g_iOffset_NoisePosition, g_iOffset_NoiseTimestamp,
        g_iOffset_Task, g_iOffset_Disposition);

    if (!MountInvestigateNoiseDetour())
    {
        LogError("[BotRouteFix] [Module 6] InvestigateNoise hook unavailable; M6 noise + OPPO guard item FAILED.");
        return false;
    }

    if (!MountSetStateDetour())
    {
        LogError("[BotRouteFix] [Module 6] SetState hook unavailable; M6 noise + OPPO guard item FAILED.");
        if (g_hInvestigateNoiseDetour != null)
        {
            delete g_hInvestigateNoiseDetour;
            g_hInvestigateNoiseDetour = null;
        }
        return false;
    }

    if (!MountAttackGuardDetour())
    {
        LogError("[BotRouteFix] [Module 6] AttackState hook unavailable; M6 noise + OPPO guard item FAILED.");
        if (g_hInvestigateNoiseDetour != null)
        {
            delete g_hInvestigateNoiseDetour;
            g_hInvestigateNoiseDetour = null;
        }
        if (g_hSetStateDetour != null)
        {
            delete g_hSetStateDetour;
            g_hSetStateDetour = null;
        }
        return false;
    }

    return true;
}

/**
 * 挂载 CCSBot::InvestigateNoise Detour（事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的噪声钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，M6 noise + OPPO guard 事项 FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountInvestigateNoiseDetour()
{
    g_hInvestigateNoiseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_CBaseEntity);
    if (g_hInvestigateNoiseDetour == null)
    {
        LogError("[BotRouteFix] [Module 6] Failed to create DHook detour instance for CCSBot_InvestigateNoise!");
        return false;
    }

    if (!DHookSetFromConf(g_hInvestigateNoiseDetour, g_hGameConf, SDKConf_Signature, "CCSBot_InvestigateNoise"))
    {
        LogError("[BotRouteFix] [Module 6] Failed to set CCSBot_InvestigateNoise signature from gamedata!");
        delete g_hInvestigateNoiseDetour;
        g_hInvestigateNoiseDetour = null;
        return false;
    }

    return true;
}

/**
 * 挂载 CCSBot::SetState Post Detour（事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的下包 OPPO 钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，M6 noise + OPPO guard 事项 FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountSetStateDetour()
{
    g_hSetStateDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hSetStateDetour == null)
    {
        LogError("[BotRouteFix] [Module 6] Failed to create DHook detour instance for CCSBot_SetState!");
        return false;
    }

    if (!DHookSetFromConf(g_hSetStateDetour, g_hGameConf, SDKConf_Signature, "CCSBot_SetState"))
    {
        LogError("[BotRouteFix] [Module 6] Failed to set CCSBot_SetState signature from gamedata!");
        delete g_hSetStateDetour;
        g_hSetStateDetour = null;
        return false;
    }

    // SetState(BotState*) 有一个指针参数；漏加参数会让 DHook trampoline 崩溃（c0000005）
    DHookAddParam(g_hSetStateDetour, HookParamType_Int);
    return true;
}

/**
 * 挂载 AttackState::OnUpdate Pre Detour（事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的交火期兜底载体）
 *
 * 引擎 Attack() 并发直入 m_attackState.OnEnter（不走 SetState，cs_bot_statemachine.cpp），
 * 守卫的 SetState Post 面在交火全程零执行；此钩子在攻击 tick 内补位，确保持包者/先锋
 * 交火期被洗回的 ENGAGE 就地补写为 OPPORTUNITY_FIRE，掐死脱视 0.1s 的 33.3% Ambush Hide。
 * 与 M5 后撤的 AttackState_OnUpdate 同目标同签名（gamedata 共条目，M9 亦同），
 * detour 独立互不干扰（与 M6/M9 双 SetState 钩子同惯例）。
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，M6 noise + OPPO guard 事项 FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountAttackGuardDetour()
{
    g_hAttackGuardDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hAttackGuardDetour == null)
    {
        LogError("[BotRouteFix] [Module 6] Failed to create DHook detour instance for AttackState_OnUpdate!");
        return false;
    }

    if (!DHookSetFromConf(g_hAttackGuardDetour, g_hGameConf, SDKConf_Signature, "AttackState_OnUpdate"))
    {
        LogError("[BotRouteFix] [Module 6] Failed to set AttackState_OnUpdate signature from gamedata!");
        delete g_hAttackGuardDetour;
        g_hAttackGuardDetour = null;
        return false;
    }

    // AttackState::OnUpdate(CCSBot *me) 的 me 以 CBaseEntity 参数直取客户端索引
    DHookAddParam(g_hAttackGuardDetour, HookParamType_CBaseEntity);
    return true;
}

//========================================================================================
// MODULE 6 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 6 的唯一事项 ITEM_M6_NOISE（M6 noise + OPPO guard）
 *
 * 失败语义：事项未武装时返回 false；单事项模块，不可能 DEGRADED。
 *
 * @return              true = 事项已武装
 */
bool InitModule6_Noise()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 6] Gamedata missing; M6 noise + OPPO guard item FAILED.");
        Item_SetReady(ITEM_M6_NOISE, false);
        return false;
    }

    bool noiseReady = PrepModule6_NoiseBackend();
    Item_SetReady(ITEM_M6_NOISE, noiseReady);

    EnableModule6_Noise(true);
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        LogModuleAllFailed(6);
        return false;
    }

    LogModuleArmed(6, "Noise", "1");
    return true;
}

/**
 * 动态启用或停用模块 6（事项 ITEM_M6_NOISE 的调度入口）
 *
 * 事项没有常驻补丁，Enable(false) 只停用钩子并撤销武装标记。
 * 失败语义：只做调度，无返回值；单事项故 Active == Armed。
 *
 * @param enable        true = 武装；false = 解除武装
 */
void EnableModule6_Noise(bool enable)
{
    if (enable == g_bModule6_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule6_NoiseBackend();
        g_bModule6_Active = false;
        return;
    }

    // ITEM_M6_NOISE 是单事项：武装即 ACTIVE，失败即 FAILED，不可能 DEGRADED
    bool noiseArmed = ApplyModule6_NoiseBackend();
    g_bModule6_Active = noiseArmed;
}

/**
 * 还原模块 6（事项 ITEM_M6_NOISE；插件卸载）
 *
 * 本模块没有持久代码补丁，只停用钩子并复位业务态，最后释放 Detour 句柄。
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule6_Noise()
{
    EnableModule6_Noise(false);

    if (g_hInvestigateNoiseDetour != null)
    {
        delete g_hInvestigateNoiseDetour;
        g_hInvestigateNoiseDetour = null;
    }

    if (g_hSetStateDetour != null)
    {
        delete g_hSetStateDetour;
        g_hSetStateDetour = null;
    }

    if (g_hAttackGuardDetour != null)
    {
        delete g_hAttackGuardDetour;
        g_hAttackGuardDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 6] Noise hooks safely restored.");
}

//========================================================================================
// MODULE 6 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 查询累计拦截的出击次数
 *
 * @return              自插件加载以来的累计拦截次数
 */
int Module6_GetTotalNoiseIntercepted()
{
    return g_iTotalNoiseIntercepted;
}

/**
 * 查询累计放行的出击次数
 *
 * @return              自插件加载以来的累计放行次数
 */
int Module6_GetTotalNoiseAllowed()
{
    return g_iTotalNoiseAllowed;
}

/**
 * 查询累计补写的下包 OPPO 次数
 *
 * 跨模块只读 getter，不参与事项状态。
 */
int Module6_GetTotalOppoGuarded()
{
    return g_iTotalOppoGuarded;
}

/**
 * 获取 Bot 当前接战策略 (Disposition)
 *
 * 跨模块只读 getter（可视化按数据源绘制），不参与本模块事项状态。
 * 下包守卫可能补写 OPPO；这里读到的就是当前实际值。
 *
 * @param client        客户端索引
 * @return              0: ENGAGE_AND_INVESTIGATE, 1: OPPORTUNITY_FIRE, -1: 无效/非Bot
 */
int Module6_GetBotDisposition(int client)
{
    // 与其它模块可视化 getter 对齐：只要求在服存活，不限 Bot（调用方均为 Bot 上下文）
    if (!IsValidClient(client, true))
    {
        return -1;
    }

    if (g_iOffset_Disposition <= 0)
    {
        return -1;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return -1;
    }

    return LoadFromAddress(pBot + view_as<Address>(g_iOffset_Disposition), NumberType_Int32);
}

//========================================================================================
// MODULE 6 DETOUR & HOOKS (CCSBOT::INVESTIGATENOISE / CCSBOT::SETSTATE / ATTACKSTATE::ONUPDATE)
//========================================================================================

/**
 * CCSBot::InvestigateNoise Pre：噪声出击统一梯度拦截
 *
 * 事项 M6 噪声控制的唯一执行点；下包前/后、T/CT 全员同一套梯度。
 * 负士气直接阻断（等效强制 OPPO）；非负士气按士气阶梯限距 + 线性距离概率判定。
 * Defuse/Plant/Move 等关键任务在引擎层根本不调本函数，进钩子的只有
 * 巡逻推进与掩体架枪两类 Bot，故不判任务、不判阵营、不判下包状态。
 * 失败语义：回调内不判定事项成败；Supercede 仅表示梯度命中（阻断出击），其余一律 Ignored 放行。
 *
 * @param client        试图出击调查声音的 Bot 客户端索引
 * @param hParams       DHook 参数
 * @return              MRES_Supercede = 拦截出击；MRES_Ignored = 放行
 */
public MRESReturn Hook_InvestigateNoise_Pre(int client)
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null || g_iOffset_Morale <= 0 || g_iOffset_NoisePosition <= 0)
    {
        return MRES_Ignored;
    }

    // 实时士气裁决（残局士气动态跌落也在此处拦下，无需事件快照）
    int morale = GetBotMorale(client);

    // 负士气直接阻断
    if (morale <= MORALE_HOLD_THRESHOLD)
    {
        if (g_iOffset_NoiseTimestamp > 0)
        {
            StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
        }
        g_iTotalNoiseIntercepted++;
        DebugLog("[BotRouteFix] [Module 6] Bot %N negative morale (%d) noise investigate intercepted", client, morale);
        return MRES_Supercede;
    }

    // 依据士气阶梯计算最大允许调查声音距离
    float maxNoiseDist = NOISE_DIST_MORALE_0;
    switch (morale)
    {
        case 0:  maxNoiseDist = NOISE_DIST_MORALE_0;
        case 1:  maxNoiseDist = NOISE_DIST_MORALE_1;
        case 2:  maxNoiseDist = NOISE_DIST_MORALE_2;
        default: maxNoiseDist = NOISE_DIST_MORALE_3; // Morale >= 3
    }

    // 计算 Bot 到声源的距离
    float botPos[3], noisePos[3];
    GetClientAbsOrigin(client, botPos);
    noisePos[0] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition), NumberType_Int32));
    noisePos[1] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition + 4), NumberType_Int32));
    noisePos[2] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition + 8), NumberType_Int32));

    float dist = GetVectorDistance(botPos, noisePos);

    // 超距直接拦截
    if (dist > maxNoiseDist)
    {
        if (g_iOffset_NoiseTimestamp > 0)
        {
            StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
        }
        g_iTotalNoiseIntercepted++;
        DebugLog("[BotRouteFix] [Module 6] Bot %N noise investigate intercepted (dist %.1f > max %.1f)",
            client, dist, maxNoiseDist);
        return MRES_Supercede;
    }

    // 距离概率判定
    float chance = 100.0 * (1.0 - (dist / maxNoiseDist));
    if (GetRandomFloat(0.0, 100.0) > chance)
    {
        if (g_iOffset_NoiseTimestamp > 0)
        {
            StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
        }
        g_iTotalNoiseIntercepted++;
        DebugLog("[BotRouteFix] [Module 6] Bot %N noise investigate intercepted by probability check (chance %.1f%%)",
            client, chance);
        return MRES_Supercede;
    }

    // 允许出击调查
    g_iTotalNoiseAllowed++;
    DebugLog("[BotRouteFix] [Module 6] Bot %N allowed to investigate noise (dist %.1f, morale %s%d)",
        client, dist, (morale >= 0) ? "+" : "", morale);
    return MRES_Ignored;
}

/**
 * 事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的下包与先锋 Disposition 守卫原语
 *
 * 保护对象：
 *   1. M7 开路先锋（Module7_GetPointman()，如果有）
 *   2. plantDelay 到期后的持包者（client == GetC4Carrier() 且 Module2_GetEarliestPlantTime() 已开放；
 *      若无法读取最早下包时间戳则返回 <= 0.0，视为已开放）——与开路先锋对称，不再枚举 TaskType，
 *      避免 FOLLOW / MOVE_TO_LAST_KNOWN / SEEK_AND_DESTROY 等任务切换导致脱保并落入 33.3% Ambush Hide；
 *      到期前顺风 Hunt 控图阶段仍保留原生听声与伏击行为
 *   3. task == BOT_TASK_PLANT_BOMB（兜底：覆盖 plantDelay 到期前提前进点由 IdleState 切入的下包任务）
 * 对上述目标当前 disposition 仍为 ENGAGE 的补写 OPPORTUNITY_FIRE；
 * 对标 CT 方 DEFUSE_BOMB 的原生 OPPO，使先锋专心开路推点、包匪专心下包，
 * 绝不脱离路线探查声音，遭遇战脱视后杜绝 33.3% 伏击长蹲。
 * 真正下包的 SELF_DEFENSE / IGNORE 原样保留。
 *
 * 失败语义：本函数只做守卫补写、不判定事项成败；输入无效或偏移缺失时直接返回。
 */
static void Module6_EnforcePlantDisposition(int client)
{
    if (!IsBot(client, true))
    {
        return;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return;
    }

    if (g_iOffset_Task <= 0 || g_iOffset_Disposition <= 0)
    {
        return;
    }

    bool isTarget = false;
    bool isPointman = false;

    float earliest = Module2_GetEarliestPlantTime();
    bool plantOpen = (earliest <= 0.0 || GetGameTime() >= earliest);

    int pointman = Module7_GetPointman();
    int carrier = GetC4Carrier();
    if (pointman > 0 && client == pointman)
    {
        isTarget = true;
        isPointman = true;
    }
    else if (carrier > 0 && client == carrier && plantOpen)
    {
        isTarget = true;
    }
    else
    {
        // plantDelay 未到但包匪自己踩进包点（Idle:166 会提前开下）时仍保底
        int task = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32);
        if (task == BOT_TASK_PLANT_BOMB)
        {
            isTarget = true;
        }
    }

    if (!isTarget)
    {
        return;
    }

    int disposition = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Disposition), NumberType_Int32);
    if (disposition != DISP_ENGAGE)
    {
        return;
    }

    // 直写 m_disposition，不动 m_ignoreEnemiesTimer，避免在掩体撤退/换弹入口触发时顺手清掉 IgnoreEnemies 保护
    StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), DISP_OPPORTUNITY, NumberType_Int32);
    g_iTotalOppoGuarded++;
    DebugLog("[BotRouteFix] [Module 6] Bot %N %s disposition -> OPPORTUNITY_FIRE",
        client, isPointman ? "pointman" : "plant");
}

/**
 * 事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的 CCSBot::SetState Post 钩子
 *
 * OnEnter/OnExit 内的 ENGAGE 重置都发生在 SetState 内部，Post 在这里统一覆盖。
 * 只处理下包任务，其余一律放行；与 M9 的同目标钩子以后缀区分，
 * 两个 Post 回调都会被 DHooks 依次调用。
 *
 * 失败语义：回调内不判定事项成败；未武装时首行自守，直接返回 MRES_Ignored。
 */
public MRESReturn Hook_SetState_Post_Plant(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M6_NOISE) || pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    int client = ResolveBotClient(pThis);
    if (client == 0)
    {
        return MRES_Ignored;
    }

    Module6_EnforcePlantDisposition(client);
    return MRES_Ignored;
}

/**
 * 事项 ITEM_M6_NOISE（M6 noise + OPPO guard）的 AttackState::OnUpdate Pre 钩子
 * （与 M5 后撤、M9 捡包守卫的同目标钩子以后缀区分，各自 detour 独立回调）。
 *
 * 引擎 Attack() 并发直入 m_attackState.OnEnter（不经 SetState），交火全程
 * SetState Post 面零执行；此面在攻击 tick 内兜底，覆盖"身份已变而状态未迁"
 * 带 ENGAGE 入战的窗口（出生直写/任务授予时机），就地补写 OPPO 掐死
 * 脱视 0.1s 的 33.3% Ambush Hide。业务判定全部下沉 EnforcePlantDisposition。
 *
 * 失败语义：回调内不判定事项成败；未武装时首行自守，直接返回 MRES_Ignored。
 */
public MRESReturn Hook_AttackState_OnUpdate_Pre_Plant(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    Module6_EnforcePlantDisposition(client);
    return MRES_Ignored;
}

/**
 * 事项 ITEM_M6_NOISE（M6 noise + OPPO guard）启用时的一次性补写
 *
 * 纯事件驱动之外的唯一一次扫描，不建 timer；只在 Apply 武装成功后调用。
 *
 * 失败语义：逐个尽力补写，不影响事项 Armed。
 */
static void Module6_SweepPlantDisposition()
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsBot(i, true))
        {
            Module6_EnforcePlantDisposition(i);
        }
    }
}

//========================================================================================
// MODULE 6 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用噪声事项：启用 InvestigateNoise Pre 钩子 + SetState Post 钩子 + AttackState OnUpdate Pre 钩子
 *
 * 事项 ITEM_M6_NOISE 的应用入口；不做 Detour 创建与签名绑定（那是 Prep 的职责）。
 * 三个钩子同属一个事项：任一失败都回滚，避免只开半边；失败只清 Armed 保留 Ready。
 * 失败语义：任一钩子启用失败则回滚已启用的，只清 Armed 保留 Ready，事项待下次 Enable 重试。
 * 武装成功后单次扫盘（热重载时把已在下包任务的持包人一次性补齐）。
 *
 * @return              true = 三钩子已驻留
 */
static bool ApplyModule6_NoiseBackend()
{
    if (!Item_IsReady(ITEM_M6_NOISE)
        || g_hInvestigateNoiseDetour == null || g_hSetStateDetour == null
        || g_hAttackGuardDetour == null)
    {
        Item_SetArmed(ITEM_M6_NOISE, false);
        return false;
    }

    bool noiseOk = SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseHooked,
        false, Hook_InvestigateNoise_Pre, true, "Module 6");
    bool stateOk = SyncDetourEnabled(g_hSetStateDetour, g_bSetStateHooked,
        true, Hook_SetState_Post_Plant, true, "Module 6");
    bool attackOk = SyncDetourEnabled(g_hAttackGuardDetour, g_bAttackGuardHooked,
        false, Hook_AttackState_OnUpdate_Pre_Plant, true, "Module 6");

    if (!noiseOk || !stateOk || !attackOk)
    {
        SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseHooked,
            false, Hook_InvestigateNoise_Pre, false, "Module 6");
        SyncDetourEnabled(g_hSetStateDetour, g_bSetStateHooked,
            true, Hook_SetState_Post_Plant, false, "Module 6");
        SyncDetourEnabled(g_hAttackGuardDetour, g_bAttackGuardHooked,
            false, Hook_AttackState_OnUpdate_Pre_Plant, false, "Module 6");
        Item_SetArmed(ITEM_M6_NOISE, false);
        LogError("[BotRouteFix] [Module 6] Detour enable failed; M6 noise + OPPO guard item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M6_NOISE, true);
    Module6_SweepPlantDisposition();
    return true;
}

/**
 * 还原噪声事项（ITEM_M6_NOISE）：摘三钩并解除武装。
 *
 * 失败语义：无（幂等；摘钩 + Armed(false)）。
 */
static void RestoreModule6_NoiseBackend()
{
    SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseHooked, false, Hook_InvestigateNoise_Pre, false, "Module 6");
    SyncDetourEnabled(g_hSetStateDetour, g_bSetStateHooked, true, Hook_SetState_Post_Plant, false, "Module 6");
    SyncDetourEnabled(g_hAttackGuardDetour, g_bAttackGuardHooked, false, Hook_AttackState_OnUpdate_Pre_Plant, false, "Module 6");
    Item_SetArmed(ITEM_M6_NOISE, false);
}
