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
Handle g_hSDKCall_Hide             = null;
Handle g_hSDKCall_GetRandomArea    = null;

/**
 * 初始化底层 SDKCall 调用表（保镖随行先行；Hide/ComputePath 随拆包/捡包模块落地）
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

    LogMessage("[BotRouteFix] [SDKCalls] Follow & StopFollowing ready (carrier escort transport online)");
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

    if (g_hSDKCall_Hide != null)
    {
        delete g_hSDKCall_Hide;
        g_hSDKCall_Hide = null;
    }

    if (g_hSDKCall_GetRandomArea != null)
    {
        delete g_hSDKCall_GetRandomArea;
        g_hSDKCall_GetRandomArea = null;
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
