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
Handle g_hSDKCall_ComputePath      = null;
Handle g_hSDKCall_IsOutnumbered    = null;
Handle g_hSDKCall_Hunt             = null;

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
 * 事项 M8 死锁 / M12 门禁的 Hunt 后端准备；缺失则相关事项 FAILED。
 */
static bool PrepSDKCall_Hunt()
{
    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Hunt");
    g_hSDKCall_Hunt = EndPrepSDKCall();
    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [SDKCalls] CCSBot_Hunt unavailable (M8 deadlock/M12 camp items will FAIL).");
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
    if (!PrepSDKCall_ComputePath())    allReady = false;
    if (!PrepSDKCall_IsOutnumbered())  allReady = false;
    if (!PrepSDKCall_Hunt())           allReady = false;

    if (allReady)
    {
        LogMessage("[BotRouteFix] [SDKCalls] All SDKCall backends ready (Follow, StopFollowing, ComputePath, IsOutnumbered, Hunt)");
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
