#if defined _botroutefix_nav_measure_included
    #endinput
#endif
#define _botroutefix_nav_measure_included

//========================================================================================
// NAV MEASURE SUBSYSTEM
//
// 真实地面行走距离 + 临时走廊探测：
// 备份关键导航状态 → 解 repath 节流 → ComputePath 实测 → 累加节点间距/可选采集 NavArea → 还原。
// 寻路失败返回 -1.0；爬梯中（m_pathLadder != NULL）直接跳过探测，避免梯子子状态错配。
//
// 快照范围：路径数组/长度/下标、梯子指针、阻挡旗、当前与最后已知区域、目标坐标、
// repath 计时器、区域进入时间。spot 遭遇点、初始遭遇区、梯子子状态与视线朝向未纳入快照
// （低频、下一拍可自愈），因此是"关键导航状态无副作用"，不是严格零副作用。
//
// 不是 SDKCall 包裹，是独立测量子系统（借用 ComputePath 句柄做实测引擎）。
// 软加载：偏移缺失只记日志，测距整体待机，不杀插件。
//========================================================================================

#define NAV_MEASURE_MAX_PATH_NODES  256     // 与 cs_bot.h 的 MAX_PATH_LENGTH 对齐

// 探针输出 scratch（单线程使用，调用后立即消费；避免 Address[] 动态数组参数的写回语义问题）
static Address g_NavProbeAreas[NAV_MEASURE_MAX_PATH_NODES];
static int g_iNavProbeCount = 0;

// 无损测距偏移组
static int g_iNav_Path         = -1;
static int g_iNav_PathStride   = -1;
static int g_iNav_PathLength   = -1;
static int g_iNav_PathLadder   = -1;
static int g_iNav_IsStopping   = -1;
static int g_iNav_Task         = -1;
static int g_iNav_ConnectPos   = -1;
static int g_iNav_TimerDur     = -1;
static int g_iNav_TimerTs      = -1;
static int g_iNav_PtrSize      = -1;
static bool g_bNavMeasureReady = false;

/**
 * 加载无损测距所需偏移（软加载：缺失只记日志，测距整体待机）
 */
bool InitNavMeasure()
{
    if (g_hGameConf == null)
    {
        return false;
    }

    g_iNav_Path       = GameConfGetOffset(g_hGameConf, "CCSBot_Path_Offset");
    g_iNav_PathStride = GameConfGetOffset(g_hGameConf, "CCSBot_PathStride");
    g_iNav_PathLength = GameConfGetOffset(g_hGameConf, "CCSBot_PathLength_Offset");
    g_iNav_PathLadder = GameConfGetOffset(g_hGameConf, "CCSBot_PathLadder_Offset");
    g_iNav_IsStopping = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");
    g_iNav_Task       = GameConfGetOffset(g_hGameConf, "CCSBot_Task_Offset");
    g_iNav_ConnectPos = GameConfGetOffset(g_hGameConf, "ConnectInfo_Vector_Offset");
    g_iNav_TimerDur   = GameConfGetOffset(g_hGameConf, "CountdownTimer_Duration_Offset");
    g_iNav_TimerTs    = GameConfGetOffset(g_hGameConf, "CountdownTimer_Timestamp_Offset");
    g_iNav_PtrSize    = GameConfGetOffset(g_hGameConf, "Pointer_Size");

    if (g_hSDKCall_ComputePath == null || g_iNav_Path <= 0 || g_iNav_PathStride <= 0 ||
        g_iNav_PathLength <= 0 || g_iNav_PathLadder <= 0 || g_iNav_IsStopping <= 0 ||
        g_iNav_Task <= 0 || g_iNav_ConnectPos <= 0 || g_iNav_TimerDur <= 0 ||
        g_iNav_TimerTs <= 0 || g_iNav_PtrSize <= 0)
    {
        LogMessage("[BotRouteFix] [NavMeasure] Offsets incomplete, walk-distance measurement on standby.");
        return false;
    }

    g_bNavMeasureReady = true;
    return true;
}

/**
 * 无损测距是否就绪
 */
stock bool BotNavReady()
{
    return g_bNavMeasureReady;
}

/**
 * 路径探测核心：备份关键导航状态 → 解 repath 节流 → ComputePath 实测 → 读取路径 NavArea → 还原
 *
 * CCSBot::ComputePath 本体先 DestroyPath() 再写入新路径（cs_bot_pathfind.cpp:1877-1891），
 * 直接调用会篡改 Bot 当前行进路径。本函数通过快照/还原保证关键导航状态无副作用；
 * 爬梯中直接跳过（梯子子状态不在快照范围）。
 * 供测距（只取距离）与走廊钉刺（只取 NavArea 指针）共用。
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @param maxAreas  采集上限（0 = 只测距，不采集）
 * @param distance  输出真实地面行走距离 units（失败 -1.0）
 * @return          实际采集的 NavArea 数量（0 = 失败/无路径/不采集）
 */
static int NavMeasure_ProbePath(int client, const float target[3], int maxAreas, float &distance)
{
    distance = -1.0;
    g_iNavProbeCount = 0;

    if (!g_bNavMeasureReady || !IsBot(client, true))
    {
        return 0;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return 0;
    }

    // 0. 爬梯中直接跳过探测：ComputePath → SetupLadderMovement 会改写梯子子状态
    //    （m_pathLadderState/FaceIn/Timestamp、m_lookAheadAngle），这些字段不在快照范围内；
    //    返回 0/-1 让调用方按"不可达"处理，避免梯子状态错配。
    Address oldPathLadder = LoadAddressFromAddress(pBot + view_as<Address>(g_iNav_PathLadder));
    if (oldPathLadder != Address_Null)
    {
        return 0;
    }

    // 1. 备份移动状态（非破坏性快照）
    int pathLengthOffset = g_iNav_PathLength;
    int pathIndexOffset = g_iNav_PathLength + 4;   // m_pathIndex
    int areaEnteredOffset = g_iNav_PathLength + 8; // m_areaEnteredTimestamp
    int oldPathLength = LoadFromAddress(pBot + view_as<Address>(pathLengthOffset), NumberType_Int32);
    int oldPathIndex = LoadFromAddress(pBot + view_as<Address>(pathIndexOffset), NumberType_Int32);
    int oldAreaEntered = LoadFromAddress(pBot + view_as<Address>(areaEnteredOffset), NumberType_Int32);
    int oldIsStopping = LoadFromAddress(pBot + view_as<Address>(g_iNav_IsStopping), NumberType_Int8);

    int currentAreaOffset = g_iNav_Task + 24;
    int lastKnownAreaOffset = currentAreaOffset + g_iNav_PtrSize;
    Address oldCurrentArea = LoadAddressFromAddress(pBot + view_as<Address>(currentAreaOffset));
    Address oldLastKnownArea = LoadAddressFromAddress(pBot + view_as<Address>(lastKnownAreaOffset));

    int goalOffset = g_iNav_Task + 8;
    int oldGoalX = LoadFromAddress(pBot + view_as<Address>(goalOffset), NumberType_Int32);
    int oldGoalY = LoadFromAddress(pBot + view_as<Address>(goalOffset + 4), NumberType_Int32);
    int oldGoalZ = LoadFromAddress(pBot + view_as<Address>(goalOffset + 8), NumberType_Int32);

    int repathOffset = g_iNav_PathLength + (g_bIsWin64 ? 16 : 12);
    int oldRepathDuration = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerDur), NumberType_Int32);
    int oldRepathTimestamp = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerTs), NumberType_Int32);

    // 解 repath 节流，保证本次实测不被 0.4 秒节流挡掉
    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerTs), view_as<int>(-1.0), NumberType_Int32);

    int dwordsPerNode = g_iNav_PathStride / 4;
    int clampedOldLength = oldPathLength;
    if (clampedOldLength < 0)
    {
        clampedOldLength = 0;
    }
    if (clampedOldLength > 256)
    {
        clampedOldLength = 256;
    }

    int oldPathBuffer[2048];
    int totalDwords = clampedOldLength * dwordsPerNode;
    if (totalDwords > 2048)
    {
        totalDwords = 2048;
    }

    for (int d = 0; d < totalDwords; d++)
    {
        oldPathBuffer[d] = LoadFromAddress(pBot + view_as<Address>(g_iNav_Path + d * 4), NumberType_Int32);
    }

    // 2. 实测（距离 + 可选 NavArea 采集）
    bool hasPath = SDKCall(g_hSDKCall_ComputePath, client, target, 0);
    int writtenAreas = 0;
    float totalDist = -1.0;

    if (hasPath)
    {
        int newPathLength = LoadFromAddress(pBot + view_as<Address>(pathLengthOffset), NumberType_Int32);

        // 防御性 clamp：引擎保证 ≤ MAX_PATH_LENGTH，但偏移漂移时防止读穿路径数组
        if (newPathLength > NAV_MEASURE_MAX_PATH_NODES)
        {
            newPathLength = NAV_MEASURE_MAX_PATH_NODES;
        }
        if (newPathLength < 0)
        {
            newPathLength = 0;
        }

        if (newPathLength > 0)
        {
            totalDist = 0.0;
            float prevPos[3];
            GetClientAbsOrigin(client, prevPos);

            for (int i = 0; i < newPathLength; i++)
            {
                Address pConnectInfo = pBot + view_as<Address>(g_iNav_Path + i * g_iNav_PathStride);

                if (writtenAreas < maxAreas && writtenAreas < NAV_MEASURE_MAX_PATH_NODES)
                {
                    g_NavProbeAreas[writtenAreas] = LoadAddressFromAddress(pConnectInfo);
                    writtenAreas++;
                }

                float nodePos[3];
                nodePos[0] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(g_iNav_ConnectPos), NumberType_Int32));
                nodePos[1] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(g_iNav_ConnectPos + 4), NumberType_Int32));
                nodePos[2] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(g_iNav_ConnectPos + 8), NumberType_Int32));

                totalDist += GetVectorDistance(prevPos, nodePos);
                prevPos = nodePos;
            }

            totalDist += GetVectorDistance(prevPos, target);
        }
    }

    // 3. 还原关键导航状态（非全量：spot/初始遭遇区/梯子子状态不在快照内）
    for (int d = 0; d < totalDwords; d++)
    {
        StoreToAddress(pBot + view_as<Address>(g_iNav_Path + d * 4), oldPathBuffer[d], NumberType_Int32);
    }

    StoreToAddress(pBot + view_as<Address>(pathLengthOffset), oldPathLength, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(pathIndexOffset), oldPathIndex, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(areaEnteredOffset), oldAreaEntered, NumberType_Int32);

    StoreAddressToAddress(pBot + view_as<Address>(currentAreaOffset), oldCurrentArea);
    StoreAddressToAddress(pBot + view_as<Address>(lastKnownAreaOffset), oldLastKnownArea);

    StoreToAddress(pBot + view_as<Address>(goalOffset), oldGoalX, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(goalOffset + 4), oldGoalY, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(goalOffset + 8), oldGoalZ, NumberType_Int32);

    StoreAddressToAddress(pBot + view_as<Address>(g_iNav_PathLadder), oldPathLadder);
    StoreToAddress(pBot + view_as<Address>(g_iNav_IsStopping), oldIsStopping, NumberType_Int8);

    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerDur), oldRepathDuration, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerTs), oldRepathTimestamp, NumberType_Int32);

    distance = totalDist;
    return writtenAreas;
}

/**
 * 真实地面行走距离（旧版 GetNavMeshWalkDistance 原样移植）
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @return          行走距离 units（失败 -1.0）
 */
stock float BotNavWalkDistance(int client, const float target[3])
{
    float dist;
    NavMeasure_ProbePath(client, target, 0, dist);
    return dist;
}

/**
 * 临时走廊探测：采集通往目标的路径 NavArea 指针，关键导航状态无副作用
 *
 * 用于模块 11 钉走廊 danger，避免 ComputePath 篡改 Bot 当前行进路径。
 * 爬梯中返回 0（跳过探测）；spot 遭遇点等次级状态可能短暂受影响，下一拍自愈。
 * 结果存放在内部 scratch，调用后立即用 BotNavProbeAreaAt(index) 读取；
 * 多次探测之间不保留（下一次探测会覆盖）。
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @return          采集到的 NavArea 数量（0 = 失败/无路径）
 */
stock int BotNavProbePathAreas(int client, const float target[3])
{
    float dist;
    return NavMeasure_ProbePath(client, target, NAV_MEASURE_MAX_PATH_NODES, dist);
}

/**
 * 读取最近一次 BotNavProbePathAreas 采集到的 NavArea 指针
 *
 * @param index     0-based 下标
 * @return          NavArea 指针；越界返回 Address_Null
 */
stock Address BotNavProbeAreaAt(int index)
{
    if (index < 0 || index >= g_iNavProbeCount)
    {
        return Address_Null;
    }
    return g_NavProbeAreas[index];
}
