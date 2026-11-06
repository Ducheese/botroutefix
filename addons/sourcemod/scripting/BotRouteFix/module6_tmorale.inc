#if defined _botroutefix_module6_tmorale_included
    #endinput
#endif
#define _botroutefix_module6_tmorale_included

//========================================================================================
// [模块 6] T 阵营士气接战策略与守包听觉出击梯度 (TMorale Disposition & Audio Interception)
//
// 业务背景与引擎逆向分析：
//
// 1. 下包前 T 包匪行为 (Pre-Plant C4 Carrier Morale Disposition):
//    - 机制分析: 原生引擎在分路推进时，任何战区（如中路）一声枪响（2000码广播）都会使全图
//               Bot 有 30%~60% 概率触发 InvestigateNoise 调头去凑热闹，导致包匪放弃下包。
//    - 补丁方案: 依赖事件（round_freeze_end 开局与 item_pickup 捡包）单次赋值:
//        * 低士气 (Morale <= -1): 设为 OPPORTUNITY_FIRE (1)，底层 HeardInterestingNoise()
//          对声音直接免疫，心无旁骛直奔包点下包；
//        * 高士气 (Morale >= 0): 保持原生 ENGAGE_AND_INVESTIGATE (0)。
//
// 2. 下包后 T 全队守点梯度 (Post-Plant T Defense Morale Gradient):
//    - 机制分析: C4 已经安放，低士气 T 应当誓死卡在掩体内守包，而不是被远端枪声勾引出掩体白给。
//    - 补丁方案: 下包瞬间 (bomb_planted):
//        * 低士气 (Morale <= -1): 设为 OPPORTUNITY_FIRE (1)，清空噪声时间戳，HideState
//          底层自动阻断出击，全员死守掩体架枪；
//        * 高士气 (Morale >= 0): 保持 ENGAGE_AND_INVESTIGATE (0)。
//
// 3. 守包出击拦截 (DHook Pre on CCSBot::InvestigateNoise):
//    - 针对任务: 仅针对守包掩体任务 (Task 4: GUARD_TICKING_BOMB)，逃跑 (Task 9) 与搜敌 (Task 0) 自然放行。
//    - 梯度约束: 针对保持 ENGAGE 的非负士气 T (Morale >= 0)，按士气阶梯约束最大可调查声音距离:
//        Morale  0 -> maxNoiseDist =  500.0 码
//        Morale +1 -> maxNoiseDist = 1000.0 码
//        Morale +2 -> maxNoiseDist = 2000.0 码
//        Morale +3 -> maxNoiseDist = 3000.0 码
//    - 超距或概率判定 (1.0 - dist / maxNoiseDist) 未命中时，清空噪声时间戳并直接 MRES_Supercede
//      拦截状态切换，强制 Bot 保持掩体架枪，阻断战火黑洞效应。
//========================================================================================

//========================================================================================
// MODULE 6 DEFINES & GLOBALS
//========================================================================================

// 守包听觉出击距离梯度宏定义 (不同士气等级下允许调查声音的最大距离，单位：单位/码)
#define NOISE_DIST_MORALE_0     500.0   // 士气 0 (均势): 仅探查包点近身 500 码
#define NOISE_DIST_MORALE_1     1000.0  // 士气 +1 (微优): 允许探查中近距离 1000 码
#define NOISE_DIST_MORALE_2     2000.0  // 士气 +2 (优势): 允许探查主干道 2000 码
#define NOISE_DIST_MORALE_3     3000.0  // 士气 +3 (大优): 全图大范围出击 3000 码

// 接战策略与任务宏定义
#define MORALE_HOLD_THRESHOLD   -1      // 保守死守掩体士气阈值 (<= -1 专注运包/死守掩体)
#define BOT_TASK_GUARD_TICKING_BOMB 4   // 引擎原生任务枚举：GUARD_TICKING_BOMB (守已安放的雷包)

// 模块 Detour 句柄与统计
// 注：对局炸弹状态 g_bBombPlanted 已上收至 global.inc，由 events.inc 事件总线唯一维护，
//     本模块只读消费，模块启停不再影响对局状态流转。
static Handle g_hInvestigateNoiseDetour = null;
static int g_iTotalNoiseIntercepted = 0;
static int g_iTotalNoiseAllowed = 0;

// 模块内部启用缓存（仅用于 Enable 幂等短路；业务判据统一用 Item_IsArmed(ITEM_M6_NOISE)）
static bool g_bModule6_Active = false;
static bool g_bNoiseDetourEnabled = false;

// 模块内部专用偏移缓存
static int g_iOffset_Disposition = -1;
static int g_iOffset_Task = -1;
static int g_iOffset_NoisePosition = -1;
static int g_iOffset_NoiseTimestamp = -1;

//========================================================================================
// MODULE 6 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：噪声控制（事件写 disposition + dhook 拦截）
 *
 * 条件 = disposition/task/noise 偏移 + morale 偏移 + InvestigateNoise Detour 挂载。
 * 事件写入与钩子拦截同属一件事，缺一不可：任一后端缺失整个事项 FAILED，
 * 不允许只跑事件写入。
 *
 * @return              true = 噪声事项后端就绪
 */
static bool PrepModule6_NoiseBackend()
{
    g_iOffset_Disposition    = GameConfGetOffset(g_hGameConf, "CCSBot_Disposition_Offset");
    g_iOffset_Task           = GameConfGetOffset(g_hGameConf, "CCSBot_Task_Offset");
    g_iOffset_NoisePosition  = GameConfGetOffset(g_hGameConf, "CCSBot_NoisePosition_Offset");
    g_iOffset_NoiseTimestamp = GameConfGetOffset(g_hGameConf, "CCSBot_NoiseTimestamp_Offset");

    bool offsetsOk = (g_iOffset_Disposition > 0 && g_iOffset_Task > 0
        && g_iOffset_NoisePosition > 0 && g_iOffset_NoiseTimestamp > 0
        && g_iOffset_Morale > 0);
    if (!offsetsOk)
    {
        LogError("[BotRouteFix] [Module 6] Offsets or morale offset missing; noise item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 6] Noise offsets verified (disposition=%d, task=%d, noisepos=%d, noisets=%d, morale=%d).",
        g_iOffset_Disposition, g_iOffset_Task, g_iOffset_NoisePosition, g_iOffset_NoiseTimestamp, g_iOffset_Morale);

    if (!MountInvestigateNoiseDetour())
    {
        LogError("[BotRouteFix] [Module 6] InvestigateNoise hook unavailable; noise item FAILED.");
        return false;
    }

    return true;
}

/**
 * 挂载 CCSBot::InvestigateNoise Detour（事项 M6 噪声控制的钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，噪声事项 FAILED。
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
 * 初始化模块 6 的唯一事项：噪声控制（事件写 disposition + dhook 拦截）
 *
 * @return              true = 事项已武装
 */
bool InitModule6_TMorale()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 6] Gamedata missing; noise item FAILED.");
        Item_SetReady(ITEM_M6_NOISE, false);
        return false;
    }

    bool noiseReady = PrepModule6_NoiseBackend();
    Item_SetReady(ITEM_M6_NOISE, noiseReady);

    EnableModule6_TMorale(true);
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        LogModuleAllFailed(6);
        return false;
    }

    LogModuleArmed(6, "Noise control", Item_IsArmed(ITEM_M6_NOISE) ? "1" : "0");
    return true;
}

/**
 * 动态启用或停用模块 6
 *
 * 事项没有常驻补丁，Enable(false) 只停用钩子并撤销武装标记。
 *
 * @param enable        true = 武装；false = 解除武装
 */
void EnableModule6_TMorale(bool enable)
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
 * 还原模块 6（插件卸载）
 *
 * 本模块没有持久代码补丁，只停用钩子并复位业务态，最后释放 Detour 句柄。
 */
void RestoreModule6_TMorale()
{
    EnableModule6_TMorale(false);

    if (g_hInvestigateNoiseDetour != null)
    {
        delete g_hInvestigateNoiseDetour;
        g_hInvestigateNoiseDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 6] Morale tactics hooks safely restored.");
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
    if (client < 1 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client) || !IsFakeClient(client))
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
// MODULE 6 EVENT HANDLERS
//========================================================================================

/**
 * 冻结结束 (round_freeze_end): 配置开局包匪接战策略
 */
void Module6_OnRoundFreezeEnd()
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return;
    }

    ApplyCarrierMoraleDisposition();
}

/**
 * 拾取道具 (item_pickup): 若新持有者产生，更新包匪策略
 */
void Module6_OnItemPickup(int client, const char[] item)
{
    #pragma unused client
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return;
    }

    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        ApplyCarrierMoraleDisposition();
    }
}

/**
 * 下包完成 (bomb_planted): 配置 T 全队守点姿态梯度
 * （g_bBombPlanted 由事件总线在分发前置真，本模块只读）
 */
void Module6_OnBombPlanted()
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return;
    }

    ApplyPlantedBombTMoraleDisposition();
}

//========================================================================================
// MODULE 6 DETOUR & HOOKS (CCSBOT::INVESTIGATENOISE)
//========================================================================================

/**
 * CCSBot::InvestigateNoise Pre：守包掩体 T 的出击拦截
 *
 * 事项 M6 噪声控制的执行点；只在噪声事项武装且已下包时拦截。
 * 负士气直接阻断；非负士气按士气阶梯限距 + 概率判定。
 *
 * @param client        被调查 noise 的 Bot 客户端索引
 * @param hParams       DHook 参数
 * @return              MRES_Supercede = 拦截出击；MRES_Ignored = 放行
 */
public MRESReturn Hook_InvestigateNoise_Pre(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M6_NOISE))
    {
        return MRES_Ignored;
    }

    if (!g_bBombPlanted)
    {
        return MRES_Ignored;
    }

    if (!IsTeamBot(client, 2, true))
    {
        return MRES_Ignored;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null || g_iOffset_Task <= 0 || g_iOffset_Morale <= 0 || g_iOffset_NoisePosition <= 0)
    {
        return MRES_Ignored;
    }

    // 仅拦截守包任务 (Task 4: GUARD_TICKING_BOMB)
    // 逃跑任务 (Task 9: ESCAPE_FROM_BOMB) 与搜敌任务 (Task 0: SEEK_AND_DESTROY) 自然放行
    int task = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32);
    if (task != BOT_TASK_GUARD_TICKING_BOMB)
    {
        return MRES_Ignored;
    }

    // 下包初态虽已分流，但残局士气可能动态跌落，此处获取实时士气进行严密二次校验与防御阻断
    int morale = GetBotMorale(client);

    // 负士气 T 直接阻断
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
 * 事项 M6 噪声控制的唯一应用入口；失败时该事项 FAILED。
 *
 * @return              true = Pre 钩子已启用
 */
static bool ApplyModule6_NoiseBackend()
{
    if (!Item_IsReady(ITEM_M6_NOISE) || g_hInvestigateNoiseDetour == null)
    {
        Item_SetArmed(ITEM_M6_NOISE, false);
        return false;
    }

    bool ok = SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseDetourEnabled,
        false, Hook_InvestigateNoise_Pre, true, "Module 6");
    Item_SetArmed(ITEM_M6_NOISE, ok);
    return ok;
}

/**
 * 还原噪声事项：停用 Pre 钩子并解除武装
 */
static void RestoreModule6_NoiseBackend()
{
    SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseDetourEnabled, false, Hook_InvestigateNoise_Pre, false, "Module 6");
    Item_SetArmed(ITEM_M6_NOISE, false);
}

//========================================================================================
// MODULE 6 DISPOSITION PRIMITIVES
//========================================================================================

/**
 * 根据持包者士气更新其接战策略
 * 低士气 (<= -1) -> OPPORTUNITY_FIRE (1) 专注运包
 * 高士气 (>= 0)  -> ENGAGE_AND_INVESTIGATE (0)
 *
 * 内部原语，只被本模块事件处理调用。
 */
static void ApplyCarrierMoraleDisposition()
{
    int carrier = GetC4Carrier();
    if (!IsBot(carrier, true))
    {
        return;
    }

    Address pCarrier = GetEntityAddress(carrier);
    if (pCarrier == Address_Null || g_iOffset_Morale <= 0 || g_iOffset_Disposition <= 0)
    {
        return;
    }

    int morale = GetBotMorale(carrier);
    if (morale <= MORALE_HOLD_THRESHOLD)
    {
        StoreToAddress(pCarrier + view_as<Address>(g_iOffset_Disposition), 1, NumberType_Int32); // OPPORTUNITY_FIRE
        DebugLog("[BotRouteFix] [Module 6] C4 Carrier %N (Morale %s%d) set to OPPORTUNITY_FIRE (Focused Plant)", carrier, (morale >= 0) ? "+" : "", morale);
    }
    else
    {
        StoreToAddress(pCarrier + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
        DebugLog("[BotRouteFix] [Module 6] C4 Carrier %N (Morale %s%d) kept as ENGAGE_AND_INVESTIGATE", carrier, (morale >= 0) ? "+" : "", morale);
    }
}

/**
 * 下包后根据全队 T 士气配置守点梯度
 * 低士气 (<= -1) -> OPPORTUNITY_FIRE (1) 死守掩体
 * 高士气 (>= 0)  -> ENGAGE_AND_INVESTIGATE (0) 动态架枪
 *
 * 内部原语，只被本模块事件处理调用。
 */
static void ApplyPlantedBombTMoraleDisposition()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsTeamBot(i, 2, true))
        {
            Address pBot = GetEntityAddress(i);
            if (pBot == Address_Null || g_iOffset_Morale <= 0 || g_iOffset_Disposition <= 0)
            {
                continue;
            }

            int morale = GetBotMorale(i);
            if (morale <= MORALE_HOLD_THRESHOLD)
            {
                StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), 1, NumberType_Int32); // OPPORTUNITY_FIRE
                if (g_iOffset_NoiseTimestamp > 0)
                {
                    StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
                }
                DebugLog("[BotRouteFix] [Module 6] Post-Plant T %N (Morale %s%d) -> OPPORTUNITY_FIRE (Site Camp)", i, (morale >= 0) ? "+" : "", morale);
            }
            else
            {
                StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
                DebugLog("[BotRouteFix] [Module 6] Post-Plant T %N (Morale %s%d) -> ENGAGE_AND_INVESTIGATE (Dynamic Guard)", i, (morale >= 0) ? "+" : "", morale);
            }
        }
    }
}
