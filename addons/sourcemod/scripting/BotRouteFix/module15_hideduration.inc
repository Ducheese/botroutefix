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
//    除 M5 紧急短蹲请求（1 秒覆写）与下包前先锋/到期包匪的无限期掩体（Hide(-1)）短蹲外，其余 Hide 一律按士气缩放。
//
//    ★ 模块 12 宿主与短路协同 (M12 Co-Detour & Short-Circuit):
//    本 Detour 同时兼任 [模块 12] 狙击 Camp 门禁的借钩宿主。Post 进入时优先经由 Module12_HandleSniperCamp(client)
//    进行 task 19 重骰；若判定前压转入 Hunt，则直接 short-circuit（提前返回 MRES_Ignored，
//    跳过时长缩放且不累加计数器），实现双模块高效共用单个 Detour 拦截点。
//    Pre 钩子由 M15 独占启停；-15 关闭 M15 时 Pre 完全卸载，Post 仍可按 M9/M12 需求继续挂载。
//
//    ★ 下包前先锋与到期包匪 Hide(-1) 短蹲（Pre+Post 单线程标记）:
//    HideState::OnEnter 在进入时会立刻把 m_duration < 0.0 (-1.0f) 归一化为 RandomFloat(30.0, 60.0)。
//    利用 SourceMod/CS:S 主循环单线程且 OnEnter 内不递归状态切换的特性，M15 在 Pre 读取原始
//    m_duration < 0.0 并记录 (pThis, client)；Post 入口第一时间读取并清空该标记，确认未被 M12/M5 优先消费后，
//    在 !g_bBombPlanted 期间沿用与 M6 下包 OPPO 守卫同口径判定（M7 开路先锋 Module7_GetPointman()、
//    plantDelay 到期后 GetGameTime() >= earliest 的 C4 携带者，或提前踩进包点拿到 BOT_TASK_PLANT_BOMB 者）：
//      - 仅当本次确实为 Hide(-1) 时把 m_duration 写成 1.0 秒并跳过士气缩放，不改状态机；
//      - 有限时长蹲（0.0s 换弹掩体、1.0s 躲雷、3~15s 撤退/埋伏、M5 短蹲）以及下包后（g_bBombPlanted == true）
//        Guard 守包 Hide(-1) 均不受此拦截，正常走士气缩放或短蹲逻辑。
//
//    ★ 模块 5 紧急短蹲入口 (M5 Soft-Exemption Short Hide, 2026-10-31):
//    M5 在焊门时调用 Module15_RequestShortHide(client)，本模块在 Post 中优先消费该请求，
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
//
// 事项契约:
//   - 唯一事项 = Hide 时长修改（ITEM_M15_HIDE），缩放与短蹲同属这一件事（items.inc:22）。
//   - 宿主 Detour 归本模块所有；M12 借用它做 camp 重骰，钩子开关由 Sync 中枢统一裁决。
//   - 短蹲请求是 M5 的后撤语义，宿主在就消费；M15 自身事项未武装时只跳过缩放。
//========================================================================================

//========================================================================================
// MODULE 15 DEFINES & GLOBALS
//========================================================================================

// Hide 蹲守时长士气缩放宏定义 (Scale = BASE - STEP * Morale)
#define HIDE_DURATION_SCALE_BASE    1.0     // 基础缩放系数 (士气 0 时 1.0x 原始时长)
#define HIDE_DURATION_SCALE_STEP    0.15    // 每级士气缩放步长 (15%，缩放范围 0.55x ~ 1.45x)

// 紧急短蹲请求（M5 焊门时下发）：一次性消费；TTL 内任意 Hide 均可消费，超时未消费则作废
#define HIDE_SHORT_REQUEST_TTL      2.0     // 请求有效期 (秒)
#define HIDE_SHORT_DURATION         1.0     // 短蹲时长：到点即起，但保留完整后撤动作

static DynamicDetour g_hHideDurationDetour = null;
static int g_iOffset_HideDuration = -1;
static int g_iTotalHideRescaled = 0;

// per-client 短蹲请求截止时间戳 (0.0 = 无请求)
static float g_flShortHideUntil[MAXPLAYERS + 1];
static int g_iTotalShortHides = 0;

// 单线程 HideState::OnEnter Pre -> Post 瞬时传递：识别原始 m_duration < 0.0 的 Hide(-1)
// 默认零即 Address_Null（全仓 Address 全局量惯例：global.inc 裸声明，不写显式初始化器）
static Address g_pPreInfiniteHideState;
static int g_iPreInfiniteHideClient = 0;

// 模块启用缓存（= Item_IsArmed(ITEM_M15_HIDE)，Enable 幂等短路）
static bool g_bModule15_Active = false;
static bool g_bHideDurationPreHooked = false;
static bool g_bHideDurationHooked = false;

//========================================================================================
// MODULE 15 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：Hide 时长修改（ITEM_M15_HIDE）
 *
 * 先挂载共享宿主 HideState::OnEnter Detour（避免 M15 独占依赖缺失时连带饿死 M9/M12 借钩宿主），
 * 再校验 M15 所需偏移，均不提前启用钩子（启用由 Apply 执行）。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = Hide 时长事项后端就绪
 */
static bool PrepModule15_HideBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 15] Gamedata missing; hide duration item FAILED.");
        return false;
    }

    // 先挂载共享宿主 Detour，避免 M15 独占依赖缺失时饿死 M9/M12 借钩宿主
    if (!MountHideDurationDetour())
    {
        return false;
    }

    if (g_iOffset_Morale <= 0)
    {
        LogError("[BotRouteFix] [Module 15] Morale offset missing; hide duration item FAILED.");
        return false;
    }

    g_iOffset_HideDuration = GameConfGetOffset(g_hGameConf, "HideState_Duration_Offset");
    if (g_iOffset_HideDuration <= 0)
    {
        LogError("[BotRouteFix] [Module 15] HideState_Duration offset missing; hide duration item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 15] Hide duration offset verified (%d).", g_iOffset_HideDuration);
    return true;
}

/**
 * 挂载 HideState::OnEnter Post Detour（事项 M15 与 M12 借钩的唯一宿主）
 *
 * 失败语义：签名缺失或句柄创建失败时 M15 事项 FAILED，M12 也随之无法武装。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
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

//========================================================================================
// MODULE 15 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 15 的唯一事项：Hide 蹲守时长缩放（ITEM_M15_HIDE）
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 *
 * @return              true = 事项已武装
 */
bool InitModule15_HideDuration()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M15_HIDE, false);
        LogError("[BotRouteFix] [Module 15] Gamedata missing; hide duration item FAILED.");
        return false;
    }

    Item_SetReady(ITEM_M15_HIDE, PrepModule15_HideBackend());
    EnableModule15_HideDuration(true);
    if (!g_bModule15_Active)
    {
        LogModuleAllFailed(15);
        return false;
    }

    LogModuleArmed(15, "Hide duration", "1");
    return true;
}

/**
 * 动态启用或停用模块 15（事项 ITEM_M15_HIDE 的调度入口；幂等；底层钩子随借钩双方的整体需求同步收敛）
 *
 * 只做调度：只武装 Ready 的事项；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 *
 * @param enable        true = 武装并同步宿主钩子；false = 解除武装并同步
 */
void EnableModule15_HideDuration(bool enable)
{
    if (enable == g_bModule15_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule15_HideBackend();
        g_bModule15_Active = false;
        return;
    }

    g_bModule15_Active = ApplyModule15_HideBackend();
}

/**
 * 还原模块 15（事项 ITEM_M15_HIDE；插件卸载：强制摘钩并释放句柄）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule15_HideDuration()
{
    EnableModule15_HideDuration(false);

    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

    if (g_hHideDurationDetour != null)
    {
        // 卸载路径不看借钩方意愿，直接强制 Disable Pre/Post 后再释放句柄
        SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationPreHooked,
            false, Hook_HideState_OnEnter_Pre, false, "Module 15 Pre");
        SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationHooked,
            true, Hook_HideState_OnEnter_Post, false, "Module 15");
        delete g_hHideDurationDetour;
        g_hHideDurationDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 15] Hide duration hooks safely restored.");
}

/**
 * 同步 HideState::OnEnter 宿主 Detour 的真实启用态（事项 ITEM_M15_HIDE 的共享宿主；M9/M12/M15 共用单钩的协同中枢）
 *
 * 模块 12（狙击 camp 门禁）不单独注册 Detour，而是借本模块的 Post Detour 作为执行宿主；
 * 模块 9（抢包名册 Hide 真值门禁）同样借宿主捕获 HideState 虚表指纹。
 * 底层钩子只要任一借钩方仍在工作就必须保持启用；只有三方全部关闭才真正 Disable。
 * 既杜绝重复 Enable/Disable 触发 "Function not detoured" 致命状态机异常，
 * 也避免"关掉模块 15 顺带把模块 12 门禁打死"的隐性耦合一刀切。
 * 失败语义：无（返回值即开关态本身；只做同步，不判定事项成败）。
 *
 * @return              true = 同步后宿主钩子处于启用态
 */
bool SyncHideDurationDetourState()
{
    bool want = (Item_IsArmed(ITEM_M15_HIDE) || Item_IsArmed(ITEM_M12_SNIPERCAMP) || Item_IsArmed(ITEM_M9_FETCH));
    return SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationHooked,
        true, Hook_HideState_OnEnter_Post, want, "Module 15");
}

/**
 * 宿主柄就绪探针（供借钩方 M12 在 Init 期断言宿主 Prep 已过）。
 *
 * @return              true = 宿主 Detour 句柄已创建
 */
bool Module15_IsHideHostReady()
{
    return g_hHideDurationDetour != null;
}

//========================================================================================
// MODULE 15 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 累积缩放次数（自加载起单调递增，换图不清零）
 */
int Module15_GetTotalHideRescaled()
{
    return g_iTotalHideRescaled;
}

/**
 * 累积紧急短蹲次数（自加载起单调递增，换图不清零）
 */
int Module15_GetTotalShortHides()
{
    return g_iTotalShortHides;
}

/**
 * 事项 M15 隐藏时长的跨模块请求入口：M5 焊门时下发紧急短蹲
 *
 * 只登记请求，不改变本模块事项状态；HideState::OnEnter Post 在 TTL 窗口内消费一次。
 * 与士气缩放互斥：命中请求直接写 1 秒并跳过缩放，不进缩放计数。
 * 已知弱身份：TryToRetreat 找不到掩体后，窗口内的换弹/守包 Hide 可能被误压成 1 秒。
 *
 * @param client        客户端索引
 */
void Module15_RequestShortHide(int client)
{
    if (client <= 0 || client > MaxClients)
    {
        return;
    }

    g_flShortHideUntil[client] = GetGameTime() + HIDE_SHORT_REQUEST_TTL;
}

//========================================================================================
// MODULE 15 EVENT HANDLERS
//========================================================================================

/**
 * 回合开始：清空上回合残留的短蹲请求（槽位复用/时间戳跨回合失效）
 */
void Module15_OnRoundStart()
{
    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

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
        if (g_iPreInfiniteHideClient == client)
        {
            g_pPreInfiniteHideState = Address_Null;
            g_iPreInfiniteHideClient = 0;
        }
    }
}

//========================================================================================
// MODULE 15 DETOUR & HOOKS (HIDESTATE::ONENTER, POST)
//========================================================================================

/**
 * HideState::OnEnter Pre：捕获原始 m_duration < 0.0（Hide(-1)）并做单线程瞬时标记。
 *
 * HideState::OnEnter 内部会把 m_duration < 0.0 归一化为 RandomFloat(30.0, 60.0)，
 * 且不触发任何嵌套状态切换；因此可在 Pre 记下 (pThis, client) 并在紧随其后的 Post 第一时间消费并清零。
 *
 * @param pThis         HideState 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_HideState_OnEnter_Pre(Address pThis, DHookParam hParams)
{
    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

    if (!Item_IsArmed(ITEM_M15_HIDE) || pThis == Address_Null || g_iOffset_HideDuration <= 0)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    float rawDuration = view_as<float>(LoadFromAddress(pThis + view_as<Address>(g_iOffset_HideDuration), NumberType_Int32));
    if (rawDuration < 0.0)
    {
        g_pPreInfiniteHideState = pThis;
        g_iPreInfiniteHideClient = client;
    }

    return MRES_Ignored;
}

/**
 * HideState::OnEnter Post：M12 camp 重骰 → M5 短蹲 → M15 先锋/包匪 Hide(-1) 短蹲 & 士气缩放（事项 ITEM_M15_HIDE 的执行入口，兼 M9/M12 借用宿主）。
 *
 * 失败语义：回调内不判定事项成败；本钩子恒返回 MRES_Ignored（只做条件写入，不拦截原生）。
 *
 * @param pThis         HideState 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_HideState_OnEnter_Post(Address pThis, DHookParam hParams)
{
    Address preState = g_pPreInfiniteHideState;
    int preClient = g_iPreInfiniteHideClient;
    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    bool isInfiniteHide = (preState == pThis && preClient == client);

    // 虚表指纹捕获（M9 名册 Hide 真值门禁共用）：HideState 对象即 pThis，首址即虚表。
    // 放 M12 门禁与 M15 武装检查之前——借钩独占（M15 关/M12 开）时也要捕获；只记一次。
    if (g_pHideStateVTable == Address_Null)
    {
        g_pHideStateVTable = LoadAddressFromAddress(pThis);
    }

    // M12 camp 门禁：task 19 的 Hide 在此重骰；被重定向去 Hunt 的直接返回，不再缩放
    if (Module12_HandleSniperCamp(client))
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

    // 借钩方可能独占了本宿主（M15 关闭、M12 开启）：缩放只在 M15 事项武装时执行
    if (!Item_IsArmed(ITEM_M15_HIDE))
    {
        return MRES_Ignored;
    }

    // 读归一化后的 m_duration（-1 已在 OnEnter 本体内变成 30~60s），按士气伸缩后写回
    // 提示：引擎在随后的 HideState::OnUpdate 判定到达目标点（range < 20.0f）时才会正式调用 m_hideTimer.Start(m_duration)
    // 因此在 OnEnter Post 中修改 duration 绝对及时且全局生效
    float duration = view_as<float>(LoadFromAddress(pThis + view_as<Address>(g_iOffset_HideDuration), NumberType_Int32));

    // 下包前 M7 先锋与 plantDelay 到期后的持包者（及提前进点拿 PLANT_BOMB 者）命中 Hide(-1) 时改为短蹲：
    // 仅拦截 Pre 阶段捕获的原始 m_duration < 0.0（-1.0f 掩体占坑重寻），有限时长蹲（0s 换弹 / 1s 躲雷 /
    // 3~15s 撤退与埋伏）与下包后 Guard 守包 Hide(-1) 均不受此拦截，正常走士气缩放。
    if (isInfiniteHide && !g_bBombPlanted)
    {
        float earliest = Module2_GetEarliestPlantTime();
        bool plantOpen = (earliest <= 0.0 || GetGameTime() >= earliest);

        int pointman = Module7_GetPointman();
        int carrier = GetC4Carrier();
        bool isTarget = false;
        bool isPointman = false;

        if (pointman > 0 && client == pointman)
        {
            isTarget = true;
            isPointman = true;
        }
        else if (carrier > 0 && client == carrier && plantOpen)
        {
            isTarget = true;
        }
        else if (g_iOffset_Task > 0)
        {
            // plantDelay 未到但包匪自己踩进包点（Idle:166 会提前开下）时仍保底
            Address pBot = GetEntityAddress(client);
            if (pBot != Address_Null
                && LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == BOT_TASK_PLANT_BOMB)
            {
                isTarget = true;
            }
        }

        if (isTarget)
        {
            StoreToAddress(pThis + view_as<Address>(g_iOffset_HideDuration),
                view_as<int>(HIDE_SHORT_DURATION), NumberType_Int32);
            g_iTotalShortHides++;
            DebugLog("[BotRouteFix] [Module 15] Bot %N (%s) Hide(-1) -> %.1fs short hide (normalized %.1fs)",
                client, isPointman ? "pointman" : "carrier", HIDE_SHORT_DURATION, duration);
            return MRES_Ignored;
        }
    }

    int morale = GetBotMorale(client);
    float scaled = duration * (HIDE_DURATION_SCALE_BASE - HIDE_DURATION_SCALE_STEP * float(morale));
    StoreToAddress(pThis + view_as<Address>(g_iOffset_HideDuration), view_as<int>(scaled), NumberType_Int32);

    g_iTotalHideRescaled++;
    DebugLog("[BotRouteFix] [Module 15] Bot %N hide duration %.1fs -> %.1fs (morale %s%d)", client, duration, scaled, (morale >= 0) ? "+" : "", morale);
    return MRES_Ignored;
}

//========================================================================================
// MODULE 15 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用 Hide 时长事项（ITEM_M15_HIDE）：同步宿主钩子并按真实启用态武装
 *
 * 失败语义：挂钩失败属动态环境抖动，不证伪探测结论，只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = 事项已武装且宿主钩子在线
 */
static bool ApplyModule15_HideBackend()
{
    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

    if (!Item_IsReady(ITEM_M15_HIDE) || g_hHideDurationDetour == null)
    {
        Item_SetArmed(ITEM_M15_HIDE, false);
        if (g_hHideDurationDetour != null)
        {
            SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationPreHooked,
                false, Hook_HideState_OnEnter_Pre, false, "Module 15 Pre");
        }
        // 即使本事项不可用，也让宿主态与借钩方需求收敛（M12 可能仍在等钩子）
        SyncHideDurationDetourState();
        LogError("[BotRouteFix] [Module 15] Hide backend unavailable (ready=%d, detour=%d); hide item unarmed.",
            Item_IsReady(ITEM_M15_HIDE), g_hHideDurationDetour != null);
        return false;
    }

    // 先临时置位再同步：宿主开关按双方事项态判定，不置位会形成循环依赖
    Item_SetArmed(ITEM_M15_HIDE, true);
    if (!SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationPreHooked,
        false, Hook_HideState_OnEnter_Pre, true, "Module 15 Pre"))
    {
        Item_SetArmed(ITEM_M15_HIDE, false);
        SyncHideDurationDetourState();
        LogError("[BotRouteFix] [Module 15] Hide Pre hook enable failed; hide item unarmed.");
        return false;
    }

    if (!SyncHideDurationDetourState())
    {
        SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationPreHooked,
            false, Hook_HideState_OnEnter_Pre, false, "Module 15 Pre");
        Item_SetArmed(ITEM_M15_HIDE, false);
        LogError("[BotRouteFix] [Module 15] Hide host converge failed; hide item unarmed.");
        return false;
    }

    return true;
}

/**
 * 还原 Hide 时长事项（ITEM_M15_HIDE）：解除武装并按借钩双方需求收敛宿主钩子。
 *
 * 失败语义：无（幂等；解除武装 + 卸载独占 Pre + Post 宿主收敛）。
 */
static void RestoreModule15_HideBackend()
{
    Item_SetArmed(ITEM_M15_HIDE, false);
    g_pPreInfiniteHideState = Address_Null;
    g_iPreInfiniteHideClient = 0;

    if (g_hHideDurationDetour != null)
    {
        SyncDetourEnabled(g_hHideDurationDetour, g_bHideDurationPreHooked,
            false, Hook_HideState_OnEnter_Pre, false, "Module 15 Pre");
    }

    SyncHideDurationDetourState();
}
