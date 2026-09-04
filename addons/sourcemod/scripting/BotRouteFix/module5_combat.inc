#if defined _botroutefix_module5_combat_included
    #endinput
#endif
#define _botroutefix_module5_combat_included

//========================================================================================
// [模块 5] 战斗决策重构 (AttackState 闪避 100% 强制与换弹进掩体 100% 强制)
//
// 业务背景与引擎逆向分析：
//
// 1. AttackState 闪避判定 100% 强制 (cs_bot_attack.cpp:115-130):
//    - 源码逻辑:
//        float dodgeChance = 80.0f * profile->GetSkill();
//        if (profile->GetSkill() > 0.5f && (me->IsOutnumbered() || me->IsEnemySniperVisible()))
//            dodgeChance = 100.0f;
//        if (RandomFloat( 0.0f, 100.0f ) <= dodgeChance)
//            m_shouldDodge = true;
//        else
//            m_shouldDodge = false;
//    - 机制痛点: 原生低难度或普通 Bot 极少闪避（0%~80%），在交火对枪时形同呆立木桩。
//    - 解决机制: 在 `AttackState::OnEnter` 的 Post 钩子中，直接将 `AttackState` 结构体中的
//               `m_shouldDodge` 字段（布尔型，32 位 +50，64 位 +58）强制置为 1 (true)。
//               全员进入 `Dodge()` 规避机动，后续的动作灵敏度在 `Dodge()` 内部自然按 skill
//               分化，彻底摆脱开局掷骰不闪避的呆滞表现。
//
// 2. 换弹寻找掩体 100% 强制 (cs_bot_weapon.cpp:1186-1210):
//    - 源码逻辑:
//        if (GetNearbyEnemyCount() > 0)
//        {
//            float hideChance = 25.0f + 100.0f * profile->GetSkill();
//            if (RandomFloat( 0.0f, 100.0f ) <= hideChance && !IsHiding())
//            {
//                if (GetTimeSinceLastSawEnemy() < 5.0f)
//                    Hide( spot );
//            }
//        }
//    - 机制痛点: 原生换弹掩体逻辑存在严重"看脸"判定（hideChance 掷骰），导致 Bot 在近距离交火
//               弹药打空时经常在开阔地原地原地干拉换弹，成为活靶子。
//    - 补丁方案: NOP 掉掷骰失败后跳过掩体分支的短跳转指令（2 字节）:
//        * 32 位: `76 ??` (jbe short)  @ sig+4 -> `90 90`
//        * 64 位: `73 ??` (jnb short)  @ sig+9 -> `90 90`
//      去除了"看脸"的概率掷骰，同时完整保留了前置条件（附近有敌、未在掩蔽、5秒内目击敌人）
//      与寻找掩体有效性检查，确保 Bot 遇敌换弹时必找掩体规避。
//
// 3. AttackState 残血强制尝试后撤 (Low Health Forced Attempt Retreat via PinnedDown):
//    - 源码逻辑 (cs_bot_attack.cpp:334-360):
//        if (m_retreatTimer.IsElapsed()) {
//            bool isPinnedDown = (gpGlobals->curtime > m_pinnedDownTimestamp);
//            if (isPinnedDown || ...) {
//                if (me->IsAnyVisibleEnemyLookingAtMe()) {
//                    m_retreatTimer.Start( RandomFloat( 3.0f, 15.0f ) );
//                    TryToRetreat();
//                }
//            }
//        }
//    - 机制分析: 原生只有交火持续 7~10 秒后 (`m_pinnedDownTimestamp`) 或被围攻时才后撤，Bot 残血时
//               往往无脑继续对枪直至暴毙。注意：引擎原生撤退段内仍受敌人视线与技能判定门控，
//               且最终调用 `TryToRetreat()` 依赖掩体可达性。
//    - 解决机制: 在 `AttackState::OnUpdate` 的 Pre 钩子中，检测血量 < 35 HP 且距上次后撤尝试过 1.5s 冷却时:
//        * 将 `m_pinnedDownTimestamp` 写为 0.0，使 `isPinnedDown` (curtime > 0.0) 恒真；
//        * 将 `m_retreatTimer` 冷却清空，使 `IsElapsed()` 立即成立；
//        强行撕开原版交火时长与退避冷却的硬性封锁，促使下一帧引擎原生撤退段立即执行 `TryToRetreat()` 尝试后撤，
//        成功即转入 `HideState`，杜绝残血时因计时器死锁而在开阔地白给。
//========================================================================================

#define HEALTH_RETREAT_THRESHOLD 35
#define RETREAT_THROTTLE_INTERVAL 1.5

// 模块状态、内存补丁与 Detour 句柄
MemoryPatch g_PatchReloadHide;
Handle g_hAttackDodgeDetour = null;
Handle g_hAttackRetreatDetour = null;
int g_iTotalDodgeForced = 0;
int g_iTotalRetreatTriggered = 0;

// 模块内部专用冷却与偏移缓存
static float g_flNextForcedRetreat[MAXPLAYERS + 1];
static int g_iOffset_AttackShouldDodge = -1;
static int g_iOffset_PinnedDownTimestamp = -1;
static int g_iOffset_MRetreatTimer = -1;
static int g_iOffset_TimerTimestamp = -1;
static int g_iOffset_TimerDuration = -1;

/**
 * 初始化模块 5 所需的偏移、内存补丁与 DHook Detour
 *
 * @return              是否初始化成功
 */
bool InitModule5_Combat()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 5] Error: Gamedata is not loaded, cannot initialize combat module!");
        return false;
    }

    // 1. 读取所需内部偏移
    g_iOffset_AttackShouldDodge   = GameConfGetOffset(g_hGameConf, "AttackState_ShouldDodge_Offset");
    g_iOffset_PinnedDownTimestamp = GameConfGetOffset(g_hGameConf, "AttackState_PinnedDownTimestamp_Offset");
    g_iOffset_MRetreatTimer       = GameConfGetOffset(g_hGameConf, "AttackState_MRetreatTimer_Offset");
    g_iOffset_TimerTimestamp      = GameConfGetOffset(g_hGameConf, "CountdownTimer_Timestamp_Offset");
    g_iOffset_TimerDuration       = GameConfGetOffset(g_hGameConf, "CountdownTimer_Duration_Offset");

    if (g_iOffset_AttackShouldDodge == -1 || g_iOffset_PinnedDownTimestamp == -1 ||
        g_iOffset_MRetreatTimer == -1 || g_iOffset_TimerTimestamp == -1 || g_iOffset_TimerDuration == -1)
    {
        LogError("[BotRouteFix] [Module 5] Failed to read one or more combat/retreat offsets from gamedata!");
        return false;
    }

    // 2. 准备并应用换弹掩体 100% NOP 补丁
    if (!PrepReloadHidePatch())
    {
        LogError("[BotRouteFix] [Module 5] Failed to inject reload hide 100%% patch!");
        return false;
    }

    // 3. 准备并挂载 AttackState::OnEnter Post Detour (闪避 100% 强制)
    if (!PrepAttackDodgeDetour())
    {
        LogError("[BotRouteFix] [Module 5] Failed to hook AttackState::OnEnter detour!");
        return false;
    }

    // 4. 准备并挂载 AttackState::OnUpdate Pre Detour (残血 <35 强制后撤)
    if (!PrepAttackRetreatDetour())
    {
        LogError("[BotRouteFix] [Module 5] Failed to hook AttackState::OnUpdate detour!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 5] Combat overhaul active (Dodge 100%% + Reload Hide 100%% + Low-HP Retreat @ %d HP) [%s]",
        HEALTH_RETREAT_THRESHOLD, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 还原模块 5 的所有内存补丁与动态拦截
 */
void RestoreModule5_Combat()
{
    // 还原换弹进掩体内存补丁
    g_PatchReloadHide.Restore();

    // 卸载并释放 AttackState::OnEnter Detour
    if (g_hAttackDodgeDetour != null)
    {
        DHookDisableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post);
        delete g_hAttackDodgeDetour;
        g_hAttackDodgeDetour = null;
    }

    // 卸载并释放 AttackState::OnUpdate Detour
    if (g_hAttackRetreatDetour != null)
    {
        DHookDisableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate_Pre);
        delete g_hAttackRetreatDetour;
        g_hAttackRetreatDetour = null;
    }

    for (int i = 0; i <= MaxClients; i++)
    {
        g_flNextForcedRetreat[i] = 0.0;
    }

    LogMessage("[BotRouteFix] [Module 5] Combat hooks and memory patches safely restored.");
}

/**
 * 动态启用或停用模块 5 补丁
 */
void EnableModule5_Combat(bool enable)
{
    if (enable)
    {
        PrepReloadHidePatch();
        if (g_hAttackDodgeDetour != null)
        {
            DHookEnableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post);
        }
        if (g_hAttackRetreatDetour != null)
        {
            DHookEnableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate_Pre);
        }
    }
    else
    {
        g_PatchReloadHide.Restore();
        if (g_hAttackDodgeDetour != null)
        {
            DHookDisableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post);
        }
        if (g_hAttackRetreatDetour != null)
        {
            DHookDisableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate_Pre);
        }
        for (int i = 0; i <= MaxClients; i++)
        {
            g_flNextForcedRetreat[i] = 0.0;
        }
    }
}

/**
 * 查询换弹进掩体补丁是否生效
 */
bool IsReloadHidePatched()
{
    return g_PatchReloadHide.isPatched;
}

/**
 * 查询闪避 Detour 是否已激活
 */
bool IsAttackDodgeHooked()
{
    return (g_hAttackDodgeDetour != null);
}

/**
 * 查询残血后撤 Detour 是否已激活
 */
bool IsAttackRetreatHooked()
{
    return (g_hAttackRetreatDetour != null);
}

/**
 * 获取累计强制闪避生效次数
 */
int GetTotalDodgeForced()
{
    return g_iTotalDodgeForced;
}

/**
 * 获取累计触发残血后撤的次数
 */
int GetTotalRetreatTriggered()
{
    return g_iTotalRetreatTriggered;
}

//========================================================================================
// PATCH: RELOAD HIDE 100% (NOP CONDITIONAL JUMP)
//========================================================================================

static bool PrepReloadHidePatch()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "ReloadCheck_HideChance");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 5] Failed to locate signature for ReloadCheck_HideChance!");
        return false;
    }

    int patchOffset = GameConfGetOffset(g_hGameConf, "ReloadHide_PatchOffset");
    if (patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 5] Failed to read ReloadHide_PatchOffset!");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);
    int patchSize = 2;

    g_PatchReloadHide.Init("ReloadHide_100Pct", targetAddr, patchSize);

    // 幂等性检测：检查是否已全部为 NOP (0x90)
    bool allNop = true;
    for (int i = 0; i < patchSize; i++)
    {
        if (LoadFromAddress(targetAddr + view_as<Address>(i), NumberType_Int8) != 0x90)
        {
            allNop = false;
            break;
        }
    }

    if (allNop)
    {
        g_PatchReloadHide.isPatched = false;
        LogMessage("[BotRouteFix] [Module 5] Reload hide patch already present @ %X, skipping", targetAddr);
        return true;
    }

    // 预检验证：严格比对跳转指令及相邻操作码 (32位: 76 <rel> A1, 64位: 73 <rel> 48 8B)
    int firstByte = LoadFromAddress(targetAddr, NumberType_Int8);
    bool isExpected = false;
    if (g_bIsWin64)
    {
        isExpected = (firstByte == 0x73
            && LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int8) == 0x48
            && LoadFromAddress(targetAddr + view_as<Address>(3), NumberType_Int8) == 0x8B);
    }
    else
    {
        isExpected = (firstByte == 0x76
            && LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int8) == 0xA1);
    }

    if (!isExpected)
    {
        LogError("[BotRouteFix] [Module 5] Reload hide template mismatch @ %X (expected %s, got %02X), patch aborted!",
            targetAddr, g_bIsWin64 ? "73 .. 48 8B" : "76 .. A1", firstByte);
        return false;
    }

    // NOP 掉该条件跳转，使掩体分支总是 fallthrough
    return g_PatchReloadHide.NopOut();
}

//========================================================================================
// DETOUR: ATTACKSTATE::ONENTER (POST HOOK FOR 100% DODGE)
//========================================================================================

static bool PrepAttackDodgeDetour()
{
    g_hAttackDodgeDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hAttackDodgeDetour == null)
    {
        LogError("[BotRouteFix] [Module 5] Failed to create DHook detour instance for AttackState_OnEnter!");
        return false;
    }

    if (!DHookSetFromConf(g_hAttackDodgeDetour, g_hGameConf, SDKConf_Signature, "AttackState_OnEnter"))
    {
        LogError("[BotRouteFix] [Module 5] Failed to set AttackState_OnEnter signature from gamedata!");
        delete g_hAttackDodgeDetour;
        g_hAttackDodgeDetour = null;
        return false;
    }

    DHookAddParam(g_hAttackDodgeDetour, HookParamType_CBaseEntity);

    if (!DHookEnableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post))
    {
        LogError("[BotRouteFix] [Module 5] Failed to enable AttackState_OnEnter Post detour!");
        delete g_hAttackDodgeDetour;
        g_hAttackDodgeDetour = null;
        return false;
    }

    return true;
}

public MRESReturn Hook_AttackState_OnEnter_Post(Address pThis, DHookParam hParams)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    if (g_iOffset_AttackShouldDodge <= 0)
    {
        return MRES_Ignored;
    }

    // 强制将 m_shouldDodge 置为 1 (true)，实现 100% 闪避决策
    StoreToAddress(pThis + view_as<Address>(g_iOffset_AttackShouldDodge), 1, NumberType_Int8);

    g_iTotalDodgeForced++;
    return MRES_Ignored;
}

//========================================================================================
// DETOUR: ATTACKSTATE::ONUPDATE (PRE HOOK FOR LOW-HEALTH FORCED RETREAT)
//========================================================================================

static bool PrepAttackRetreatDetour()
{
    g_hAttackRetreatDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hAttackRetreatDetour == null)
    {
        LogError("[BotRouteFix] [Module 5] Failed to create DHook detour instance for AttackState_OnUpdate!");
        return false;
    }

    if (!DHookSetFromConf(g_hAttackRetreatDetour, g_hGameConf, SDKConf_Signature, "AttackState_OnUpdate"))
    {
        LogError("[BotRouteFix] [Module 5] Failed to set AttackState_OnUpdate signature from gamedata!");
        delete g_hAttackRetreatDetour;
        g_hAttackRetreatDetour = null;
        return false;
    }

    DHookAddParam(g_hAttackRetreatDetour, HookParamType_CBaseEntity);

    if (!DHookEnableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate_Pre))
    {
        LogError("[BotRouteFix] [Module 5] Failed to enable AttackState_OnUpdate Pre detour!");
        delete g_hAttackRetreatDetour;
        g_hAttackRetreatDetour = null;
        return false;
    }

    return true;
}

public MRESReturn Hook_AttackState_OnUpdate_Pre(Address pThis, DHookParam hParams)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    if (g_iOffset_PinnedDownTimestamp <= 0 || g_iOffset_MRetreatTimer <= 0)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    // 检测血量是否低于阈值 (< 35 HP)
    if (GetClientHealth(client) >= HEALTH_RETREAT_THRESHOLD)
    {
        return MRES_Ignored;
    }

    // 节流阀：防范无掩体死角反复失败导致每 Tick 暴力清零触发昂贵 A* 寻路尖峰
    float curtime = GetGameTime();
    if (curtime < g_flNextForcedRetreat[client])
    {
        return MRES_Ignored;
    }
    g_flNextForcedRetreat[client] = curtime + RETREAT_THROTTLE_INTERVAL;

    // 1. 置 isPinnedDown 恒真 (curtime > 0.0) 触发撤退分支门控
    StoreToAddress(pThis + view_as<Address>(g_iOffset_PinnedDownTimestamp), view_as<int>(0.0), NumberType_Int32);

    // 2. 清除 m_retreatTimer 冷却时间戳，使 IsElapsed() 立即成立，达成残血立撤
    if (g_iOffset_TimerTimestamp > 0)
    {
        StoreToAddress(pThis + view_as<Address>(g_iOffset_MRetreatTimer + g_iOffset_TimerTimestamp),
            view_as<int>(0.0), NumberType_Int32);
        if (g_iOffset_TimerDuration > 0)
        {
            StoreToAddress(pThis + view_as<Address>(g_iOffset_MRetreatTimer + g_iOffset_TimerDuration),
                view_as<int>(0.0), NumberType_Int32);
        }
    }

    g_iTotalRetreatTriggered++;
    return MRES_Ignored;
}
