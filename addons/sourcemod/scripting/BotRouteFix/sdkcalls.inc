#if defined _botroutefix_sdkcalls_included
    #endinput
#endif
#define _botroutefix_sdkcalls_included

//========================================================================================
// SDKCALLS SUBSYSTEM
//
// 统一管理引擎级虚函数与底层静态函数的 SDKCall 封装句柄与接口。
// 后续模块（保镖随行、寻路测距、战术躲藏等）可直接按需调用标准接口。
//========================================================================================

// SDKCall 句柄容器
Handle g_hSDKCall_Follow           = null;
Handle g_hSDKCall_StopFollowing    = null;
Handle g_hSDKCall_MoveTo           = null;
Handle g_hSDKCall_Idle             = null;
Handle g_hSDKCall_ComputePath      = null;
Handle g_hSDKCall_IsOutnumbered    = null;
Handle g_hSDKCall_Hunt             = null;
Handle g_hSDKCall_ResetStuckMonitor = null;
Handle g_hSDKCall_IsSniper         = null;
Handle g_hSDKCall_EquipBestWeapon  = null;

//========================================================================================
// PER-CALL PREPARATION
//
// 每个 SDKCall 是独立后端：单独准备、单独失败，不再让一个缺失签名拖死整张表。
//========================================================================================

/**
 * 事项 M7 护送花名册的 Follow 后端准备；缺失则 M7 事项 FAILED。
 */
static bool PrepSDKCall_Follow()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Follow");
    PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer);
    g_hSDKCall_Follow = EndPrepSDKCall();
    if (g_hSDKCall_Follow == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_Follow unavailable (M7 will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M7 护送花名册的 StopFollowing 后端准备；缺失则 M7 事项 FAILED。
 */
static bool PrepSDKCall_StopFollowing()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_StopFollowing");
    g_hSDKCall_StopFollowing = EndPrepSDKCall();
    if (g_hSDKCall_StopFollowing == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_StopFollowing unavailable (M7 will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M7 开路先锋寻路的 MoveTo 后端准备；缺失则 M7 事项 FAILED。
 */
static bool PrepSDKCall_MoveTo()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_MoveTo");
    PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef);
    PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
    g_hSDKCall_MoveTo = EndPrepSDKCall();
    if (g_hSDKCall_MoveTo == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_MoveTo unavailable (M7 will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M7 先锋复位 / 状态监督与 M15 Ambush 踢出的 Idle 后端准备；缺失则相关事项 FAILED。
 *
 * Idle 是引擎里所有状态收束的回口：MoveTo 完成/放弃、Hide 结束、Hunt 结束都会走 Idle()。
 * M7 用它取消先锋残留的 MoveToState，并用同一签名挂 Detour 感知先锋转入 Idle；
 * M15 用它把下包前陷入 Ambush Hide 的先锋与到期持包者踢回 Idle 续接原任务。
 */
static bool PrepSDKCall_Idle()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Idle");
    g_hSDKCall_Idle = EndPrepSDKCall();
    if (g_hSDKCall_Idle == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_Idle unavailable (M7/M15 will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M9 抢包 / M10 拆包 / M11 走廊的测距后端准备（nav_measure 快照探测）；缺失则相关事项 FAILED。
 */
static bool PrepSDKCall_ComputePath()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_ComputePath");
    PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef);
    PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
    PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_ComputePath = EndPrepSDKCall();
    if (g_hSDKCall_ComputePath == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_ComputePath unavailable (M9/M10/M11 nav items will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M5 后撤的一打多判定后端准备；缺失则 M5 retreat 事项 FAILED。
 */
static bool PrepSDKCall_IsOutnumbered()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_IsOutnumbered");
    PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_IsOutnumbered = EndPrepSDKCall();
    if (g_hSDKCall_IsOutnumbered == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_IsOutnumbered unavailable (M5 retreat will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M8 死锁 / M9 选举解蹲 / M12 门禁 / M15 Ambush 踢出的 Hunt 后端准备；缺失则相关事项 FAILED。
 */
static bool PrepSDKCall_Hunt()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Hunt");
    g_hSDKCall_Hunt = EndPrepSDKCall();
    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_Hunt unavailable (M8 deadlock/M9 fetch/M12 sniper camp/M15 hide items will FAIL).");
        return false;
    }
    return true;
}

/**
 * 事项 M8 排队打碎的 ResetStuckMonitor 后端准备；缺失则 M8 queue 事项 FAILED。
 */
static bool PrepSDKCall_ResetStuckMonitor()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_ResetStuckMonitor");
    g_hSDKCall_ResetStuckMonitor = EndPrepSDKCall();
    if (g_hSDKCall_ResetStuckMonitor == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_ResetStuckMonitor unavailable (M8 queue will FAIL).");
        return false;
    }
    return true;
}

/**
 * 狙击手引擎真值后端准备（CCSBot::IsSniper，主武器槽类型判定；非事项后端）。
 * 缺失不影响任何事项：M12 名单回退为不过滤（与旧行为一致），M11 回退名册硬编码。
 */
static bool PrepSDKCall_IsSniper()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_IsSniper");
    PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_IsSniper = EndPrepSDKCall();
    if (g_hSDKCall_IsSniper == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_IsSniper unavailable (M12 roster unfiltered, M11 legacy list).");
        return false;
    }
    return true;
}

/**
 * 准备 M16 逃跑切枪的 EquipBestWeapon 后端；缺失则 M16 事项 FAILED。
 */
static bool PrepSDKCall_EquipBestWeapon()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_EquipBestWeapon");
    PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_EquipBestWeapon = EndPrepSDKCall();
    if (g_hSDKCall_EquipBestWeapon == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_EquipBestWeapon unavailable (M16 escape item will FAIL).");
        return false;
    }
    return true;
}

/**
 * 初始化底层 SDKCall 调用表（逐项准备，汇总返回，不 fail-fast）
 */
bool InitSDKCalls()
{
    if (g_hGameConf == null)
    {
        return false;
    }

    bool allReady = true;
    if (!PrepSDKCall_Follow())         allReady = false;
    if (!PrepSDKCall_StopFollowing())  allReady = false;
    if (!PrepSDKCall_MoveTo())         allReady = false;
    if (!PrepSDKCall_Idle())           allReady = false;
    if (!PrepSDKCall_ComputePath())    allReady = false;
    if (!PrepSDKCall_IsOutnumbered())  allReady = false;
    if (!PrepSDKCall_Hunt())           allReady = false;
    if (!PrepSDKCall_ResetStuckMonitor()) allReady = false;
    if (!PrepSDKCall_IsSniper())       allReady = false;
    if (!PrepSDKCall_EquipBestWeapon()) allReady = false;

    if (allReady)
    {
        LogMessage("[BotRouteFix] [SDKCalls] All SDKCall backends ready (Follow, StopFollowing, MoveTo, Idle, ComputePath, IsOutnumbered, Hunt, ResetStuckMonitor, IsSniper, EquipBestWeapon)");
    }

    return allReady;
}

/**
 * 释放所有 SDKCall 句柄
 */
void CleanupSDKCalls()
{
    if (g_hSDKCall_Follow != null)
    {
        delete g_hSDKCall_Follow;
        g_hSDKCall_Follow = null;
    }

    if (g_hSDKCall_StopFollowing != null)
    {
        delete g_hSDKCall_StopFollowing;
        g_hSDKCall_StopFollowing = null;
    }

    if (g_hSDKCall_MoveTo != null)
    {
        delete g_hSDKCall_MoveTo;
        g_hSDKCall_MoveTo = null;
    }

    if (g_hSDKCall_Idle != null)
    {
        delete g_hSDKCall_Idle;
        g_hSDKCall_Idle = null;
    }

    if (g_hSDKCall_ComputePath != null)
    {
        delete g_hSDKCall_ComputePath;
        g_hSDKCall_ComputePath = null;
    }

    if (g_hSDKCall_IsOutnumbered != null)
    {
        delete g_hSDKCall_IsOutnumbered;
        g_hSDKCall_IsOutnumbered = null;
    }

    if (g_hSDKCall_Hunt != null)
    {
        delete g_hSDKCall_Hunt;
        g_hSDKCall_Hunt = null;
    }

    if (g_hSDKCall_ResetStuckMonitor != null)
    {
        delete g_hSDKCall_ResetStuckMonitor;
        g_hSDKCall_ResetStuckMonitor = null;
    }

    if (g_hSDKCall_IsSniper != null)
    {
        delete g_hSDKCall_IsSniper;
        g_hSDKCall_IsSniper = null;
    }

    if (g_hSDKCall_EquipBestWeapon != null)
    {
        delete g_hSDKCall_EquipBestWeapon;
        g_hSDKCall_EquipBestWeapon = null;
    }
}

//========================================================================================
// SAFE CALLABLE SDKCALL WRAPPERS
//========================================================================================

/**
 * 命令指定 Bot 跟随目标领袖实体
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @param leader    目标领袖客户端索引 (必须为存活有效玩家/Bot)
 * @return          是否成功发起 SDKCall
 */
stock bool BotFollow(int bot, int leader)
{
    if (g_hSDKCall_Follow == null || !IsBot(bot, true) || !IsValidClient(leader, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_Follow, bot, leader);
    return true;
}

/**
 * 停止指定 Bot 的跟随状态
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @return          是否成功发起 SDKCall
 */
stock bool BotStopFollowing(int bot)
{
    if (g_hSDKCall_StopFollowing == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_StopFollowing, bot);
    return true;
}

/**
 * 命令指定 Bot 向目标三维坐标行进（进入 MoveToState）
 *
 * @param bot           Bot 客户端索引 (必须为存活有效 Bot)
 * @param pos           目标三维坐标
 * @param routeType     路线类型 (0 = DEFAULT, 1 = FASTEST, 2 = SAFEST, 3 = RETREAT)
 * @return              是否成功发起 SDKCall
 */
stock bool BotMoveTo(int bot, const float pos[3], int routeType = 2)
{
    if (g_hSDKCall_MoveTo == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_MoveTo, bot, pos, routeType);
    return true;
}

/**
 * 把指定 Bot 拉回原生 Idle（SetTask(SEEK_AND_DESTROY) + SetState(IdleState)）。
 *
 * 与 StopFollowing 的区别：StopFollowing 只清 m_isFollowing/m_leader，不会结束
 * MoveToState；先锋被划走/释放时只有 Idle 才能让残留 MoveTo 真正停下。
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @return          是否成功发起 SDKCall
 */
stock bool BotIdle(int bot)
{
    if (g_hSDKCall_Idle == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_Idle, bot);
    return true;
}

//--------------------------------------------------------------------------------------------------------------
// 已废弃：stock bool BotComputePath(int bot, const float goal[3], int routeType = 0)
//
// 裸调 CCSBot::ComputePath 会先 DestroyPath() 再重写 m_path（cs_bot_pathfind.cpp:1877-1997），
// 且绕过 repath 节流与状态快照，调用即持久篡改 Bot 当前行进路径（M11 旧版踩过此坑）。
// 需要走廊/距离一律走 nav_measure.inc 的 BotNavWalkDistance / BotNavProbePathAreas
// （内部快照-计算-还原），不要恢复此封装。
//--------------------------------------------------------------------------------------------------------------

/**
 * 直调原生一打多判定（CCSBot::IsOutnumbered，友 < 敌-1，读视觉记忆表计数值）
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @return          是否被包围（句柄无效一律按未包围处理，fail-open）
 */
stock bool BotIsOutnumbered(int bot)
{
    if (g_hSDKCall_IsOutnumbered == null || !IsBot(bot, true))
    {
        return false;
    }

    return SDKCall(g_hSDKCall_IsOutnumbered, bot);
}

/**
 * 命令指定 Bot 转入搜敌（CCSBot::Hunt，仅 SetState，无参数无返回）
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @return          是否成功发起 SDKCall
 */
stock bool BotHunt(int bot)
{
    if (g_hSDKCall_Hunt == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_Hunt, bot);
    return true;
}

/**
 * 重置指定 Bot 的卡住监视器（CCSBot::ResetStuckMonitor）
 *
 * 清除 m_isStuck、卡住计时、速度采样与 m_areaEnteredTimestamp；
 * M8 排队打碎后调用，与原生 polite timeout 的 ResetStuckMonitor() 对齐。
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @return          是否成功发起 SDKCall
 */
stock bool BotResetStuckMonitor(int bot)
{
    if (g_hSDKCall_ResetStuckMonitor == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_ResetStuckMonitor, bot);
    return true;
}

/**
 * 让指定 Bot 立即换出最佳武器（CCSBot::EquipBestWeapon）。
 *
 * mustEquip=true 绕过节流（m_equipTimer / minEquipInterval）；兜底无更好武器时会尾调
 * EquipKnife。调用方若在 EquipKnife 钩内调用，必须自带重入闸（M16 已实现）。
 *
 * @param bot           Bot 客户端索引（必须是有效 Bot）
 * @param mustEquip     true = 忽略节流立即换
 * @return              是否成功发出 SDKCall
 */
stock bool BotEquipBestWeapon(int bot, bool mustEquip)
{
    if (g_hSDKCall_EquipBestWeapon == null || !IsBot(bot, true))
    {
        return false;
    }

    SDKCall(g_hSDKCall_EquipBestWeapon, bot, mustEquip);
    return true;
}

/**
 * 狙击手判定总入口，人机通用（M11 敌方识别 + M12 名单过滤共用）。
 *
 * 引擎真值优先：`CCSBot::IsSniper`（主武器槽 + WEAPONTYPE_SNIPER_RIFLE 类型判定，
 * 自定义狙击枪只要类型正确即被识别，与 camp 分支的上游门禁同源）。
 * 签名缺失时回退名册硬编码（AWP/Scout/G3SG1/SG550，手持 + Slot 0 双重校验，旧行为）。
 * 纯只读；无效/阵亡返回 false。
 */
stock bool IsSniper(int client)
{
    if (!IsValidClient(client, true))
    {
        return false;
    }

    if (g_hSDKCall_IsSniper != null)
    {
        return SDKCall(g_hSDKCall_IsSniper, client);
    }

    // 回退：手持 + Slot 0 双重校验（覆盖切副手/切刀时的狙击手身份）
    char classname[32];
    int activeWpn = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (activeWpn > 0 && IsValidEntity(activeWpn))
    {
        GetEntityClassname(activeWpn, classname, sizeof(classname));
        if (StrEqual(classname, "weapon_awp")
            || StrEqual(classname, "weapon_scout")
            || StrEqual(classname, "weapon_g3sg1")
            || StrEqual(classname, "weapon_sg550"))
        {
            return true;
        }
    }

    int primaryWpn = GetPlayerWeaponSlot(client, 0);
    if (primaryWpn > 0 && IsValidEntity(primaryWpn))
    {
        GetEntityClassname(primaryWpn, classname, sizeof(classname));
        if (StrEqual(classname, "weapon_awp")
            || StrEqual(classname, "weapon_scout")
            || StrEqual(classname, "weapon_g3sg1")
            || StrEqual(classname, "weapon_sg550"))
        {
            return true;
        }
    }

    return false;
}
