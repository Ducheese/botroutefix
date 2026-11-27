#if defined _botroutefix_module6_noise_included
    #endinput
#endif
#define _botroutefix_module6_noise_included

//========================================================================================
// [模块 6] 噪声出击统一梯度 (Noise Interception Gradient)
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
// （原下包/先锋防伏击已脱离本模块，统一交由 M15 的 Hide(-1) 与 Ambush Hide 处置。）
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

// 模块 Detour 句柄与统计
static Handle g_hInvestigateNoiseDetour = null;
static bool g_bNoiseHooked = false;             // per-hook 真实启用态（InvestigateNoise Pre）
static int g_iTotalNoiseIntercepted = 0;
static int g_iTotalNoiseAllowed = 0;

// 模块内部启用缓存（仅用于 Enable 幂等短路；业务判据统一用 Item_IsArmed(ITEM_M6_NOISE)）
static bool g_bModule6_Active = false;

// 模块内部专用偏移缓存
static int g_iOffset_NoisePosition = -1;
static int g_iOffset_NoiseTimestamp = -1;

//========================================================================================
// MODULE 6 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：噪声出击拦截（ITEM_M6_NOISE）
 *
 * 条件 = morale/noise 偏移（morale 上收 global.inc，OnPluginStart 一次解析，
 * 这里只做 > 0 门禁）+ CCSBot::InvestigateNoise Pre Detour 挂载。
 * 失败语义：任一条件缺失返回 false，Init 置 Ready=false 即事项 FAILED。
 *
 * @return              true = 噪声后端就绪
 */
static bool PrepModule6_NoiseBackend()
{
    g_iOffset_NoisePosition  = GameConfGetOffset(g_hGameConf, "CCSBot_NoisePosition_Offset");
    g_iOffset_NoiseTimestamp = GameConfGetOffset(g_hGameConf, "CCSBot_NoiseTimestamp_Offset");

    bool offsetsOk = (g_iOffset_Morale > 0
        && g_iOffset_NoisePosition > 0 && g_iOffset_NoiseTimestamp > 0);
    if (!offsetsOk)
    {
        LogError("[BotRouteFix] [Module 6] Morale/noise offsets missing; M6 noise item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 6] Noise offsets verified (morale=%d, noisepos=%d, noisets=%d).",
        g_iOffset_Morale, g_iOffset_NoisePosition, g_iOffset_NoiseTimestamp);

    if (!MountInvestigateNoiseDetour())
    {
        LogError("[BotRouteFix] [Module 6] InvestigateNoise hook unavailable; M6 noise item FAILED.");
        return false;
    }

    return true;
}

/**
 * 挂载 CCSBot::InvestigateNoise Detour（事项 ITEM_M6_NOISE 的噪声钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，M6 noise 事项 FAILED。
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

//========================================================================================
// MODULE 6 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 6 的唯一事项 ITEM_M6_NOISE（M6 noise intercept）
 *
 * 失败语义：事项未武装时返回 false；单事项模块，不可能 DEGRADED。
 *
 * @return              true = 事项已武装
 */
bool InitModule6_Noise()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 6] Gamedata missing; M6 noise item FAILED.");
        Item_SetReady(ITEM_M6_NOISE, false);
        return false;
    }

    bool noiseReady = PrepModule6_NoiseBackend();
    Item_SetReady(ITEM_M6_NOISE, noiseReady);

    EnableModule6_Noise(true);
    if (!g_bModule6_Active)
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
 * 本模块没有持久代码补丁，只停用钩子并释放 Detour 句柄。
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
 * 获取 Bot 当前接战策略 (Disposition)
 *
 * 跨模块只读 getter（可视化按数据源绘制），不参与本模块事项状态。
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
// MODULE 6 DETOUR & HOOKS (CCSBOT::INVESTIGATENOISE)
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

//========================================================================================
// MODULE 6 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用噪声事项：启用 InvestigateNoise Pre 钩子
 *
 * 事项 ITEM_M6_NOISE 的应用入口；不做 Detour 创建与签名绑定（那是 Prep 的职责）。
 * 失败语义：钩子启用失败只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = 钩子已驻留
 */
static bool ApplyModule6_NoiseBackend()
{
    if (!Item_IsReady(ITEM_M6_NOISE) || g_hInvestigateNoiseDetour == null)
    {
        Item_SetArmed(ITEM_M6_NOISE, false);
        return false;
    }

    if (!SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseHooked,
        false, Hook_InvestigateNoise_Pre, true, "Module 6"))
    {
        RestoreModule6_NoiseBackend();
        LogError("[BotRouteFix] [Module 6] Detour enable failed; M6 noise item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M6_NOISE, true);
    return true;
}

/**
 * 还原噪声事项（ITEM_M6_NOISE）：摘钩并解除武装。
 *
 * 失败语义：无（幂等；摘钩 + Armed(false)）。
 */
static void RestoreModule6_NoiseBackend()
{
    SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseHooked, false, Hook_InvestigateNoise_Pre, false, "Module 6");
    Item_SetArmed(ITEM_M6_NOISE, false);
}
