#if defined _botroutefix_module8_queue_included
    #endinput
#endif
#define _botroutefix_module8_queue_included

//========================================================================================
// MODULE 8: POLITE QUEUE BREAKER & DEADLOCK HUNT TRANSITION (UpdatePathMovement Post hook)
//
// 门口/路口被队友挡路时的礼貌等待，原生要 2~5 秒（5-3*aggr）才散，还可能一头再扎回同一个堵点。
// 本模块在引擎每 think 评估等待的同一位置（CCSBot::UpdatePathMovement Post）介入：
//   - 等待超过 QUEUE_BREAKER_TIMEOUT (2.0s）即打碎排队
//   - 被堵 node 与下一个 node 各钉 +0.8 danger（封顶 2.5），A* 立即绕开，下次重算不走回头路
//   - 连续打碎达到 QUEUE_SHATTER_LIMIT (5次，约等待 10 秒) 判定为顽固死锁：
//     递延一帧调用 CCSBot::Hunt 将其强切为 HuntState，彻底清空导致死锁的 Task (转为 SEEK_AND_DESTROY)，
//     使 Bot 放弃僵尸执念，重新选路奔赴战场。
// 原生 2~5 秒那套还在后面兜底：我们先清了它到点就跳过；我们没赶上它照旧散。双向交接，无 timer 轮询。
//========================================================================================

//========================================================================================
// MODULE 8 DEFINES & GLOBALS
//========================================================================================

#define QUEUE_BREAKER_TIMEOUT   2.0
#define QUEUE_SHATTER_LIMIT     5       // 连续打碎达到 5 次（约等待 10 秒），判定为顽固死锁
#define QUEUE_NAIL_DANGER       0.8
#define QUEUE_NAIL_MAX          2.5
// 沉降速率 DANGER_DECAY_RATE_SEC 统一定义于 global.inc

// 本模块私有状态（static，偏移与句柄不出本文件）
static bool g_bModule8_Active = false;      // 业务开启态
static bool g_bUpdatePathHooked = false;    // per-hook 真实启用态
static Handle g_hUpdatePathDetour = null;
static int g_iShatterCount[MAXPLAYERS + 1];
static int g_iOffset_Waiting = -1;
static int g_iOffset_PoliteTimer = -1;
static int g_iOffset_TimerDuration = -1;
static int g_iOffset_TimerTimestamp = -1;
static int g_iOffset_PathLength = -1;
static int g_iOffset_PathIndex = -1;
static int g_iOffset_IsStopping = -1;
static int g_iOffset_PathLadder = -1;
static int g_iOffset_Path = -1;
static int g_iPathStride = -1;
static int g_iOffset_Danger = -1;
static int g_iOffset_DangerTimestamp = -1;
static int g_iPtrSize = 4;

// 遥测统计计数器
static int g_iTotalQueueShattered = 0;
static int g_iTotalDeadlockHunt = 0;

// 可视化追踪：因堵路加过 danger 的 NavArea (持续至少 60 秒) 与死锁解脱 Bot
#define MAX_BLOCKED_AREAS 64
static Address g_BlockedAreas[MAX_BLOCKED_AREAS];
static float g_flBlockedAreaExpire[MAX_BLOCKED_AREAS];
static int g_iBlockedAreaCount = 0;
static float g_flDeadlockBrokenUntil[MAXPLAYERS + 1];

//========================================================================================
// MODULE 8 ITEM BACKENDS: QUEUE SHATTER (UpdatePathMovement Post) + DEADLOCK HUNT (Hunt SDKCall)
//========================================================================================

/**
 * 事项后端：排队打碎（ITEM_M8_QUEUE）
 *
 * 只读偏移、挂载 UpdatePathMovement Post 钩子，不启用钩子（启用由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把排队事项标记为 FAILED，死锁事项不受影响。
 *
 * @return              true = 排队事项后端就绪
 */
static bool PrepModule8_QueueBackend()
{
    g_iOffset_Waiting        = GameConfGetOffset(g_hGameConf, "CCSBot_IsWaitingBehindFriend_Offset");
    g_iOffset_PoliteTimer    = GameConfGetOffset(g_hGameConf, "CCSBot_PoliteTimer_Offset");
    g_iOffset_TimerDuration  = GameConfGetOffset(g_hGameConf, "CountdownTimer_Duration_Offset");
    g_iOffset_TimerTimestamp = GameConfGetOffset(g_hGameConf, "CountdownTimer_Timestamp_Offset");
    g_iOffset_PathLength     = GameConfGetOffset(g_hGameConf, "CCSBot_PathLength_Offset");
    g_iOffset_Path           = GameConfGetOffset(g_hGameConf, "CCSBot_Path_Offset");
    g_iPathStride            = GameConfGetOffset(g_hGameConf, "CCSBot_PathStride");
    g_iOffset_IsStopping     = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");
    g_iOffset_PathLadder     = GameConfGetOffset(g_hGameConf, "CCSBot_PathLadder_Offset");
    g_iOffset_Danger         = GameConfGetOffset(g_hGameConf, "NavArea_Danger_Offset");
    g_iOffset_DangerTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_DangerTimestamp_Offset");
    g_iPtrSize               = GameConfGetOffset(g_hGameConf, "Pointer_Size");

    if (g_iPtrSize <= 0)
    {
        g_iPtrSize = g_bIsWin64 ? 8 : 4;
    }

    g_iOffset_PathIndex = g_iOffset_PathLength + 4; // C++ 结构体紧随 m_pathLength 之后

    bool offsetsOk = (g_iOffset_Waiting > 0 && g_iOffset_PoliteTimer > 0 && g_iOffset_PathLength > 0
        && g_iOffset_TimerDuration > 0 && g_iOffset_TimerTimestamp > 0 && g_iOffset_IsStopping > 0
        && g_iOffset_PathLadder > 0 && g_iOffset_Path > 0 && g_iPathStride > 0
        && g_iOffset_Danger > 0 && g_iOffset_DangerTimestamp > 0);
    if (!offsetsOk)
    {
        LogError("[BotRouteFix] [Module 8] Queue/path offsets missing; queue item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 8] Queue offsets verified (waiting=%d, polite=%d, duration=%d, timestamp=%d, pathlen=%d, path=%d, stride=%d, stopping=%d, ladder=%d, danger=%d, dangerts=%d).",
        g_iOffset_Waiting, g_iOffset_PoliteTimer, g_iOffset_TimerDuration, g_iOffset_TimerTimestamp,
        g_iOffset_PathLength, g_iOffset_Path, g_iPathStride, g_iOffset_IsStopping, g_iOffset_PathLadder,
        g_iOffset_Danger, g_iOffset_DangerTimestamp);

    // 挂载 CCSBot::UpdatePathMovement Post 钩子（只挂载不启用）
    g_hUpdatePathDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Int, ThisPointer_CBaseEntity);
    // 纯 hook 创建不占检查点行（成功静默；失败走上面的事项 FAILED 错误行）。
    if (g_hUpdatePathDetour == null)
    {
        LogError("[BotRouteFix] [Module 8] UpdatePathMovement detour creation failed; queue item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hUpdatePathDetour, g_hGameConf, SDKConf_Signature, "CCSBot_UpdatePathMovement"))
    {
        LogError("[BotRouteFix] [Module 8] UpdatePathMovement signature missing; queue item FAILED.");
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
        return false;
    }

    DHookAddParam(g_hUpdatePathDetour, HookParamType_Bool);
    return true;
}

/**
 * 事项后端：死锁强切（ITEM_M8_DEADLOCK）
 *
 * 只校验 Hunt SDKCall 句柄。死锁事项复用排队事项的 Post 钩子做等待检测，
 * 强切本身走 SDKCall，因此本 Prep 不挂载任何钩子。
 * 失败返回 false，Init 会把死锁事项标记为 FAILED，排队事项不受影响。
 *
 * @return              true = 死锁事项后端就绪
 */
static bool PrepModule8_DeadlockBackend()
{
    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [Module 8] Hunt SDKCall missing; deadlock item FAILED.");
        return false;
    }
    LogMessage("[BotRouteFix] [Module 8] Deadlock Hunt SDKCall verified.");
    return true;
}


//========================================================================================
// MODULE 8 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 8（读偏移 + 挂 UpdatePathMovement Post）
 * 若 gamedata 尚未填入 "CCSBot_UpdatePathMovement" 签名，则优雅进入待机状态（不阻断全库启动）
 */
bool InitModule8_Queue()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 8] GameConf missing; both items FAILED.");
        Item_SetReady(ITEM_M8_QUEUE, false);
        Item_SetReady(ITEM_M8_DEADLOCK, false);
        return false;
    }

    // 排队打碎与死锁 Hunt 是两个事项：任一 Prep 失败只熔断自身事项。
    Item_SetReady(ITEM_M8_QUEUE, PrepModule8_QueueBackend());
    Item_SetReady(ITEM_M8_DEADLOCK, PrepModule8_DeadlockBackend());

    EnableModule8_Queue(true);
    if (!g_bModule8_Active)
    {
        LogModuleAllFailed(8);
        return false;
    }

    char armedDetail[64];
    Format(armedDetail, sizeof(armedDetail), "queue=%d, deadlock=%d",
        Item_IsArmed(ITEM_M8_QUEUE), Item_IsArmed(ITEM_M8_DEADLOCK));
    LogModuleArmed(8, "Queue", armedDetail);
    return true;
}

/**
 * 动态启用或停用模块 8（幂等）
 */
void EnableModule8_Queue(bool enable)
{
    if (enable == g_bModule8_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule8_QueueBackend();
        RestoreModule8_DeadlockBackend();
        Module8_OnRoundStart();
        g_bModule8_Active = false;
        return;
    }

    // ITEM_M8_QUEUE / ITEM_M8_DEADLOCK 独立武装；任一事项武装即视为模块部分激活
    bool queueArmed = ApplyModule8_QueueBackend();
    bool deadlockArmed = ApplyModule8_DeadlockBackend();
    g_bModule8_Active = (queueArmed || deadlockArmed);

    if (g_bModule8_Active && (!queueArmed || !deadlockArmed))
    {
        char partialDetail[64];
        Format(partialDetail, sizeof(partialDetail), "queue=%d, deadlock=%d", queueArmed, deadlockArmed);
        LogModulePartial(8, partialDetail);
    }
}

/**
 * 还原模块 8（插件卸载：摘钩并释放句柄）
 */
void RestoreModule8_Queue()
{
    EnableModule8_Queue(false);

    if (g_hUpdatePathDetour != null)
    {
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
    }
}


//========================================================================================
// MODULE 8 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 获取累计打碎礼貌排队次数
 */
int Module8_GetTotalQueueShattered()
{
    return g_iTotalQueueShattered;
}

/**
 * 获取累计死锁强切 Hunt 次数
 */
int Module8_GetTotalDeadlockHunt()
{
    return g_iTotalDeadlockHunt;
}

/**
 * 获取当前所有处于 60 秒高亮期内的堵路 NavArea
 *
 * @param areas         输出 NavArea 指针数组
 * @param expires       输出各 Area 的过期时间戳
 * @return              实际有效数量
 */
int Module8_GetBlockedAreas(Address areas[MAX_BLOCKED_AREAS], float expires[MAX_BLOCKED_AREAS])
{
    float curtime = GetGameTime();
    int count = 0;
    for (int i = 0; i < g_iBlockedAreaCount; i++)
    {
        if (g_BlockedAreas[i] != Address_Null && curtime < g_flBlockedAreaExpire[i])
        {
            areas[count] = g_BlockedAreas[i];
            expires[count] = g_flBlockedAreaExpire[i];
            count++;
        }
    }
    return count;
}

/**
 * 判断指定 Bot 当前是否刚被打破死锁强切 Hunt (4秒高亮期内)
 */
bool Module8_IsDeadlockBroken(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
    {
        return false;
    }
    return (GetGameTime() < g_flDeadlockBrokenUntil[client]);
}


//========================================================================================
// MODULE 8 EVENT HANDLERS
//========================================================================================

/**
 * 回合重置：清空所有 Bot 的打碎计数
 *
 * 【例外：不随业务态守卫】本函数同时被 EnableModule8_Queue(false) 作为内部重置复用，
 *   依赖"在置假之前完成清理"的调用顺序；加守卫会让关闭流程的清理失效。
 */
void Module8_OnRoundStart()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        g_iShatterCount[i] = 0;
        g_flDeadlockBrokenUntil[i] = 0.0;
    }
    g_iBlockedAreaCount = 0;
}

/**
 * 地图启动重置：换图清空排队打碎计数，消除跨图槽位复用残留
 */
void Module8_OnMapStart()
{
    Module8_OnRoundStart();
}

/**
 * 客户端掉线事件：清空该槽位打碎计数，防止新 Bot 复用槽位时误继承
 */
void Module8_OnClientDisconnect(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        g_iShatterCount[client] = 0;
    }
}

/**
 * 递延一帧转 Hunt（帧外切状态才安全），彻底清空导致死锁的 Task 并全局重新选路
 */
public void Module8_FrameHunt(any data)
{
    // 递延帧可能在停用之后才到，必须二次校验事项武装态
    if (!Item_IsArmed(ITEM_M8_DEADLOCK))
    {
        return;
    }

    int client = GetClientOfUserId(view_as<int>(data));
    if (!IsBot(client, true))
    {
        return;
    }

    BotHunt(client);
    DebugLog("[BotRouteFix] [Module 8] Bot %N transitioned to HuntState (deadlock broken)", client);
}


//========================================================================================
// MODULE 8 DETOUR & HOOKS (UPDATEPATHMOVEMENT::POST)
//========================================================================================

/**
 * 事项 M8 排队打碎 / 死锁检测的唯一入口钩子（UpdatePathMovement Post）
 *
 * ITEM_M8_QUEUE 武装才介入：等待超 2 秒即打碎排队（清等待旗、钉 danger、碎路径），
 * 累计打碎 5 次且 ITEM_M8_DEADLOCK 武装则递延切 Hunt。条件不符一律 MRES_Ignored 放行。
 */
public MRESReturn Hook_UpdatePathMovement_Post(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M8_QUEUE))
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return MRES_Ignored;
    }

    // 不在礼貌等待中，或手里没路：原生自己会处理，直接放行
    if (LoadFromAddress(pBot + view_as<Address>(g_iOffset_Waiting), NumberType_Int8) == 0)
    {
        return MRES_Ignored;
    }

    int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
    if (pathLength <= 0)
    {
        return MRES_Ignored;
    }

    float duration = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_PoliteTimer + g_iOffset_TimerDuration), NumberType_Int32));
    float timestamp = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_PoliteTimer + g_iOffset_TimerTimestamp), NumberType_Int32));
    if (duration <= 0.0)
    {
        return MRES_Ignored;
    }

    float curtime = GetGameTime();
    float elapsedWait = curtime - (timestamp - duration);
    if (elapsedWait < QUEUE_BREAKER_TIMEOUT)
    {
        return MRES_Ignored;
    }

    int team = GetClientTeam(client);
    if (team != 2 && team != 3)
    {
        return MRES_Ignored;
    }

    // danger 下标与 danger 系统同约定：teamID % 2（T=0/CT=1），别和 hunt 的 (team-1)%2 搞混
    int teamIdx = team - 2;

    // 1. 清等待旗
    StoreToAddress(pBot + view_as<Address>(g_iOffset_Waiting), 0, NumberType_Int8);

    // 2. 钉住当前被堵 node 与下一个 node，逼 A* 绕开
    int pathIndex = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathIndex), NumberType_Int32);
    if (pathIndex < 0)
    {
        pathIndex = 0;
    }
    if (pathIndex >= pathLength)
    {
        pathIndex = pathLength - 1;
    }

    Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + pathIndex * g_iPathStride);
    Queue_NailDanger(LoadAddressFromAddress(pConnectInfo), teamIdx, curtime);

    int nextIdx = pathIndex + 1;
    if (nextIdx < pathLength)
    {
        Address pNextConnectInfo = pBot + view_as<Address>(g_iOffset_Path + nextIdx * g_iPathStride);
        Queue_NailDanger(LoadAddressFromAddress(pNextConnectInfo), teamIdx, curtime);
    }

    // 3. 死锁 Hunt 事项：累计打碎达 5 次（每次卡 2 秒，累计 ~10 秒死锁）后递延强切 HuntState
    if (Item_IsArmed(ITEM_M8_DEADLOCK))
    {
        g_iShatterCount[client]++;
        if (g_iShatterCount[client] >= QUEUE_SHATTER_LIMIT)
        {
            g_iShatterCount[client] = 0; // 触发后清零
            g_iTotalDeadlockHunt++;
            g_flDeadlockBrokenUntil[client] = curtime + 4.0; // 持续 4 秒高亮醒目标识
            RequestFrame(Module8_FrameHunt, GetClientUserId(client));
            DebugLog("[BotRouteFix] [Module 8] Bot %N shattered %d times, deadlocked -> deferred Hunt state transition", client, QUEUE_SHATTER_LIMIT);
        }
    }

    // 4. 碎路径（引擎 DestroyPath 手工版）+ 礼貌 timer 置 -1 去残留，逼立即重算
    StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PathIndex), 0, NumberType_Int32);
    StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PoliteTimer + g_iOffset_TimerTimestamp), view_as<int>(-1.0), NumberType_Int32);

    g_iTotalQueueShattered++;
    DebugLog("[BotRouteFix] [Module 8] Bot %N broke %.1fs polite queue, nailed doorway danger", client, elapsedWait);
    return MRES_Ignored;
}


//========================================================================================
// MODULE 8 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用排队打碎事项：只有 Prep 已就绪才启用 Post 钩子
 *
 * 事项 ITEM_M8_QUEUE 的应用入口；不做偏移读取与钩子挂载（那是 Prep 的职责）。
 * 失败时该事项 FAILED，死锁事项不受影响。
 *
 * @return              true = 排队钩子已驻留
 */
static bool ApplyModule8_QueueBackend()
{
    if (!Item_IsReady(ITEM_M8_QUEUE) || g_hUpdatePathDetour == null)
    {
        Item_SetArmed(ITEM_M8_QUEUE, false);
        return false;
    }

    if (!SyncDetourEnabled(g_hUpdatePathDetour, g_bUpdatePathHooked,
        true, Hook_UpdatePathMovement_Post, true, "Module 8"))
    {
        // 挂钩失败属动态环境抖动，不证伪探测结论：只清 Armed 保留 Ready，下次 Enable 重试
        Item_SetArmed(ITEM_M8_QUEUE, false);
        LogError("[BotRouteFix] [Module 8] Queue detour enable failed; queue item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M8_QUEUE, true);
    return true;
}

/**
 * 应用死锁强切事项：只有排队钩子已驻留且本事项就绪才武装
 *
 * 死锁事项复用排队事项的 Post 钩子做等待检测，强切本身走 Hunt SDKCall，
 * 因此本 Apply 不做任何挂钩动作，只做武装门控。
 *
 * @return              true = 死锁事项已武装
 */
static bool ApplyModule8_DeadlockBackend()
{
    bool ok = (Item_IsArmed(ITEM_M8_QUEUE) && Item_IsReady(ITEM_M8_DEADLOCK));
    Item_SetArmed(ITEM_M8_DEADLOCK, ok);
    return ok;
}

/**
 * 还原排队打碎事项：摘钩并解除武装
 */
static void RestoreModule8_QueueBackend()
{
    SyncDetourEnabled(g_hUpdatePathDetour, g_bUpdatePathHooked, true, Hook_UpdatePathMovement_Post, false, "Module 8");
    Item_SetArmed(ITEM_M8_QUEUE, false);
}

/**
 * 还原死锁强切事项：解除武装（钩子由排队事项释放）
 */
static void RestoreModule8_DeadlockBackend()
{
    Item_SetArmed(ITEM_M8_DEADLOCK, false);
}


//========================================================================================
// MODULE 8 QUEUE PRIMITIVES
//========================================================================================

/**
 * 事项 M8 排队打碎的可视化追踪登记：记下加过 danger 的 NavArea
 *
 * 同一 Area 重复登记只刷新过期时间；满 64 个挤掉最早过期的。空地址直接返回。
 */
static void RecordBlockedArea(Address pArea, float expireTime)
{
    if (pArea == Address_Null)
    {
        return;
    }

    for (int i = 0; i < g_iBlockedAreaCount; i++)
    {
        if (g_BlockedAreas[i] == pArea)
        {
            if (expireTime > g_flBlockedAreaExpire[i])
            {
                g_flBlockedAreaExpire[i] = expireTime;
            }
            return;
        }
    }

    if (g_iBlockedAreaCount < MAX_BLOCKED_AREAS)
    {
        g_BlockedAreas[g_iBlockedAreaCount] = pArea;
        g_flBlockedAreaExpire[g_iBlockedAreaCount] = expireTime;
        g_iBlockedAreaCount++;
        return;
    }

    int oldestIdx = 0;
    float oldestTime = g_flBlockedAreaExpire[0];
    for (int i = 1; i < MAX_BLOCKED_AREAS; i++)
    {
        if (g_flBlockedAreaExpire[i] < oldestTime)
        {
            oldestTime = g_flBlockedAreaExpire[i];
            oldestIdx = i;
        }
    }
    g_BlockedAreas[oldestIdx] = pArea;
    g_flBlockedAreaExpire[oldestIdx] = expireTime;
}

/**
 * 事项 M8 排队打碎的钉 danger：给指定 NavArea 按阵营钉 +0.8（封顶 2.5）
 *
 * 先按沉降公式结算旧值再加，刷时间戳；逼 A* 立即绕开，下次重算不走回头路。
 */
static void Queue_NailDanger(Address pArea, int teamIdx, float curtime)
{
    if (pArea == Address_Null)
    {
        return;
    }

    RecordBlockedArea(pArea, curtime + 60.0);

    float rawDanger = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), NumberType_Int32));
    float dangerTimestamp = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), NumberType_Int32));

    float elapsed = curtime - dangerTimestamp;
    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }

    float effDanger = rawDanger - (elapsed / DANGER_DECAY_RATE_SEC);
    if (effDanger < 0.0)
    {
        effDanger = 0.0;
    }

    float newDanger = effDanger + QUEUE_NAIL_DANGER;
    if (newDanger > QUEUE_NAIL_MAX)
    {
        newDanger = QUEUE_NAIL_MAX;
    }

    StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), view_as<int>(newDanger), NumberType_Int32);
    StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), view_as<int>(curtime), NumberType_Int32);
}
