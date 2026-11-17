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
//
// 本文件的第二职责：导航/计时器级共享偏移的唯一家（InitNavMeasure 一次解析，
// 事项 Prep 只做 > 0 门禁，不再各自私下 GameConfGetOffset）：
//   g_iOffset_TimerDuration / g_iOffset_TimerTimestamp → M5 后撤计时器、M8 排队 polite 计时器
//   g_iOffset_Danger / g_iOffset_DangerTimestamp       → M3 软衰减、M8 排队钉刺、M11 走廊钉刺
//   g_iPtrSize                                         → M3 容器解析、M8 排队（缺失回退 g_bIsWin64 ? 8 : 4）
//   g_iOffset_Task 由 global.inc 提供（OnPluginStart 解析），此处只用不存。
// 待定：M8 的 Path/PathLength/PathStride/PathLadder/IsStopping 与本文件同 key 私有并存
// （变量名不同：g_iOffset_* vs g_iNav_*），收敛与否等拍板后再动。
//========================================================================================

#define NAV_MEASURE_MAX_PATH_NODES  256     // 与 cs_bot.h 的 MAX_PATH_LENGTH 对齐

// 探针输出 scratch（单线程使用，调用后立即消费；避免 Address[] 动态数组参数的写回语义问题）
static Address g_NavProbeAreas[NAV_MEASURE_MAX_PATH_NODES];
static int g_iNavProbeCount = 0;

//========================================================================================
// OFFSET GROUPS（命名公约：g_iNav_* = 本文件测距私用；g_iOffset_* = 跨模块共享）
//========================================================================================

// A 组：测距本体私用（快照/还原路径状态；只在本文件读写）
static int g_iNav_Path         = -1;
static int g_iNav_PathStride   = -1;
static int g_iNav_PathLength   = -1;
static int g_iNav_PathLadder   = -1;
static int g_iNav_IsStopping   = -1;
static int g_iNav_ConnectPos   = -1;
static bool g_bNavMeasureReady = false;

// B 组：跨模块共享（非 static；解析点唯一在 InitNavMeasure，事项 Prep 只做 > 0 门禁）
int g_iOffset_TimerDuration   = -1;
int g_iOffset_TimerTimestamp  = -1;
int g_iOffset_Danger          = -1;
int g_iOffset_DangerTimestamp = -1;
int g_iPtrSize                = 4;

/**
 * 加载无损测距所需偏移 + 一次解析跨模块共享偏移（软加载：缺失只记日志，测距整体待机）
 */
bool InitNavMeasure()
{
    if (g_hGameConf == null)
    {
        return false;
    }

    // A 组：测距本体（进待机门，缺一不可）
    g_iNav_Path       = GameConfGetOffset(g_hGameConf, "CCSBot_Path_Offset");
    g_iNav_PathStride = GameConfGetOffset(g_hGameConf, "CCSBot_PathStride");
    g_iNav_PathLength = GameConfGetOffset(g_hGameConf, "CCSBot_PathLength_Offset");
    g_iNav_PathLadder = GameConfGetOffset(g_hGameConf, "CCSBot_PathLadder_Offset");
    g_iNav_IsStopping = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");
    g_iNav_ConnectPos = GameConfGetOffset(g_hGameConf, "ConnectInfo_Vector_Offset");

    // B 组：跨模块共享（只解析；Danger 不进待机门——测距本体用不到它，缺失由各事项 Prep 自查）
    g_iOffset_TimerDuration   = GameConfGetOffset(g_hGameConf, "CountdownTimer_Duration_Offset");
    g_iOffset_TimerTimestamp  = GameConfGetOffset(g_hGameConf, "CountdownTimer_Timestamp_Offset");
    g_iOffset_Danger          = GameConfGetOffset(g_hGameConf, "NavArea_Danger_Offset");
    g_iOffset_DangerTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_DangerTimestamp_Offset");
    g_iPtrSize                = GameConfGetOffset(g_hGameConf, "Pointer_Size");
    if (g_iPtrSize <= 0)
    {
        g_iPtrSize = g_bIsWin64 ? 8 : 4;
    }

    if (g_hSDKCall_ComputePath == null || g_iNav_Path <= 0 || g_iNav_PathStride <= 0 ||
        g_iNav_PathLength <= 0 || g_iNav_PathLadder <= 0 || g_iNav_IsStopping <= 0 ||
        g_iOffset_Task <= 0 || g_iNav_ConnectPos <= 0 || g_iOffset_TimerDuration <= 0 ||
        g_iOffset_TimerTimestamp <= 0 || g_iPtrSize <= 0)
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

//========================================================================================
// PROBE CORE（内部实现；外部统一走下面的 PUBLIC API，不得直调）
//========================================================================================

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

    int currentAreaOffset = g_iOffset_Task + 24;
    int lastKnownAreaOffset = currentAreaOffset + g_iPtrSize;
    Address oldCurrentArea = LoadAddressFromAddress(pBot + view_as<Address>(currentAreaOffset));
    Address oldLastKnownArea = LoadAddressFromAddress(pBot + view_as<Address>(lastKnownAreaOffset));

    int goalOffset = g_iOffset_Task + 8;
    int oldGoalX = LoadFromAddress(pBot + view_as<Address>(goalOffset), NumberType_Int32);
    int oldGoalY = LoadFromAddress(pBot + view_as<Address>(goalOffset + 4), NumberType_Int32);
    int oldGoalZ = LoadFromAddress(pBot + view_as<Address>(goalOffset + 8), NumberType_Int32);

    int repathOffset = g_iNav_PathLength + (g_bIsWin64 ? 16 : 12);
    int oldRepathDuration = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerDuration), NumberType_Int32);
    int oldRepathTimestamp = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), NumberType_Int32);

    // 解 repath 节流，保证本次实测不被 0.4 秒节流挡掉
    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), view_as<int>(-1.0), NumberType_Int32);

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
                    Address pNodeArea = LoadAddressFromAddress(pConnectInfo);
                    if (IsValidAddress(pNodeArea))
                    {
                        g_NavProbeAreas[writtenAreas] = pNodeArea;
                        writtenAreas++;
                    }
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

    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerDuration), oldRepathDuration, NumberType_Int32);
    StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), oldRepathTimestamp, NumberType_Int32);

    // 探针 scratch 的游标必须与本次实际采集数同步：BotNavProbeAreaAt 用它判越界，
    // 不赋值会让所有读者永远拿到 Address_Null（走廊钉刺会静默空转）。
    g_iNavProbeCount = writtenAreas;

    distance = totalDist;
    return writtenAreas;
}

//========================================================================================
// PUBLIC API（跨模块调用点：BotNavReady 门禁 + 测距/走廊探测 + 采集读取）
//========================================================================================

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
