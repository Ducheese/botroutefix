#if defined _botroutefix_nav_measure_included
    #endinput
#endif
#define _botroutefix_nav_measure_included

//========================================================================================
// NAV MEASURE SUBSYSTEM
//
// 真实地面行走距离（旧版 GetNavMeshWalkDistance 原样移植）：
// 备份移动状态 → 解 repath 节流 → ComputePath 实测 → 逐段累加节点间距 → 全量还原。
// 调用方视角零副作用；寻路失败返回 -1.0。
//
// 不是 SDKCall 包裹，是独立测量子系统（借用 ComputePath 句柄做实测引擎）。
// 软加载：偏移缺失只记日志，测距整体待机，不杀插件。
//========================================================================================

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

    if (g_hSDKCall_ComputePath == null || g_iNav_Path == -1 || g_iNav_PathStride == -1 ||
        g_iNav_PathLength == -1 || g_iNav_PathLadder == -1 || g_iNav_IsStopping == -1 ||
        g_iNav_Task == -1 || g_iNav_ConnectPos == -1 || g_iNav_TimerDur == -1 ||
        g_iNav_TimerTs == -1 || g_iNav_PtrSize == -1)
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
 * 真实地面行走距离（旧版 GetNavMeshWalkDistance 原样移植）
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @return          行走距离 units（失败 -1.0）
 */
stock float BotNavWalkDistance(int client, const float target[3])
{
    if (!g_bNavMeasureReady || !IsBot(client, true))
    {
        return -1.0;
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
    {
        return -1.0;
    }

    // 1. 备份移动状态（非破坏性快照）
    int pathLengthOffset = g_iNav_PathLength;
    int pathIndexOffset = g_iNav_PathLength + 4; // C++ 结构体紧随 m_pathLength 之后
    int oldPathLength = LoadFromAddress(pBot + view_as<Address>(pathLengthOffset), NumberType_Int32);
    int oldPathIndex = LoadFromAddress(pBot + view_as<Address>(pathIndexOffset), NumberType_Int32);
    Address oldPathLadder = LoadAddressFromAddress(pBot + view_as<Address>(g_iNav_PathLadder));
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

    // 2. 实测
    bool hasPath = SDKCall(g_hSDKCall_ComputePath, client, target, 0);
    float totalDist = -1.0;

    if (hasPath)
    {
        int newPathLength = LoadFromAddress(pBot + view_as<Address>(pathLengthOffset), NumberType_Int32);
        if (newPathLength > 0)
        {
            totalDist = 0.0;
            float prevPos[3];
            GetClientAbsOrigin(client, prevPos);

            for (int i = 0; i < newPathLength; i++)
            {
                Address pConnectInfo = pBot + view_as<Address>(g_iNav_Path + i * g_iNav_PathStride);
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

    // 3. 全量还原
    for (int d = 0; d < totalDwords; d++)
    {
        StoreToAddress(pBot + view_as<Address>(g_iNav_Path + d * 4), oldPathBuffer[d], NumberType_Int32);
    }

    StoreToAddress(pBot + view_as<Address>(pathLengthOffset), oldPathLength, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(pathIndexOffset), oldPathIndex, NumberType_Int32);

    StoreAddressToAddress(pBot + view_as<Address>(currentAreaOffset), oldCurrentArea);
    StoreAddressToAddress(pBot + view_as<Address>(lastKnownAreaOffset), oldLastKnownArea);

    StoreToAddress(pBot + view_as<Address>(goalOffset), oldGoalX, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(goalOffset + 4), oldGoalY, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(goalOffset + 8), oldGoalZ, NumberType_Int32);

    StoreAddressToAddress(pBot + view_as<Address>(g_iNav_PathLadder), oldPathLadder);
    StoreToAddress(pBot + view_as<Address>(g_iNav_IsStopping), oldIsStopping, NumberType_Int8);

    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerDur), oldRepathDuration, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iNav_TimerTs), oldRepathTimestamp, NumberType_Int32);

    return totalDist;
}
