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

/**
 * 初始化底层 SDKCall 调用表
 */
bool InitSDKCalls()
{
    if (g_hGameConf == null)
    {
        return false;
    }

    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Follow");
    PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer);
    g_hSDKCall_Follow = EndPrepSDKCall();
    if (g_hSDKCall_Follow == null)
    {
        LogError("[BotRouteFix] [SDKCalls] Failed to create SDKCall for CCSBot_Follow!");
        return false;
    }

    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_StopFollowing");
    g_hSDKCall_StopFollowing = EndPrepSDKCall();
    if (g_hSDKCall_StopFollowing == null)
    {
        LogError("[BotRouteFix] [SDKCalls] Failed to create SDKCall for CCSBot_StopFollowing!");
        return false;
    }

    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_ComputePath");
    PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef);
    PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
    PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_ComputePath = EndPrepSDKCall();
    if (g_hSDKCall_ComputePath == null)
    {
        LogError("[BotRouteFix] [SDKCalls] Failed to create SDKCall for CCSBot_ComputePath!");
        return false;
    }

    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_IsOutnumbered");
    PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
    g_hSDKCall_IsOutnumbered = EndPrepSDKCall();
    if (g_hSDKCall_IsOutnumbered == null)
    {
        LogError("[BotRouteFix] [SDKCalls] Failed to create SDKCall for CCSBot_IsOutnumbered!");
        return false;
    }

    StartPrepSDKCall(SDKCall_Entity);
    PrepSDKCall_SetFromConf(g_hGameConf, SDKConf_Signature, "CCSBot_Hunt");
    g_hSDKCall_Hunt = EndPrepSDKCall();
    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [SDKCalls] Failed to create SDKCall for CCSBot_Hunt!");
        return false;
    }

    LogMessage("[BotRouteFix] [SDKCalls] Core SDKCall table ready (Follow, StopFollowing, ComputePath, IsOutnumbered, Hunt)");

    return true;
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

/**
 * 计算指定 Bot 通往目标坐标的导航路径
 *
 * @param bot       Bot 客户端索引 (必须为存活有效 Bot)
 * @param goal      目标世界坐标
 * @param routeType 寻路策略 (0 = FASTEST_ROUTE, 1 = SAFEST_ROUTE)
 * @return          是否成功生成路径
 */
stock bool BotComputePath(int bot, const float goal[3], int routeType = 0)
{
    if (g_hSDKCall_ComputePath == null || !IsBot(bot, true))
    {
        return false;
    }

    return SDKCall(g_hSDKCall_ComputePath, bot, goal, routeType);
}

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
