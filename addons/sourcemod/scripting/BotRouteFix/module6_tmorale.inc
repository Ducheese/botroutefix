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
Handle g_hInvestigateNoiseDetour = null;
int g_iTotalNoiseIntercepted = 0;
int g_iTotalNoiseAllowed = 0;

// 业务开启态与 per-hook 真实启用态（DHook 状态机唯一事实来源）
static bool g_bModule6_Active = false;
static bool g_bNoiseDetourEnabled = false;

// 模块内部专用偏移缓存
static int g_iOffset_Disposition = -1;
static int g_iOffset_Task = -1;
static int g_iOffset_NoisePosition = -1;
static int g_iOffset_NoiseTimestamp = -1;

/**
 * 初始化模块 6 所需的偏移与 DHook Detour
 *
 * @return              是否初始化成功
 */
bool InitModule6_TMorale()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 6] Error: Gamedata is not loaded, cannot initialize morale tactics module!");
        return false;
    }

    // 1. 读取所需内部偏移
    g_iOffset_Disposition    = GameConfGetOffset(g_hGameConf, "CCSBot_Disposition_Offset");
    g_iOffset_Task           = GameConfGetOffset(g_hGameConf, "CCSBot_Task_Offset");
    g_iOffset_NoisePosition  = GameConfGetOffset(g_hGameConf, "CCSBot_NoisePosition_Offset");
    g_iOffset_NoiseTimestamp = GameConfGetOffset(g_hGameConf, "CCSBot_NoiseTimestamp_Offset");

    if (g_iOffset_Disposition == -1 || g_iOffset_Task == -1 ||
        g_iOffset_NoisePosition == -1 || g_iOffset_NoiseTimestamp == -1)
    {
        LogError("[BotRouteFix] [Module 6] Failed to read one or more disposition/noise offsets from gamedata!");
        return false;
    }

    // 2. 挂载 CCSBot::InvestigateNoise Pre Detour
    if (!MountInvestigateNoiseDetour())
    {
        LogError("[BotRouteFix] [Module 6] Failed to hook CCSBot::InvestigateNoise detour!");
        return false;
    }

    // 3. 套用业务态
    EnableModule6_TMorale(true);
    if (!g_bModule6_Active)
    {
        delete g_hInvestigateNoiseDetour;
        g_hInvestigateNoiseDetour = null;
        LogError("[BotRouteFix] [Module 6] Failed to enable InvestigateNoise detour!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 6] Morale tactics & audio interception active [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 动态启用或停用模块 6（幂等）
 */
void EnableModule6_TMorale(bool enable)
{
    if (enable == g_bModule6_Active)
    {
        return;
    }

    if (enable)
    {
        g_bModule6_Active = SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseDetourEnabled,
            false, Hook_InvestigateNoise_Pre, true, "Module 6");
        return;
    }

    SyncDetourEnabled(g_hInvestigateNoiseDetour, g_bNoiseDetourEnabled, false, Hook_InvestigateNoise_Pre, false, "Module 6");
    g_bModule6_Active = false;
}

/**
 * 还原模块 6（插件卸载：摘钩并释放句柄）
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

/**
 * 查询模块 6 业务开启态
 */
bool IsModule6_Active()
{
    return g_bModule6_Active;
}

/**
 * 查询声音出击拦截 Detour 是否生效
 */
bool IsInvestigateNoiseHooked()
{
    return (g_hInvestigateNoiseDetour != null && g_bNoiseDetourEnabled);
}

/**
 * 获取累计拦截的出击次数
 */
int GetTotalNoiseIntercepted()
{
    return g_iTotalNoiseIntercepted;
}

/**
 * 获取累计放行的出击次数
 */
int GetTotalNoiseAllowed()
{
    return g_iTotalNoiseAllowed;
}

//========================================================================================
// EVENT HANDLERS
//========================================================================================

/**
 * 冻结结束 (round_freeze_end): 配置开局包匪接战策略
 */
void Module6_OnRoundFreezeEnd()
{
    if (!g_bModule6_Active)
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
    if (!g_bModule6_Active)
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
    if (!g_bModule6_Active)
    {
        return;
    }

    ApplyPlantedBombTMoraleDisposition();
}

//========================================================================================
// DISPOSITION MANAGEMENT PRIMITIVES
//========================================================================================

/**
 * 根据持包者士气更新其接战策略
 * 低士气 (<= -1) -> OPPORTUNITY_FIRE (1) 专注运包
 * 高士气 (>= 0)  -> ENGAGE_AND_INVESTIGATE (0)
 */
void ApplyCarrierMoraleDisposition()
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
        DebugLog("[BotRouteFix] [Module 6] C4 Carrier %N (Morale %d) set to OPPORTUNITY_FIRE (Focused Plant)", carrier, morale);
    }
    else
    {
        StoreToAddress(pCarrier + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
        DebugLog("[BotRouteFix] [Module 6] C4 Carrier %N (Morale %d) kept as ENGAGE_AND_INVESTIGATE", carrier, morale);
    }
}

/**
 * 下包后根据全队 T 士气配置守点梯度
 * 低士气 (<= -1) -> OPPORTUNITY_FIRE (1) 死守掩体
 * 高士气 (>= 0)  -> ENGAGE_AND_INVESTIGATE (0) 动态架枪
 */
void ApplyPlantedBombTMoraleDisposition()
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
                DebugLog("[BotRouteFix] [Module 6] Post-Plant T %N (Morale %d) -> OPPORTUNITY_FIRE (Site Camp)", i, morale);
            }
            else
            {
                StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
                DebugLog("[BotRouteFix] [Module 6] Post-Plant T %N (Morale %d) -> ENGAGE_AND_INVESTIGATE (Dynamic Guard)", i, morale);
            }
        }
    }
}

//========================================================================================
// DETOUR: CCSBOT::INVESTIGATENOISE
//========================================================================================

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

public MRESReturn Hook_InvestigateNoise_Pre(int client, DHookParam hParams)
{
    if (!g_bModule6_Active)
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
        // DebugLog("[BotRouteFix] [Module 6] Bot %N noise investigate intercepted (dist %.1f > max %.1f)",
        //     client, dist, maxNoiseDist);
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
        // DebugLog("[BotRouteFix] [Module 6] Bot %N noise investigate intercepted by probability check (chance %.1f%%)",
        //     client, chance);
        return MRES_Supercede;
    }

    // 允许出击调查
    g_iTotalNoiseAllowed++;
    DebugLog("[BotRouteFix] [Module 6] Bot %N allowed to investigate noise (dist %.1f, morale %d)",
        client, dist, morale);
    return MRES_Ignored;
}
