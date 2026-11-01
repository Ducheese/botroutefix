#if defined _botroutefix_module5_combat_included
    #endinput
#endif
#define _botroutefix_module5_combat_included

//========================================================================================
// [模块 5] 战斗决策重构 (身法闪避 100%、换弹进掩体 100%、残血/一打多/见狙威慑强制脱战)
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
//               往往无脑继续对枪直至暴毙。注意：引擎原生撤退段仍受敌人视线与技能判定门控，
//               且最终调用 `TryToRetreat()` 依赖掩体可达性。
//    - 解决机制: 在 `AttackState::OnUpdate` 的 Pre 钩子中，检测血量 < 35 HP 且距上次后撤尝试过 60s 冷却时:
//        * 将 `m_pinnedDownTimestamp` 写为 0.0，使 `isPinnedDown` (curtime > 0.0) 恒真；
//        * 将 `m_retreatTimer` 冷却清空，使 `IsElapsed()` 立即成立；
//        强行撕开原版交火时长与退避冷却的硬性封锁，促使本次 OnUpdate 的原生撤退段立即执行 `TryToRetreat()` 尝试后撤，
//        成功即转入 `HideState`，杜绝残血时因计时器死锁而在开阔地白给。
//
//    ★ 后撤档位三档制 (2026-10-31 重构，豁免语义唯一裁决点在本模块；M11 只发请求):
//        硬豁免 → 不施加强制后撤，直接放行交火（使命动作的临界态；引擎原生撤退判定仍保留）；
//        软豁免 → 照常后撤，但经 M15 `HideDuration_RequestShort` 把蹲守压到 1 秒（到点即起）；
//        放行   → 照常后撤，Hide 时长交由 M15 按士气缩放。
//      档位矩阵（触发源 × 身份）:
//        下包前 (T 有使命):
//          包匪 `PLANT_BOMB && m_bInBombZone` → 硬；包匪其他情况 / M9 捡包名册 / M7 保镖 → 软；
//          其他 T → 放行。
//        下包后 (CT 有使命):
//          `Task == DEFUSE_BOMB` → 硬；其他 CT → 软；T 全员 → 放行。
//        狙击威胁:
//          T 下包前包点内包匪 → 硬，其余 T → 软；T 下包后放行；
//          CT 按拆包任务档（DEFUSE_BOMB 硬、其他软）；CT 下包前放行。
//      注: 包匪硬豁免的 `m_bInBombZone` 由 func_bomb_target 仅对持包者置位，
//          天然等价"持包者已进入包点触区"；CT 硬豁免以拆包任务为准——
//          M10 开启时仅首席持有该任务，M10 关闭/降级时全员原生持有，自动 fail-open
//          （例外：bot_defer_to_human 开且队内有活人时 bot 可能拿不到该任务，回落软豁免）。
//
// 4. 孤身一打多强制后撤 (Outnumbered Forced Retreat via SDKCall):
//    - 源码逻辑 (cs_bot.cpp:606-621): `IsOutnumbered() = 友 < 敌-1`（自己不占名额，1v2 起算），
//      计数读 3 秒视觉记忆表 `m_nearbyFriendCount/m_nearbyEnemyCount`，非距离圈；原生走 c 支骰子/aggr 全绕过。
//    - 解决机制: 同一 Pre 钩子里直调 `CCSBot::IsOutnumbered`，确认被包后再读原生 `m_nearbyFriendCount`，
//      视野内有队友（2v3/2v4）放行交火，仅计数为 0 的真 1v 多才写同一两处焊门内存；完全不走节流阀。
//      档位与残血共用上面的矩阵。
//
// 5. 狙击手威慑脱战 (Sniper Deterrence Disengage via Module 11):
//    - 跨模块联动接口: 开放 `Combat_RequestSniperRetreat(client)`，由模块 11 见狙威慑触发；
//    - 请求带 2 秒 TTL：bot 未在窗口内进入 AttackState 则自动作废，杜绝跨状态残影误触发；
//    - 穿透机制: 命中请求时完全穿透 35 HP 残血门控与节流阀，赋予单拍立即后撤特权；
//    - 档位由本模块统一裁决（见上矩阵），M11 不再自己判任务/包点豁免。
//========================================================================================

#define HEALTH_RETREAT_THRESHOLD    35      // 残血强制后撤生命值阈值
#define RETREAT_THROTTLE_INTERVAL   60.0    // 残血脱战后撤节流间隔 (秒)
#define BOT_TASK_PLANT_BOMB         1       // 引擎原生任务枚举：PLANT_BOMB (下包任务)
#define BOT_TASK_DEFUSE_BOMB        3       // 引擎原生任务枚举：DEFUSE_BOMB (拆包任务)
#define SNIPER_RETREAT_REQUEST_TTL  2.0     // 狙击撤退请求有效期 (秒)

// 后撤触发原因枚举
#define RETREAT_REASON_NONE         0
#define RETREAT_REASON_LOW_HP       1       // 残血脱战
#define RETREAT_REASON_OUTNUMBERED  2       // 孤身一打多
#define RETREAT_REASON_SNIPER       3       // 狙击手威慑

// 模块状态、内存补丁与 Detour 句柄
MemoryPatch g_PatchReloadHide;
Handle g_hAttackDodgeDetour = null;
Handle g_hAttackRetreatDetour = null;
int g_iTotalDodgeForced = 0;
int g_iTotalRetreatTriggered = 0;

// 业务开启态与 per-hook 真实启用态（DHook 状态机唯一事实来源）
static bool g_bModule5_Active = false;
static bool g_bDodgeHooked = false;
static bool g_bRetreatHooked = false;

// 模块内部专用冷却、偏移与可视化追踪缓存
static float g_flNextForcedRetreat[MAXPLAYERS + 1];
static int g_iRetreatReason[MAXPLAYERS + 1];
static float g_flRetreatUntil[MAXPLAYERS + 1];
static int g_iOffset_AttackShouldDodge = -1;
static int g_iOffset_PinnedDownTimestamp = -1;
static int g_iOffset_MRetreatTimer = -1;
static int g_iOffset_TimerTimestamp = -1;
static int g_iOffset_TimerDuration = -1;
static int g_iOffset_NearbyFriendCount = -1;
static int g_iOffset_Task = -1;

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
    g_iOffset_NearbyFriendCount   = GameConfGetOffset(g_hGameConf, "CCSBot_NearbyFriendCount_Offset");
    g_iOffset_Task                = GameConfGetOffset(g_hGameConf, "CCSBot_Task_Offset");

    if (g_iOffset_AttackShouldDodge == -1 || g_iOffset_PinnedDownTimestamp == -1 ||
        g_iOffset_MRetreatTimer == -1 || g_iOffset_TimerTimestamp == -1 || g_iOffset_TimerDuration == -1 ||
        g_iOffset_NearbyFriendCount == -1 || g_iOffset_Task == -1)
    {
        LogError("[BotRouteFix] [Module 5] Failed to read one or more combat/retreat offsets from gamedata!");
        return false;
    }

    // 2. 挂载 AttackState::OnEnter Post Detour (闪避 100% 强制)
    if (!MountAttackDodgeDetour())
    {
        LogError("[BotRouteFix] [Module 5] Failed to hook AttackState::OnEnter detour!");
        return false;
    }

    // 3. 挂载 AttackState::OnUpdate Pre Detour (残血 <35 强制后撤)
    if (!MountAttackRetreatDetour())
    {
        LogError("[BotRouteFix] [Module 5] Failed to hook AttackState::OnUpdate detour!");
        delete g_hAttackDodgeDetour;
        g_hAttackDodgeDetour = null;
        return false;
    }

    // 4. 套用业务态（补丁 + 双钩子全成或全败）
    EnableModule5_Combat(true);
    if (!g_bModule5_Active)
    {
        delete g_hAttackDodgeDetour;
        delete g_hAttackRetreatDetour;
        g_hAttackDodgeDetour = null;
        g_hAttackRetreatDetour = null;
        LogError("[BotRouteFix] [Module 5] Failed to activate combat overhaul!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 5] Combat overhaul active (Dodge 100%% + Reload Hide 100%% + Low-HP Retreat @ %d HP + Outnumbered Retreat) [%s]",
        HEALTH_RETREAT_THRESHOLD, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 动态启用或停用模块 5（幂等；补丁 + 两钩子同步收敛）
 */
void EnableModule5_Combat(bool enable)
{
    if (enable == g_bModule5_Active)
    {
        return;
    }

    if (!enable)
    {
        SyncDetourEnabled(g_hAttackDodgeDetour, g_bDodgeHooked, true, Hook_AttackState_OnEnter_Post, false, "Module 5");
        SyncDetourEnabled(g_hAttackRetreatDetour, g_bRetreatHooked, false, Hook_AttackState_OnUpdate_Pre, false, "Module 5");
        g_PatchReloadHide.Restore();
        for (int i = 0; i <= MaxClients; i++)
        {
            g_flNextForcedRetreat[i] = 0.0;
        }
        g_bModule5_Active = false;
        return;
    }

    if (!PrepReloadHidePatch())
    {
        LogError("[BotRouteFix] [Module 5] Reload hide patch failed, combat module stays inactive!");
        return;
    }

    bool bDodge   = SyncDetourEnabled(g_hAttackDodgeDetour, g_bDodgeHooked, true, Hook_AttackState_OnEnter_Post, true, "Module 5");
    bool bRetreat = SyncDetourEnabled(g_hAttackRetreatDetour, g_bRetreatHooked, false, Hook_AttackState_OnUpdate_Pre, true, "Module 5");

    if (!bDodge || !bRetreat)
    {
        SyncDetourEnabled(g_hAttackDodgeDetour, g_bDodgeHooked, true, Hook_AttackState_OnEnter_Post, false, "Module 5");
        SyncDetourEnabled(g_hAttackRetreatDetour, g_bRetreatHooked, false, Hook_AttackState_OnUpdate_Pre, false, "Module 5");
        g_PatchReloadHide.Restore();
        g_bModule5_Active = false;
        LogError("[BotRouteFix] [Module 5] Enable aborted (dodge=%d, retreat=%d), all resources rolled back!", bDodge, bRetreat);
        return;
    }

    g_bModule5_Active = true;
}

/**
 * 还原模块 5（插件卸载：摘钩并释放句柄）
 */
void RestoreModule5_Combat()
{
    EnableModule5_Combat(false);

    if (g_hAttackDodgeDetour != null)
    {
        delete g_hAttackDodgeDetour;
        g_hAttackDodgeDetour = null;
    }

    if (g_hAttackRetreatDetour != null)
    {
        delete g_hAttackRetreatDetour;
        g_hAttackRetreatDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 5] Combat hooks and memory patches safely restored.");
}

/**
 * 查询模块 5 业务开启态
 */
bool IsModule5_Active()
{
    return g_bModule5_Active;
}

static bool g_bSniperRetreatWanted[MAXPLAYERS + 1];
static float g_flSniperRetreatUntil[MAXPLAYERS + 1];

/**
 * 模块 11 联动接口：请求对指定 Bot 发起狙击手强制撤退（带 TTL）
 */
void Combat_RequestSniperRetreat(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_bSniperRetreatWanted[client] = true;
        g_flSniperRetreatUntil[client] = GetGameTime() + SNIPER_RETREAT_REQUEST_TTL;
        g_flNextForcedRetreat[client] = 0.0;
    }
}

/**
 * 回合开始 (OnRoundStart)：新回合清空残留后撤请求与节流时间戳
 */
void Module5_OnRoundStart()
{
    for (int i = 0; i <= MaxClients; i++)
    {
        g_flNextForcedRetreat[i] = 0.0;
        g_bSniperRetreatWanted[i] = false;
        g_flSniperRetreatUntil[i] = 0.0;
        g_iRetreatReason[i] = RETREAT_REASON_NONE;
        g_flRetreatUntil[i] = 0.0;
    }
}

/**
 * 玩家阵亡 (OnPlayerDeath)：阵亡玩家清空后撤请求
 */
void Module5_OnPlayerDeath(int victim)
{
    if (victim > 0 && victim <= MaxClients)
    {
        g_flNextForcedRetreat[victim] = 0.0;
        g_bSniperRetreatWanted[victim] = false;
        g_flSniperRetreatUntil[victim] = 0.0;
        g_iRetreatReason[victim] = RETREAT_REASON_NONE;
        g_flRetreatUntil[victim] = 0.0;
    }
}

/**
 * 地图加载 (OnMapStart)：curtime 归零，旧未来时间戳会误节流新图后撤，清节流数组
 */
void Module5_OnMapStart()
{
    for (int i = 0; i <= MaxClients; i++)
    {
        g_flNextForcedRetreat[i] = 0.0;
        g_bSniperRetreatWanted[i] = false;
        g_flSniperRetreatUntil[i] = 0.0;
        g_iRetreatReason[i] = RETREAT_REASON_NONE;
        g_flRetreatUntil[i] = 0.0;
    }
}

/**
 * 客户端进服 (OnClientPutInServer)：槽位复用，旧主人的节流时间戳不能留给新人
 */
void Module5_OnClientPutInServer(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_flNextForcedRetreat[client] = 0.0;
        g_bSniperRetreatWanted[client] = false;
        g_flSniperRetreatUntil[client] = 0.0;
        g_iRetreatReason[client] = RETREAT_REASON_NONE;
        g_flRetreatUntil[client] = 0.0;
    }
}

/**
 * 客户端断开 (OnClientDisconnect)：清空玩家状态
 */
void Module5_OnClientDisconnect(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_flNextForcedRetreat[client] = 0.0;
        g_bSniperRetreatWanted[client] = false;
        g_flSniperRetreatUntil[client] = 0.0;
        g_iRetreatReason[client] = RETREAT_REASON_NONE;
        g_flRetreatUntil[client] = 0.0;
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
    return (g_hAttackDodgeDetour != null && g_bDodgeHooked);
}

/**
 * 查询残血/一打多后撤 Detour 与 IsOutnumbered SDKCall 是否完全就绪
 */
bool IsAttackRetreatHooked()
{
    return (g_hAttackRetreatDetour != null && g_bRetreatHooked && g_hSDKCall_IsOutnumbered != null);
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
        // 有备份 = 自家补丁，保持旗标；无备份 = 外来已旁路，记 false（无基线可还原）
        if (!g_PatchReloadHide.hasBackup)
        {
            g_PatchReloadHide.isPatched = false;
        }
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

static bool MountAttackDodgeDetour()
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

    return true;
}

public MRESReturn Hook_AttackState_OnEnter_Post(Address pThis, DHookParam hParams)
{
    if (!g_bModule5_Active)
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
    // DebugLog("[BotRouteFix] [Module 5] Bot %N forced 100%% dodge maneuver", client);
    return MRES_Ignored;
}

//========================================================================================
// DETOUR: ATTACKSTATE::ONUPDATE (PRE HOOK FOR LOW-HEALTH FORCED RETREAT)
//========================================================================================

static bool MountAttackRetreatDetour()
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

    return true;
}

public MRESReturn Hook_AttackState_OnUpdate_Pre(Address pThis, DHookParam hParams)
{
    if (!g_bModule5_Active)
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    if (g_iOffset_PinnedDownTimestamp <= 0 || g_iOffset_MRetreatTimer <= 0 || g_iOffset_NearbyFriendCount <= 0)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return MRES_Ignored;
    }

    int team = GetClientTeam(client);
    if (team != 2 && team != 3)
    {
        return MRES_Ignored;
    }

    // 狙击威慑请求消费（TTL 2 秒，过期即作废，杜绝跨状态残影）
    bool bSniperRetreat = false;
    if (g_bSniperRetreatWanted[client])
    {
        g_bSniperRetreatWanted[client] = false;
        bSniperRetreat = (GetGameTime() <= g_flSniperRetreatUntil[client]);
        g_flSniperRetreatUntil[client] = 0.0;
    }

    // 硬豁免：使命动作的临界态
    if (team == 2)
    {
        // 持包者已进入包点触区（m_bInBombZone 仅对持包者置位）且处于 PLANT_BOMB = 下一拍下包
        // 三大触发源（残血/一打多/狙击威胁）一律同构：命中即硬豁免
        if (g_iOffset_Task > 0
            && LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == BOT_TASK_PLANT_BOMB
            && GetEntProp(client, Prop_Send, "m_bInBombZone") != 0)
        {
            return MRES_Ignored;
        }
    }
    else
    {
        // CT 拆包使命：DEFUSE_BOMB 任务即硬豁免
        // 注：M10 开启时只有首席拿此任务；M10 关闭/降级时全员原生拿 DEFUSE_BOMB，
        //     硬豁免随任务自动 fail-open，不会出现"无首席即无硬豁免"的真空
        if (g_iOffset_Task > 0
            && LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == BOT_TASK_DEFUSE_BOMB)
        {
            return MRES_Ignored;
        }
    }

    // 判定后撤触发源（优先级：狙击威慑 > 孤身一打多 > 残血脱战）：
    bool bOutnumbered = false;
    bool bLowHealth = false;

    if (!bSniperRetreat)
    {
        int nearbyFriends = LoadFromAddress(pBot + view_as<Address>(g_iOffset_NearbyFriendCount), NumberType_Int32);

        // 孤身一打多紧急态：直调原生 IsOutnumbered，仅在确认被包且视野内零队友 (1v多) 时触发
        if (BotIsOutnumbered(client) && nearbyFriends == 0)
        {
            bOutnumbered = true;
        }
        // 常规残血态：血量低于阈值 (< 35 HP)
        else if (GetClientHealth(client) < HEALTH_RETREAT_THRESHOLD)
        {
            bLowHealth = true;
        }
        else
        {
            return MRES_Ignored;
        }
    }

    // 节流阀：只有纯残血后撤要走 60 秒长节流；高危紧急态（狙击威胁、孤身一对多）完全不走节流
    if (bLowHealth)
    {
        float curtime = GetGameTime();
        if (curtime < g_flNextForcedRetreat[client])
        {
            return MRES_Ignored;
        }
        g_flNextForcedRetreat[client] = curtime + RETREAT_THROTTLE_INTERVAL;
    }

    // 软豁免：只压缩蹲守时长，后撤动作完整保留
    bool bShortHide = false;
    if (team == 3)
    {
        // CT：下包前无使命放行；下包后非拆包临界态一律短蹲
        bShortHide = g_bBombPlanted;
    }
    else if (bSniperRetreat)
    {
        // T 见狙：下包前短蹲（包点内包匪已被上面硬豁免拦下）；下包后放行
        bShortHide = !g_bBombPlanted;
    }
    else if (!g_bBombPlanted)
    {
        // T 残血/一打多（下包前）：包匪、M9 捡包名册、M7 保镖短蹲；其他 T 放行
        bShortHide = (client == GetC4Carrier())
            || Loot_IsInRoster(client)
            || Escort_IsBodyguard(client);
    }

    if (bShortHide)
    {
        HideDuration_RequestShort(client);
    }

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

    // 记录后撤原因与高亮维持时长 (4.0 秒)，供可视化系统呈现
    if (bSniperRetreat)
    {
        g_iRetreatReason[client] = RETREAT_REASON_SNIPER;
    }
    else if (bOutnumbered)
    {
        g_iRetreatReason[client] = RETREAT_REASON_OUTNUMBERED;
    }
    else if (bLowHealth)
    {
        g_iRetreatReason[client] = RETREAT_REASON_LOW_HP;
    }
    g_flRetreatUntil[client] = GetGameTime() + 4.0;

    // DebugLog("[BotRouteFix] [Module 5] Low-HP bot %N (HP: %d) forced retreat via pinnedDown",
    //     client, GetClientHealth(client));
    if (bOutnumbered)
    {
        // DebugLog("[BotRouteFix] [Module 5] Outnumbered bot %N forced retreat via confirm (no dice)", client);
    }
    return MRES_Ignored;
}

/**
 * 获取 Bot 当前后撤原因枚举 (供可视化子系统渲染特效)
 *
 * @param client        客户端索引
 * @return              RETREAT_REASON_* (若未处于后撤或超时则返回 RETREAT_REASON_NONE)
 */
int Module5_GetRetreatReason(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
    {
        return RETREAT_REASON_NONE;
    }

    if (GetGameTime() < g_flRetreatUntil[client])
    {
        return g_iRetreatReason[client];
    }
    return RETREAT_REASON_NONE;
}
