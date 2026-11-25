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
//        软豁免 → 照常后撤，但经 M15 `Module15_RequestShortHide` 把蹲守压到 1 秒（到点即起）；
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
//          M10 开启时仅 Top-3 拆包名册成员（及尚未进 750 码的远端 CT）持有该任务，
//          M10 关闭/FAILED 时全员原生持有，这是原生任务语义，
//          不是 M10 fail-open（例外：bot_defer_to_human 开且队内有活人时 bot 可能拿不到该任务，
//          回落软豁免）。
//
// 4. 孤身一打多强制后撤 (Outnumbered Forced Retreat via SDKCall):
//    - 源码逻辑 (cs_bot.cpp:606-621): `IsOutnumbered() = 友 < 敌-1`（自己不占名额，1v2 起算），
//      计数读 3 秒视觉记忆表 `m_nearbyFriendCount/m_nearbyEnemyCount`，非距离圈；原生走 c 支骰子/aggr 全绕过。
//    - 解决机制: 同一 Pre 钩子里直调 `CCSBot::IsOutnumbered`，确认被包后再读原生 `m_nearbyFriendCount`，
//      视野内有队友（2v3/2v4）放行交火，仅计数为 0 的真 1v 多才写同一两处焊门内存；完全不走节流阀。
//      档位与残血共用上面的矩阵。
//
// 5. 狙击手威慑脱战 (Sniper Deterrence Disengage via Module 11):
//    - 跨模块联动接口: 开放 `Module5_RequestSniperRetreat(client)`，由模块 11 见狙威慑触发；
//    - 请求带 2 秒 TTL：bot 未在窗口内进入 AttackState 则自动作废，杜绝跨状态残影误触发；
//    - 穿透机制: 命中请求时完全穿透 35 HP 残血门控与节流阀，赋予单拍立即后撤特权；
//    - 档位由本模块统一裁决（见上矩阵），M11 不再自己判任务/包点豁免。
//========================================================================================

#define HEALTH_RETREAT_THRESHOLD    35      // 残血强制后撤生命值阈值
#define RETREAT_THROTTLE_INTERVAL   60.0    // 残血脱战后撤节流间隔 (秒)
#define SNIPER_RETREAT_REQUEST_TTL  2.0     // 狙击撤退请求有效期 (秒)

//========================================================================================
// MODULE 5 DEFINES & GLOBALS
//========================================================================================

// 后撤触发原因枚举
#define RETREAT_REASON_NONE         0
#define RETREAT_REASON_LOW_HP       1       // 残血脱战
#define RETREAT_REASON_OUTNUMBERED  2       // 孤身一打多
#define RETREAT_REASON_SNIPER       3       // 狙击手威慑

// 模块状态、内存补丁与 Detour 句柄
static MemoryPatch g_PatchReloadHide;
static Handle g_hAttackDodgeDetour = null;
static Handle g_hAttackRetreatDetour = null;
static int g_iTotalDodgeForced = 0;
static int g_iTotalRetreatTriggered = 0;

// 模块内部启用缓存（仅用于 Enable 幂等短路；逐事项判据统一用 Item_IsArmed）
static bool g_bModule5_Active = false;
static bool g_bDodgeHooked = false;
static bool g_bRetreatHooked = false;

// 狙击撤退跨模块请求状态（TTL 窗口由后撤事项的 Pre 钩子消费）
static bool g_bSniperRetreatWanted[MAXPLAYERS + 1];
static float g_flSniperRetreatUntil[MAXPLAYERS + 1];

// 模块内部专用冷却、偏移与可视化追踪缓存
static float g_flNextForcedRetreat[MAXPLAYERS + 1];
static int g_iRetreatReason[MAXPLAYERS + 1];
static float g_flRetreatUntil[MAXPLAYERS + 1];
static int g_iOffset_AttackShouldDodge = -1;
static int g_iOffset_PinnedDownTimestamp = -1;
static int g_iOffset_MRetreatTimer = -1;
// Timer/Task 上收共享（Timer→nav_measure.inc，Task→global.inc），本文件只用不存
static int g_iOffset_NearbyFriendCount = -1;

//========================================================================================
// MODULE 5 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：换弹掩体 NOP
 *
 * 只定位目标、校验模板并确认可备份，不写入 NOP（写入由 Apply 执行，M4 式分工）。
 * 外来已 NOP 但无基线时直接标事项 FAILED，不允许无基线假装可用。
 * 任一后端失败都返回 false，Init 会把 NOP 事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = NOP 事项后端就绪
 */
static bool PrepModule5_ReloadBackend()
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
            LogError("[BotRouteFix] [Module 5] Reload hide already NOP without baseline @ %X; reload item FAILED.",
                targetAddr);
            return false;
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

    LogMessage("[BotRouteFix] [Module 5] Reload hide block verified @ %X [%s]",
        targetAddr, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 事项后端：闪避 dhook 写（ITEM_M5_DODGE）
 *
 * 条件 = m_shouldDodge 偏移 + AttackState::OnEnter Detour 挂载。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 闪避事项后端就绪
 */
static bool PrepModule5_DodgeBackend()
{
    g_iOffset_AttackShouldDodge = GameConfGetOffset(g_hGameConf, "AttackState_ShouldDodge_Offset");
    if (g_iOffset_AttackShouldDodge <= 0)
    {
        LogError("[BotRouteFix] [Module 5] ShouldDodge offset missing; dodge item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 5] Dodge offsets verified (shoulddodge=%d).",
        g_iOffset_AttackShouldDodge);

    if (!MountAttackDodgeDetour())
    {
        LogError("[BotRouteFix] [Module 5] AttackState::OnEnter hook unavailable; dodge item FAILED.");
        return false;
    }

    return true;
}

/**
 * 挂载 AttackState::OnEnter Detour（事项 M5 闪避的钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，闪避事项 FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
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

/**
 * 事项后端：后撤 dhook 写
 *
 * 条件 = 全部后撤偏移 + AttackState::OnUpdate Detour 挂载 + IsOutnumbered SDKCall。
 * 任一后端缺失整条后撤停，不允许只跑残血分支。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 后撤事项后端就绪
 */
static bool PrepModule5_RetreatBackend()
{
    g_iOffset_PinnedDownTimestamp = GameConfGetOffset(g_hGameConf, "AttackState_PinnedDownTimestamp_Offset");
    g_iOffset_MRetreatTimer       = GameConfGetOffset(g_hGameConf, "AttackState_MRetreatTimer_Offset");
    g_iOffset_NearbyFriendCount   = GameConfGetOffset(g_hGameConf, "CCSBot_NearbyFriendCount_Offset");
    // Timer/Task 由 InitNavMeasure / OnPluginStart 统一解析，此处只做门禁

    bool retreatOffsetsOk = (g_iOffset_PinnedDownTimestamp > 0 && g_iOffset_MRetreatTimer > 0
        && g_iOffset_TimerTimestamp > 0 && g_iOffset_TimerDuration > 0
        && g_iOffset_NearbyFriendCount > 0 && g_iOffset_Task > 0);
    if (!retreatOffsetsOk)
    {
        LogError("[BotRouteFix] [Module 5] One or more retreat offsets missing; retreat item FAILED.");
        return false;
    }

    if (g_hSDKCall_IsOutnumbered == null)
    {
        LogError("[BotRouteFix] [Module 5] IsOutnumbered SDKCall missing; retreat item FAILED.");
        return false;
    }

    if (!MountAttackRetreatDetour())
    {
        LogError("[BotRouteFix] [Module 5] AttackState::OnUpdate hook unavailable; retreat item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 5] Retreat backend verified (pinned=%d, mretreat=%d, ts=%d, dur=%d, friends=%d, task=%d, outnumbered SDKCall).",
        g_iOffset_PinnedDownTimestamp, g_iOffset_MRetreatTimer, g_iOffset_TimerTimestamp,
        g_iOffset_TimerDuration, g_iOffset_NearbyFriendCount, g_iOffset_Task);
    return true;
}

/**
 * 挂载 AttackState::OnUpdate Detour（事项 M5 后撤的钩子载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false，后撤事项 FAILED。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
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

//========================================================================================
// MODULE 5 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 5 的三个独立事项：换弹掩体 NOP（ITEM_M5_RELOAD）/ 闪避 dhook 写（ITEM_M5_DODGE）/ 后撤（整体，ITEM_M5_RETREAT）
 *
 * 分别准备三条事项链并立即武装；任一失败不拖累其他两条。
 * 失败语义：全灭时 LogModuleAllFailed + return false；部分成功按事项各自 FAILED，不连带。
 *
 * @return              true = 至少一个事项武装成功
 */
bool InitModule5_Combat()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 5] Gamedata missing; all items FAILED.");
        Item_SetReady(ITEM_M5_RELOAD, false);
        Item_SetReady(ITEM_M5_DODGE, false);
        Item_SetReady(ITEM_M5_RETREAT, false);
        return false;
    }

    // 1-3. 三个事项的后端准备（偏移/挂钩/SDK 缺一即对应事项 FAILED，互不连带）
    bool reloadReady = PrepModule5_ReloadBackend();
    Item_SetReady(ITEM_M5_RELOAD, reloadReady);

    bool dodgeReady = PrepModule5_DodgeBackend();
    Item_SetReady(ITEM_M5_DODGE, dodgeReady);

    bool retreatReady = PrepModule5_RetreatBackend();
    Item_SetReady(ITEM_M5_RETREAT, retreatReady);

    // 4. 套用业务态（三个事项独立武装）
    EnableModule5_Combat(true);
    if (!Item_IsArmed(ITEM_M5_RELOAD) && !Item_IsArmed(ITEM_M5_DODGE) && !Item_IsArmed(ITEM_M5_RETREAT))
    {
        LogModuleAllFailed(5);
        return false;
    }

    char armedDetail[64];
    Format(armedDetail, sizeof(armedDetail), "reload=%d, dodge=%d, retreat=%d",
        Item_IsArmed(ITEM_M5_RELOAD), Item_IsArmed(ITEM_M5_DODGE), Item_IsArmed(ITEM_M5_RETREAT));
    LogModuleArmed(5, "Combat", armedDetail);
    return true;
}

/**
 * 动态启用或停用模块 5（事项 ITEM_M5_RELOAD / ITEM_M5_DODGE / ITEM_M5_RETREAT 的调度入口；幂等；三个事项分别武装、分别还原，互不连带）
 *
 * 只做调度：只武装 Ready 的事项；Active = OR(Armed)。
 * 失败语义：无返回值；Apply 内部失败只清对应事项 Armed。
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule5_Combat(bool enable)
{
    if (enable == g_bModule5_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule5_ReloadBackend();
        RestoreModule5_DodgeBackend();
        RestoreModule5_RetreatBackend();
        for (int i = 0; i <= MaxClients; i++)
        {
            g_flNextForcedRetreat[i] = 0.0;
        }
        g_bModule5_Active = false;
        return;
    }

    // ITEM_M5_RELOAD / ITEM_M5_DODGE / ITEM_M5_RETREAT 独立武装；任一事项武装即视为模块部分激活
    bool reload  = ApplyModule5_ReloadBackend();
    bool dodge   = ApplyModule5_DodgeBackend();
    bool retreat = ApplyModule5_RetreatBackend();
    g_bModule5_Active = (reload || dodge || retreat);

    if (g_bModule5_Active && (!reload || !dodge || !retreat))
    {
        char partialDetail[96];
        Format(partialDetail, sizeof(partialDetail), "reload=%d, dodge=%d, retreat=%d", reload, dodge, retreat);
        LogModulePartial(5, partialDetail);
    }
}

/**
 * 还原模块 5（事项 ITEM_M5_RELOAD / ITEM_M5_DODGE / ITEM_M5_RETREAT；插件卸载）
 *
 * 逐事项还原后释放 Detour 句柄；NOP 补丁与钩子全部复位。
 * 失败语义：无（尽力还原；幂等）。
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


//========================================================================================
// MODULE 5 PUBLIC GETTERS & TELEMETRY
//========================================================================================


/**
 * 查询累计强制闪避生效次数
 *
 * @return              自插件加载以来的累计闪避次数
 */
int Module5_GetTotalDodgeForced()
{
    return g_iTotalDodgeForced;
}

/**
 * 查询累计触发后撤的次数
 *
 * @return              自插件加载以来的累计后撤次数
 */
int Module5_GetTotalRetreatTriggered()
{
    return g_iTotalRetreatTriggered;
}

/**
 * 获取 Bot 当前后撤原因 (供可视化子系统渲染特效)
 *
 * 跨模块只读 getter，不参与本模块事项状态。
 *
 * @param client        客户端索引
 * @return              RETREAT_REASON_* (若未处于后撤或超时则返回 RETREAT_REASON_NONE)
 */
int Module5_GetRetreatReason(int client)
{
    if (!IsValidClient(client, true))
    {
        return RETREAT_REASON_NONE;
    }

    if (GetGameTime() < g_flRetreatUntil[client])
    {
        return g_iRetreatReason[client];
    }
    return RETREAT_REASON_NONE;
}

//========================================================================================
// MODULE 5 EVENT HANDLERS
//========================================================================================

/**
 * 回合开始 (OnRoundStart)：新回合清空残留后撤请求与节流时间戳
 *
 * 只清理跨回合状态，不执行战术行为；不受事项武装态门控。
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

//========================================================================================
// MODULE 5 DETOUR & HOOKS (ATTACKSTATE::ONENTER, POST FOR 100% DODGE)
//========================================================================================

/**
 * AttackState::OnEnter Post：强制 100% 闪避决策
 *
 * 事项 M5 闪避 dhook 写的执行点；只在闪避事项武装时写入。
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * @param pThis         AttackState 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_AttackState_OnEnter_Post(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M5_DODGE))
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
// MODULE 5 DETOUR & HOOKS (ATTACKSTATE::ONUPDATE, PRE FOR FORCED RETREAT)
//========================================================================================

/**
 * AttackState::OnUpdate Pre：残血 / 一打多 / 狙击威胁强制后撤
 *
 * 事项 M5 后撤（整体）的执行点；只在后撤事项武装时写入。
 * 触发源优先级：狙击威慑 > 孤身一打多 > 残血；档位由本模块统一裁决。
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * @param pThis         AttackState 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_AttackState_OnUpdate_Pre(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M5_RETREAT))
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
        // 注：M10 开启时仅 Top-3 拆包名册成员（及尚未进 750 码的远端 CT）持有该任务；
        //     M10 关闭/FAILED 时全员原生持有 DEFUSE_BOMB，
        //     这是原生任务语义（非 M10 fail-open），不会出现"无名册即无硬豁免"的真空
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
            || Module9_IsInRoster(client)
            || Module7_IsBodyguard(client);
    }

    if (bShortHide)
    {
        Module15_RequestShortHide(client);
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

//========================================================================================
// MODULE 5 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 事项 M5 后撤的跨模块请求入口：登记狙击手强制撤退请求（带 TTL）
 *
 * 只登记请求，不改变本模块事项状态；请求是否生效由后撤 Pre 钩子在 TTL 窗口内消费时判定。
 * 未武装时后撤钩子不启用，请求自然不会被消费，无需在此另行门控。
 *
 * @param client        目标 Bot 客户端索引
 */
void Module5_RequestSniperRetreat(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_bSniperRetreatWanted[client] = true;
        g_flSniperRetreatUntil[client] = GetGameTime() + SNIPER_RETREAT_REQUEST_TTL;
        g_flNextForcedRetreat[client] = 0.0;
    }
}

/**
 * 应用换弹掩体 NOP 事项：只有 Prep 已就绪才写入 NOP
 *
 * 事项 M5 换弹掩体 NOP 的应用入口；不做定位与校验（那是 Prep 的职责）。
 * 失败时该事项 FAILED，另两项不受影响。
 * 失败语义：写入失败同步清 Ready 并回滚，事项 FAILED，另两项不受影响。
 *
 * @return              true = NOP 已驻留
 */
static bool ApplyModule5_ReloadBackend()
{
    if (!Item_IsReady(ITEM_M5_RELOAD) || g_PatchReloadHide.address == Address_Null)
    {
        Item_SetArmed(ITEM_M5_RELOAD, false);
        return false;
    }

    if (!g_PatchReloadHide.isPatched)
    {
        // NOP 穿透该条件跳转，使掩体分支总是 fallthrough
        if (!g_PatchReloadHide.NopOut())
        {
            Item_SetReady(ITEM_M5_RELOAD, false);
            Item_SetArmed(ITEM_M5_RELOAD, false);
            LogError("[BotRouteFix] [Module 5] Reload hide NOP write failed; reload item FAILED.");
            return false;
        }
    }

    Item_SetArmed(ITEM_M5_RELOAD, true);
    return true;
}

/**
 * 应用闪避事项：启用 AttackState::OnEnter Post 钩子
 *
 * 事项 M5 闪避 dhook 写的唯一应用入口；失败时该事项 FAILED，另两项不受影响。
 * 失败语义：钩子启用失败只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = Post 钩子已启用
 */
static bool ApplyModule5_DodgeBackend()
{
    if (!Item_IsReady(ITEM_M5_DODGE) || g_hAttackDodgeDetour == null)
    {
        Item_SetArmed(ITEM_M5_DODGE, false);
        return false;
    }

    bool ok = SyncDetourEnabled(g_hAttackDodgeDetour, g_bDodgeHooked,
        true, Hook_AttackState_OnEnter_Post, true, "Module 5");
    Item_SetArmed(ITEM_M5_DODGE, ok);
    return ok;
}

/**
 * 应用后撤事项：启用 AttackState::OnUpdate Pre 钩子
 *
 * 事项 M5 后撤（整体）的唯一应用入口；所有后端（偏移 + Detour + SDKCall）已在 Init
 * 就绪才允许整条链启用，任一缺失整条后撤停，不允许只跑残血分支。
 * 失败时该事项 FAILED，另两项不受影响。
 * 失败语义：钩子启用失败只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = Pre 钩子已启用
 */
static bool ApplyModule5_RetreatBackend()
{
    if (!Item_IsReady(ITEM_M5_RETREAT) || g_hAttackRetreatDetour == null)
    {
        Item_SetArmed(ITEM_M5_RETREAT, false);
        return false;
    }

    bool ok = SyncDetourEnabled(g_hAttackRetreatDetour, g_bRetreatHooked,
        false, Hook_AttackState_OnUpdate_Pre, true, "Module 5");
    Item_SetArmed(ITEM_M5_RETREAT, ok);
    return ok;
}

/**
 * 还原换弹掩体 NOP 事项（ITEM_M5_RELOAD；写回原始字节并解除武装）。
 *
 * 失败语义：无（幂等；写回 + Armed(false)）。
 */
static void RestoreModule5_ReloadBackend()
{
    g_PatchReloadHide.Restore();
    Item_SetArmed(ITEM_M5_RELOAD, false);
}

/**
 * 还原闪避事项（ITEM_M5_DODGE；停用 Post 钩子并解除武装）。
 *
 * 失败语义：无（幂等；摘钩 + Armed(false)）。
 */
static void RestoreModule5_DodgeBackend()
{
    SyncDetourEnabled(g_hAttackDodgeDetour, g_bDodgeHooked, true, Hook_AttackState_OnEnter_Post, false, "Module 5");
    Item_SetArmed(ITEM_M5_DODGE, false);
}

/**
 * 还原后撤事项（ITEM_M5_RETREAT；停用 Pre 钩子并解除武装）。
 *
 * 失败语义：无（幂等；摘钩 + Armed(false)）。
 */
static void RestoreModule5_RetreatBackend()
{
    SyncDetourEnabled(g_hAttackRetreatDetour, g_bRetreatHooked, false, Hook_AttackState_OnUpdate_Pre, false, "Module 5");
    Item_SetArmed(ITEM_M5_RETREAT, false);
}
