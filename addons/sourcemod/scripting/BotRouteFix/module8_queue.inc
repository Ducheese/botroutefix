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
    }
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
    // 递延帧可能在停用之后才到，必须二次校验业务态
    if (!g_bModule8_Active)
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

public MRESReturn Hook_UpdatePathMovement_Post(int client, DHookParam hParams)
{
    if (!g_bModule8_Active)
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

    // 3. 累计打碎达 5 次（每次卡 2 秒，累计 ~10 秒死锁）：递延一帧强切 HuntState 解脱死锁
    g_iShatterCount[client]++;
    if (g_iShatterCount[client] >= QUEUE_SHATTER_LIMIT)
    {
        g_iShatterCount[client] = 0; // 触发后清零
        g_iTotalDeadlockHunt++;
        RequestFrame(Module8_FrameHunt, GetClientUserId(client));
        DebugLog("[BotRouteFix] [Module 8] Bot %N shattered %d times, deadlocked -> deferred Hunt state transition", client, QUEUE_SHATTER_LIMIT);
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

/**
 * 查询排队化解 UpdatePathMovement Detour 与 Hunt SDKCall 是否完全就绪
 */
bool IsQueueBreakerHooked()
{
    return (g_hUpdatePathDetour != null && g_bUpdatePathHooked && g_hSDKCall_Hunt != null);
}

/**
 * 获取累计打碎礼貌排队次数
 */
int GetTotalQueueShattered()
{
    return g_iTotalQueueShattered;
}

/**
 * 获取累计死锁强切 Hunt 次数
 */
int GetTotalDeadlockHunt()
{
    return g_iTotalDeadlockHunt;
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

    // 挂载 CCSBot::UpdatePathMovement Post 钩子
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
        EnableModule8_Queue(true);  // 待机：签名缺失，业务态开启、底层缺失（状态行显示 DEGRADED）
        return true;
    }

    DHookAddParam(g_hUpdatePathDetour, HookParamType_Bool);

    EnableModule8_Queue(true);
    if (!g_bModule8_Active)
    {
        delete g_hUpdatePathDetour;
        g_hUpdatePathDetour = null;
        LogError("[BotRouteFix] [Module 8] Failed to enable UpdatePathMovement Post detour!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 8] Polite queue breaker & 10s deadlock Hunt transition online [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
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

    if (enable)
    {
        if (g_hUpdatePathDetour == null)
        {
            // 待机态：签名缺失，无可挂载钩子；业务态仍记为开启
            g_bModule8_Active = true;
            return;
        }

        g_bModule8_Active = SyncDetourEnabled(g_hUpdatePathDetour, g_bUpdatePathHooked,
            true, Hook_UpdatePathMovement_Post, true, "Module 8");
        return;
    }

    SyncDetourEnabled(g_hUpdatePathDetour, g_bUpdatePathHooked, true, Hook_UpdatePathMovement_Post, false, "Module 8");
    Module8_OnRoundStart();
    g_bModule8_Active = false;
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

/**
 * 查询模块 8 业务开启态
 */
bool IsModule8_Active()
{
    return g_bModule8_Active;
}
