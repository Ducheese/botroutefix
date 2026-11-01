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
//    除 M5 紧急短蹲请求（1 秒覆写）外不做额外钳制；其余 Hide 一律按士气缩放。
//
//    ★ 模块 12 宿主与短路协同 (M12 Co-Detour & Short-Circuit):
//    本 Detour 同时兼任 [模块 12] 狙击 Camp 门禁的借钩宿主。进入时优先经由 Module12_CampGate(client)
//    进行 task 19 重骰；若判定前压转入 Hunt，则直接 short-circuit（提前返回 MRES_Ignored，
//    跳过时长缩放且不累加计数器），实现双模块高效共用单个 Detour 拦截点。
//
//    ★ 模块 5 紧急短蹲入口 (M5 Soft-Exemption Short Hide, 2026-10-31):
//    M5 在焊门时调用 HideDuration_RequestShort(client)，本模块在 Post 中优先消费该请求，
//    将 m_duration 直接写为 1 秒并跳过士气缩放（一次性 + 2 秒 TTL）。
//    语义：后撤动作完整保留（跑向掩体、IgnoreEnemies 都照做），到达藏身点后蹲 1 秒即起，
//    用于"有使命在身、不能久蹲"的身份（包匪/捡包名册/保镖/CT 掩护组等）。
//    注：请求只按 client + TTL 绑定，TTL 内任何 Hide 都会消费它（含换弹 0 秒 Hide、守包长蹲）——
//    这是已知的弱身份约束；无待消费请求时，换弹 Hide 才保持原生 0 秒。
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

// 紧急短蹲请求（M5 焊门时下发）：一次性消费；TTL 内任意 Hide 均可消费，超时未消费则作废
#define HIDE_SHORT_REQUEST_TTL      2.0     // 请求有效期 (秒)
#define HIDE_SHORT_DURATION         1.0     // 短蹲时长：到点即起，但保留完整后撤动作

DynamicDetour g_hHideDurationDetour = null;
int g_iOffset_HideDuration = -1;
int g_iTotalHideRescaled = 0;

// per-client 短蹲请求截止时间戳 (0.0 = 无请求)
static float g_flShortHideUntil[MAXPLAYERS + 1];
static int g_iTotalShortHides = 0;

// 业务开启态与 per-hook 真实启用态（DHook 状态机唯一事实来源）
static bool g_bModule15_Active = false;
static bool g_bHideDurationDetourEnabled = false;

/**
 * 初始化并挂载模块 15（DHook HideState::OnEnter post 宿主）
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

    if (!MountHideDurationDetour())
    {
        LogError("[BotRouteFix] [Module 15] Failed to inject HideState::OnEnter post hook!");
        return false;
    }

    // 套用业务态（借钩方模块 12 可能已先行武装：底层真实启用态统一由 Enable 内部的 Sync 裁决）
    EnableModule15_HideDuration(true);

    LogMessage("[BotRouteFix] [Module 15] Hide duration morale scaling activated [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 同步 HideState::OnEnter 宿主 Detour 的真实启用态（M12/M15 共用单钩的协同中枢）
 *
 * 模块 12（狙击 camp 门禁）不单独注册 Detour，而是借本模块的 Post Detour 作为执行宿主。
 * 底层钩子只要任一借钩方仍在工作就必须保持启用；只有双方全部关闭才真正 Disable。
 * 既杜绝重复 Enable/Disable 触发 "Function not detoured" 致命状态机异常，
 * 也避免"关掉模块 15 顺带把模块 12 门禁打死"的隐性耦合一刀切。
 */
void SyncHideDurationDetourState()
{
    bool want = (IsModule12_Active() || g_bModule15_Active);
    SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationDetourEnabled,
        true, Hook_HideState_OnEnter_Post, want, "Module 15");
}

/**
 * 动态启用或停用模块 15（幂等；底层钩子随借钩双方的整体需求同步收敛）
 */
void EnableModule15_HideDuration(bool enable)
{
    if (enable == g_bModule15_Active)
    {
        return;
    }

    g_bModule15_Active = enable;
    SyncHideDurationDetourState();
}

/**
 * 还原模块 15（插件卸载：强制摘钩并释放句柄）
 */
void RestoreModule15_HideDuration()
{
    EnableModule15_HideDuration(false);

    if (g_hHideDurationDetour != null)
    {
        // 卸载路径不看借钩方意愿，直接强制 Disable 后再释放句柄
        SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationDetourEnabled,
            true, Hook_HideState_OnEnter_Post, false, "Module 15");
        delete g_hHideDurationDetour;
        g_hHideDurationDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 15] Hook safely removed.");
}

/**
 * 查询模块 15 业务开启态
 */
bool IsModule15_Active()
{
    return g_bModule15_Active;
}

/**
 * 查询 HideState::OnEnter 宿主钩子的底层挂载态（含借钩方需求）
 */
bool IsHideDurationHooked()
{
    return (g_hHideDurationDetour != null && g_bHideDurationDetourEnabled);
}

/**
 * 累积缩放次数（自加载起单调递增，换图不清零）
 */
int GetTotalHideRescaled()
{
    return g_iTotalHideRescaled;
}

/**
 * 累积紧急短蹲次数（自加载起单调递增，换图不清零）
 */
int GetTotalShortHides()
{
    return g_iTotalShortHides;
}

/**
 * 紧急短蹲请求：M5 焊门时下发，HideState::OnEnter Post 消费一次
 *
 * 与士气缩放互斥：命中请求直接写 1 秒并跳过缩放，不进缩放计数。
 * TTL 约束：请求只绑定 client + 2 秒窗口，窗口内任意 Hide 都会消费它；超时未消费则作废。
 * 已知弱身份：TryToRetreat 找不到掩体后，窗口内的换弹/守包 Hide 可能被误压成 1 秒。
 *
 * @param client        客户端索引
 */
void HideDuration_RequestShort(int client)
{
    if (client <= 0 || client > MaxClients)
    {
        return;
    }

    g_flShortHideUntil[client] = GetGameTime() + HIDE_SHORT_REQUEST_TTL;
}

/**
 * 回合开始：清空上回合残留的短蹲请求（槽位复用/时间戳跨回合失效）
 */
void Module15_OnRoundStart()
{
    for (int i = 0; i <= MaxClients; i++)
    {
        g_flShortHideUntil[i] = 0.0;
    }
}

/**
 * 地图加载 (OnMapStart)：换图后 GetGameTime 归零，旧图绝对时间戳必须清，
 * 否则旧图末尾的 large timestamp 会在新图前几秒内被误判为有效请求
 */
void Module15_OnMapStart()
{
    Module15_OnRoundStart();
}

/**
 * 客户端断开：清空该槽位短蹲请求
 */
void Module15_OnClientDisconnect(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_flShortHideUntil[client] = 0.0;
    }
}

//========================================================================================
// DETOUR: HIDESTATE::ONENTER (POST HOOK FOR MORALE DURATION SCALING)
//========================================================================================

static bool MountHideDurationDetour()
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

    return true;
}

public MRESReturn Hook_HideState_OnEnter_Post(Address pThis, DHookParam hParams)
{
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
    if (IsModule12_Active() && Module12_CampGate(client))
    {
        return MRES_Ignored;
    }

    if (g_iOffset_HideDuration <= 0)
    {
        return MRES_Ignored;
    }

    // 紧急短蹲优先：M5 在焊门时下发的请求，命中即写 1 秒并跳过士气缩放
    // ★ 它在 M15 业务门之前消费：短蹲属于 M5 的后撤语义，M15 只是执行宿主；
    //   即使 M15 被模块规则关闭、只要 M12 借钩仍保持 Detour 挂载，短蹲也应生效。
    // 注：换弹掩体的 Hide(*spot, 0.0f) 天生 0 秒，与本入口无关；士气缩放对 0.0 也是 no-op
    float shortUntil = g_flShortHideUntil[client];
    if (shortUntil > 0.0)
    {
        g_flShortHideUntil[client] = 0.0; // 一次性消费

        if (GetGameTime() <= shortUntil)
        {
            StoreToAddress(pThis + view_as<Address>(g_iOffset_HideDuration),
                view_as<int>(HIDE_SHORT_DURATION), NumberType_Int32);
            g_iTotalShortHides++;
            return MRES_Ignored;
        }
    }

    // 借钩方可能独占了本宿主（M15 关闭、M12 开启）：业务门各自独立把关
    if (!g_bModule15_Active)
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
