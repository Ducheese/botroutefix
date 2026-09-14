#if defined _botroutefix_module15_hideduration_included
    #endinput
#endif
#define _botroutefix_module15_hideduration_included

//========================================================================================
// [模块 15] Hide 蹲守时长士气缩放 m_duration *= (1 - 0.15 * Morale)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生机制 (cs_bot_hide.cpp:22-31, HideState::OnEnter)：
//      if (m_duration < 0.0f) m_duration = RandomFloat(30.0f, 60.0f);  // -1（无限蹲）归一化
//    到点后 m_hideTimer.Start(m_duration)（hide.cpp:458），超时回 Idle 重决策。
//    所有 Hide()/TryToHide() 调用（守包 -1、狙击 camp 10~30s、前沿架枪 timeToWait）
//    最终都进这一个 OnEnter，是天然的单 choke 点（双阵营全覆盖，per-bot 生效）。
//
// 2. 补丁方案：DHook HideState::OnEnter 的 post，读归一化后的 m_duration，按当事 bot
//    士气现算现缩：duration *= (1.0 - 0.15 * Morale)。
//      士气 +3: 0.55x（蹲 60s → 33s，多动）
//      士气  0: 1.00x（原样）
//      士气 -3: 1.45x（蹲 60s → 87s，少动）
//    不钳制、不豁免（带包者引擎 Idle 分流本来进不来长 Hide），先看实测效果。
//
//    ★ 模块 12 宿主与短路协同 (M12 Co-Detour & Short-Circuit):
//    本 Detour 同时兼任 [模块 12] 狙击 Camp 门禁的借钩宿主。进入时优先经由 Module12_CampGate(client)
//    进行 task 19 重骰；若判定前压转入 Hunt，则直接 short-circuit（提前返回 MRES_Ignored，
//    跳过时长缩放且不累加计数器），实现双模块高效共用单个 Detour 拦截点。
//
// 3. 二进制实证（find_bytes 各 n=1）：
//    - 32 位 OnEnter @ v91 0x102B9450 / v92 0x102BB840：
//      comiss xmm0, [esi+1Ch] + RandomFloat(30,60) 回写 [esi+1Ch] → m_duration @ +0x1C。
//    - 64 位 OnEnter @ 0x180361170：
//      comiss xmm6, [rcx+24h] + RandomFloat(30.0, 60.0) 回写 [rbp+24h] → m_duration @ +0x24。
//      （与 cs_bot.h 头文件布局推导一致：vtable + ptr + float + Vector + 2bool + pad。）
//========================================================================================
// Hide 蹲守时长士气缩放宏定义 (Scale = BASE - STEP * Morale)
#define HIDE_DURATION_SCALE_BASE    1.0     // 基础缩放系数 (士气 0 时 1.0x 原始时长)
#define HIDE_DURATION_SCALE_STEP    0.15    // 每级士气缩放步长 (15%，缩放范围 0.55x ~ 1.45x)

DynamicDetour g_hHideDurationDetour = null;
int g_iOffset_HideDuration = -1;
int g_iTotalHideRescaled = 0;

/**
 * 初始化并激活模块 15（DHook HideState::OnEnter post）
 *
 * @return              是否成功激活
 */
bool InitModule15_HideDuration()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 15] Error: Gamedata is not loaded, cannot initialize Hide duration hook!");
        return false;
    }

    g_iOffset_HideDuration = GameConfGetOffset(g_hGameConf, "HideState_Duration_Offset");
    if (g_iOffset_HideDuration <= 0)
    {
        LogError("[BotRouteFix] [Module 15] Failed to read HideState_Duration_Offset!");
        return false;
    }

    if (!PrepHideDurationDetour())
    {
        LogError("[BotRouteFix] [Module 15] Failed to inject HideState::OnEnter post hook!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 15] Hide duration morale scaling activated [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 还原模块 15（关闭 detour）
 */
void RestoreModule15_HideDuration()
{
    if (g_hHideDurationDetour != null)
    {
        DHookDisableDetour(g_hHideDurationDetour, true, Hook_HideState_OnEnter_Post);
        delete g_hHideDurationDetour;
        g_hHideDurationDetour = null;
    }
    LogMessage("[BotRouteFix] [Module 15] Hook safely removed.");
}

/**
 * 动态启用或停用模块 15
 */
void EnableModule15_HideDuration(bool enable)
{
    if (g_hHideDurationDetour == null)
    {
        return;
    }
    if (enable)
    {
        DHookEnableDetour(g_hHideDurationDetour, true, Hook_HideState_OnEnter_Post);
    }
    else
    {
        DHookDisableDetour(g_hHideDurationDetour, true, Hook_HideState_OnEnter_Post);
    }
}

/**
 * 查询模块 15 是否挂载
 */
bool IsHideDurationHooked()
{
    return g_hHideDurationDetour != null;
}

/**
 * 累积缩放次数（自加载起单调递增，换图不清零）
 */
int GetTotalHideRescaled()
{
    return g_iTotalHideRescaled;
}

//========================================================================================
// DETOUR: HIDESTATE::ONENTER (POST HOOK FOR MORALE DURATION SCALING)
//========================================================================================

static bool PrepHideDurationDetour()
{
    g_hHideDurationDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hHideDurationDetour == null)
    {
        LogError("[BotRouteFix] [Module 15] Failed to create DHook detour instance for HideState_OnEnter!");
        return false;
    }

    if (!DHookSetFromConf(g_hHideDurationDetour, g_hGameConf, SDKConf_Signature, "HideState_OnEnter"))
    {
        LogError("[BotRouteFix] [Module 15] Failed to set HideState_OnEnter signature from gamedata!");
        delete g_hHideDurationDetour;
        g_hHideDurationDetour = null;
        return false;
    }

    DHookAddParam(g_hHideDurationDetour, HookParamType_CBaseEntity);

    if (!DHookEnableDetour(g_hHideDurationDetour, true, Hook_HideState_OnEnter_Post))
    {
        LogError("[BotRouteFix] [Module 15] Failed to enable HideState_OnEnter Post detour!");
        delete g_hHideDurationDetour;
        g_hHideDurationDetour = null;
        return false;
    }

    return true;
}

public MRESReturn Hook_HideState_OnEnter_Post(Address pThis, DHookParam hParams)
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

    // M12 camp 门禁：task 19 的 Hide 在此重骰；被重定向去 Hunt 的直接返回，不再缩放
    if (Module12_CampGate(client))
    {
        return MRES_Ignored;
    }

    if (g_iOffset_HideDuration <= 0)
    {
        return MRES_Ignored;
    }

    // 读归一化后的 m_duration（-1 已在 OnEnter 本体内变成 30~60s），按士气伸缩后写回
    // 提示：引擎在随后的 HideState::OnUpdate 判定到达目标点（range < 20.0f）时才会正式调用 m_hideTimer.Start(m_duration)
    // 因此在 OnEnter Post 中修改 duration 绝对及时且全局生效
    float duration = view_as<float>(LoadFromAddress(pThis + view_as<Address>(g_iOffset_HideDuration), NumberType_Int32));
    int morale = GetBotMorale(client);
    float scaled = duration * (HIDE_DURATION_SCALE_BASE - HIDE_DURATION_SCALE_STEP * float(morale));
    StoreToAddress(pThis + view_as<Address>(g_iOffset_HideDuration), view_as<int>(scaled), NumberType_Int32);

    g_iTotalHideRescaled++;
    // DebugLog("[BotRouteFix] [Module 15] Bot %N hide duration %.1fs -> %.1fs (morale %d)", client, duration, scaled, morale);
    return MRES_Ignored;
}
