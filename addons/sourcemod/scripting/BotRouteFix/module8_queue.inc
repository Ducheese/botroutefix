#if defined _botroutefix_module8_queue_included
    #endinput
#endif
#define _botroutefix_module8_queue_included

//========================================================================================
// MODULE 8: POLITE QUEUE BREAKER & DOORWAY TURNAROUND (UpdatePathMovement Post hook)
//
// 门口/路口被队友挡路时的礼貌等待，原生要 2~5 秒（5-3*aggr）才散，还可能一头再扎回同一个堵点。
// 本模块在引擎每 think 评估等待的同一位置（CCSBot::UpdatePathMovement Post）介入：
//   - 等待超过 QUEUE_BREAKER_TIMEOUT (2.0s）即打碎排队
//   - 被堵 node 与下一个 node 各钉 +0.8 danger（封顶 2.5），A* 立即绕开，下次重算不走回头路
// 原生 2~5 秒那套还在后面兜底：我们先清了它到点就跳过；我们没赶上它照旧散。双向交接，无 timer 轮询。
//========================================================================================

#define QUEUE_BREAKER_TIMEOUT 2.0
#define QUEUE_NAIL_DANGER 0.8
#define QUEUE_NAIL_MAX 2.5

// 本模块私有状态（static，偏移与句柄不出本文件）
static Handle g_hUpdatePathDetour = null;
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

// 给指定 NavArea 按阵营钉 danger（先沉降再加，封顶防永久封路，刷时间戳）
static void Queue_NailDanger(Address pArea, int teamIdx, float curtime)
{
    if (pArea == Address_Null)
    {
        return;
    }

    float rawDanger = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), NumberType_Int32));
    float dangerTimestamp = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), NumberType_Int32));

    float elapsed = curtime - dangerTimestamp;
    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }

    float effDanger = rawDanger - (elapsed / 120.0);
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

public MRESReturn Hook_UpdatePathMovement_Post(int client, DHookParam hParams)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
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

    float elapsedWait = GetGameTime() - (timestamp - duration);
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
    float curtime = GetGameTime();
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

    // 3. 碎路径（引擎 DestroyPath 手工版）+ 礼貌 timer 置 -1 去残留，逼立即重算
    StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PathIndex), 0, NumberType_Int32);
    StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
    StoreToAddress(pBot + view_as<Address>(g_iOffset_PoliteTimer + g_iOffset_TimerTimestamp), view_as<int>(-1.0), NumberType_Int32);

    g_iTotalQueueShattered++;
    DebugLog("[BotRouteFix] [Module 8] Bot %N broke %.1fs polite queue, nailed doorway danger", client, elapsedWait);
    return MRES_Ignored;
}

/**
 * 查询 UpdatePathMovement Detour 是否处于就绪/已挂载状态
 */
bool IsQueueBreakerHooked()
{
    return (g_hUpdatePathDetour != null);
}

/**
 * 获取累计打碎礼貌排队次数
 */
int GetTotalQueueShattered()
{
    return g_iTotalQueueShattered;
}

/**
 * 初始化模块 8（读偏移 + 挂 UpdatePathMovement Post）
 * 若 gamedata 尚未填入 "CCSBot_UpdatePathMovement" 签名，则优雅进入待机状态（不阻断全库启动）
 */
bool InitModule8_Queue()
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

    if (g_iOffset_Waiting == -1 || g_iOffset_PoliteTimer == -1 || g_iOffset_PathLength == -1
        || g_iOffset_TimerDuration == -1 || g_iOffset_TimerTimestamp == -1 || g_iOffset_IsStopping == -1
        || g_iOffset_PathLadder == -1 || g_iOffset_Path == -1 || g_iPathStride == -1
        || g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1)
    {
        LogError("[BotRouteFix] [Module 8] Failed to read one or more queue/path offsets from gamedata!");
        return false;
    }

    g_hUpdatePathDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Int, ThisPointer_CBaseEntity);
    if (g_hUpdatePathDetour == null)
    {
        LogError("[BotRouteFix] [Module 8] Failed to create DHook detour instance for UpdatePathMovement!");
        return false;
    }

    if (!DHookSetFromConf(g_hUpdatePathDetour, g_hGameConf, SDKConf_Signature, "CCSBot_UpdatePathMovement"))
    {
        LogMessage("[BotRouteFix] [Module 8] Signature 'CCSBot_UpdatePathMovement' pending in gamedata (prototype mode). Module 8 is on standby.");
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
        return true;
    }

    DHookAddParam(g_hUpdatePathDetour, HookParamType_Bool);

    if (!DHookEnableDetour(g_hUpdatePathDetour, true, Hook_UpdatePathMovement_Post))
    {
        LogError("[BotRouteFix] [Module 8] Failed to enable UpdatePathMovement Post detour!");
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
        return false;
    }

    LogMessage("[BotRouteFix] [Module 8] Polite queue breaker online (UpdatePathMovement Post hook)");
    return true;
}

/**
 * 还原模块 8（摘钩子）
 */
void RestoreModule8_Queue()
{
    if (g_hUpdatePathDetour != null)
    {
        DHookDisableDetour(g_hUpdatePathDetour, true, Hook_UpdatePathMovement_Post);
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
    }
}

/**
 * 动态启用或停用模块 8
 */
void EnableModule8_Queue(bool enable)
{
    if (g_hUpdatePathDetour == null)
    {
        return;
    }

    if (enable)
    {
        DHookEnableDetour(g_hUpdatePathDetour, true, Hook_UpdatePathMovement_Post);
    }
    else
    {
        DHookDisableDetour(g_hUpdatePathDetour, true, Hook_UpdatePathMovement_Post);
    }
}
