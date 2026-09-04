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
 * 初始化底层 SDKCall 调用表
 */
void InitSDKCalls()
{
    if (g_hGameConf == null)
    {
        return;
    }

    // 后续各功能模块所需的底层 SDKCall 会在此按需预检初始化
    // 例如：CCSBot::Follow, CCSBot::StopFollowing, CCSBot::ComputePath 等
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
