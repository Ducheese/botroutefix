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
//   g_iPtrSize                                         → TheNavAreas 容器读取、M8 排队（缺失回退 g_bIsWin64 ? 8 : 4）
//   g_iOffset_Task 由 global.inc 提供（OnPluginStart 解析），此处只用不存。
//
// 本文件的第三职责：TheNavAreas 全局导航网格容器（第 0 层无票基建，与测距就绪解耦）：
//   InitNavMeasure() 首装解析 + NavAreas_OnMapStart() 换图无条件重解；
//   M3/M4/M8/M11 统一消费 NavAreas_IsReady / NavAreas_GetCount / NavAreas_GetAt / NavAreas_IsKnown；
//   NavAreas_IsKnown 未命中时一律静默返回 false。
//
// 待定：M8 的 Path/PathLength/PathStride/PathLadder/IsStopping 与本文件同 key 私有并存
// （变量名不同：g_iOffset_* vs g_iNav_*），收敛与否等拍板后再动。
//========================================================================================

#define NAV_MEASURE_MAX_PATH_NODES  256     // 与 cs_bot.h 的 MAX_PATH_LENGTH 对齐
#define NAV_AREA_MAX_SANITY         100000  // 导航网格数量合理性上限
#define NAV_AREA_PAIR_SCAN_BYTES    0x700   // 32 位锚点后的候选指令扫描窗口
#define NAV_AREA_PAIR_MAX_DELTA     0x800000 // 候选 base 与锚点的最大距离

// 路由类型定义（与引擎 enum RouteType 严格对齐）
#define ROUTE_DEFAULT   0   // 原生默认路由（不带危险度惩罚）
#define ROUTE_FASTEST   1   // 最快路线（忽略阻碍与小跌落痛感）
#define ROUTE_SAFEST    2   // 最安全路线（带 NavArea 动态危险度与侵略度加权惩罚）
#define ROUTE_RETREAT   3   // 撤退路线

// 探针输出 scratch（单线程使用，调用后立即消费；避免 Address[] 动态数组参数的写回语义问题）
// 【编译器缺陷补偿】同 M11：全局 Address 数组 4B/元分配、8B 步进，故声明双倍长度。
static Address g_NavProbeAreas[NAV_MEASURE_MAX_PATH_NODES * 2];
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

// C 组：TheNavAreas 全局导航容器单例指针（第 0 层私有封装，外部走 NavAreas_* 接口）
static Address g_pTheNavAreasCount = Address_Null;
static Address g_pTheNavAreasBase  = Address_Null;

/**
 * 加载无损测距所需偏移 + 解析跨模块共享偏移与 TheNavAreas 容器
 * （软加载：测距偏移缺失只让测距待机；TheNavAreas 独立维护 NavAreas_IsReady，不阻塞测距或受测距阻塞）
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

    // C 组：TheNavAreas 全局导航容器（独立就绪态 NavAreas_IsReady，不进下方测距待机门）
    ResolveTheNavAreas();

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
 * @param route     路由类型 (ROUTE_DEFAULT=0, ROUTE_FASTEST=1, ROUTE_SAFEST=2, ROUTE_RETREAT=3)
 * @return          实际采集的 NavArea 数量（0 = 失败/无路径/不采集）
 */
static int NavMeasure_ProbePath(int client, const float target[3], int maxAreas, float &distance, int route = ROUTE_DEFAULT)
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
    bool hasPath = SDKCall(g_hSDKCall_ComputePath, client, target, route);
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
 * 真实地面行走距离（可指定路由策略，默认 ROUTE_DEFAULT 原生路由）
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @param route     路由类型 (ROUTE_DEFAULT=0, ROUTE_FASTEST=1, ROUTE_SAFEST=2, ROUTE_RETREAT=3)
 * @return          行走距离 units（失败 -1.0）
 */
stock float BotNavWalkDistance(int client, const float target[3], int route = ROUTE_DEFAULT)
{
    float dist;
    NavMeasure_ProbePath(client, target, 0, dist, route);
    return dist;
}

/**
 * 临时走廊探测：采集通往目标的路径 NavArea 指针，关键导航状态无副作用
 *
 * 用于模块 11 钉走廊 danger，避免 ComputePath 篡改 Bot 当前行进路径。
 * 默认使用 ROUTE_FASTEST：无视危险度惩罚直指对峙走廊，防止走廊已有危险度时绕道侧翼误伤其他路网。
 * 爬梯中返回 0（跳过探测）；spot 遭遇点等次级状态可能短暂受影响，下一拍自愈。
 * 结果存放在内部 scratch，调用后立即用 BotNavProbeAreaAt(index) 读取；
 * 多次探测之间不保留（下一次探测会覆盖）。
 *
 * @param client    Bot 客户端索引 (必须为存活有效 Bot)
 * @param target    目标世界坐标
 * @param route     路由类型 (默认 ROUTE_FASTEST 保证直击走廊，绝不绕行)
 * @return          采集到的 NavArea 数量（0 = 失败/无路径）
 */
stock int BotNavProbePathAreas(int client, const float target[3], int route = ROUTE_FASTEST)
{
    float dist;
    return NavMeasure_ProbePath(client, target, NAV_MEASURE_MAX_PATH_NODES, dist, route);
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

//========================================================================================
// THE NAV AREAS CONTAINER INFRA（第 0 层无票引擎单例；M3/M4/M8/M11 共用，与测距就绪解耦）
//========================================================================================

/**
 * 解析全局 TheNavAreas 容器（CUtlVector<CNavArea*> 的 Base/Count 全局地址）
 *
 * 64 位：从 CCSBotManager::GetRandomArea 的 mov ecx, cs:TheNavAreas.m_Size 定位。
 * 32 位：从 HuntState::OnUpdate 的 NavArray 对特征扫描定位。
 * 失败语义：先用局部候选地址校验 count 合理性，失败不写入全局变量，保证 NavAreas_IsReady() 诚实。
 *
 * @return              true = 容器 Base/Count 均已验证有效
 */
static bool ResolveTheNavAreas()
{
    if (g_pTheNavAreasCount != Address_Null && g_pTheNavAreasBase != Address_Null)
    {
        return true;
    }

    if (g_hGameConf == null)
    {
        return false;
    }

    if (g_bIsWin64)
    {
        // -------------------------------------------------------------------------------
        // 64-bit 实现 (Steam x64)
        // 通过 CCSBotManager::GetRandomArea 的 64 位特征指令定位 TheNavAreas
        // 指令序列: mov ecx, cs:TheNavAreas.m_Size (8B 0D <rel32> @ sigAddr + 14)
        // -------------------------------------------------------------------------------
        Address sigAddr = GameConfGetAddress(g_hGameConf, "CCSBotManager_GetRandomArea");
        if (sigAddr == Address_Null)
        {
            LogError("[BotRouteFix] [NavMeasure] Failed to locate signature for CCSBotManager_GetRandomArea!");
            return false;
        }

        int relOffset = LoadFromAddress(sigAddr + view_as<Address>(16), NumberType_Int32);
        Address candCount = sigAddr + view_as<Address>(20 + relOffset);
        // CUtlVector 内存布局: m_pMemory 位于 m_Size 前面 16 字节
        Address candBase = candCount - view_as<Address>(16);

        int areaCount = LoadFromAddress(candCount, NumberType_Int32);
        if (areaCount < 0 || areaCount > NAV_AREA_MAX_SANITY)
        {
            LogError("[BotRouteFix] [NavMeasure] 64-bit areaCount (%d) out of reasonable range [0, %d]!",
                areaCount, NAV_AREA_MAX_SANITY);
            return false;
        }

        g_pTheNavAreasCount = candCount;
        g_pTheNavAreasBase  = candBase;

        LogMessage("[BotRouteFix] [NavMeasure] Resolved 64-bit TheNavAreas (Base: @%X, Count: @%X, Areas: %d)",
            g_pTheNavAreasBase, g_pTheNavAreasCount, areaCount);
        return true;
    }

    // -------------------------------------------------------------------------------
    // 32-bit 实现 (v91/v92)
    // 通过 HuntState::OnUpdate 内部稳定的 NavArray 对特征扫描定位:
    //   mov ecx, [TheNavAreasBase] / mov ecx, [ecx + edx*4] (8B 0D <Base> 8B 0C 91)
    // 该特征在 CS:S 32 位二进制中全局唯一，Count = Base + 12
    // -------------------------------------------------------------------------------
    Address huntAddr = GameConfGetAddress(g_hGameConf, "HuntState_NavArrayAnchor");
    if (huntAddr == Address_Null)
    {
        LogError("[BotRouteFix] [NavMeasure] Failed to locate signature for HuntState_NavArrayAnchor!");
        return false;
    }

    LogMessage("[BotRouteFix] [NavMeasure] Hunt anchor @ %X, scanning nav-array pairs...", huntAddr);

    Address pairAddr = Address_Null;
    int seenPairs = 0;

    // 逐个验候选 pair：base 先过邻近性检查（相对 anchor ±8MB，防野地址读崩服；
    // 不写死 0x10000000 基址——某些加载器会把 server.dll 重定基到 0x65 等他址），
    // 再验 count 是否 sane；首个全过即用；全灭则报错供诊断。
    for (int off = 0; off < NAV_AREA_PAIR_SCAN_BYTES; off++)
    {
        if (LoadFromAddress(huntAddr + view_as<Address>(off), NumberType_Int8) == 0x8B
            && LoadFromAddress(huntAddr + view_as<Address>(off + 1), NumberType_Int8) == 0x0D
            && LoadFromAddress(huntAddr + view_as<Address>(off + 6), NumberType_Int8) == 0x8B
            && LoadFromAddress(huntAddr + view_as<Address>(off + 7), NumberType_Int8) == 0x0C
            && LoadFromAddress(huntAddr + view_as<Address>(off + 8), NumberType_Int8) == 0x91)
        {
            seenPairs++;
            Address candAddr = huntAddr + view_as<Address>(off);
            int candBase = LoadFromAddress(candAddr + view_as<Address>(2), NumberType_Int32);

            int baseDelta = candBase - view_as<int>(huntAddr);
            if (baseDelta < 0)
            {
                baseDelta = -baseDelta;
            }
            if (baseDelta > NAV_AREA_PAIR_MAX_DELTA)
            {
                LogMessage("[BotRouteFix] [NavMeasure] Candidate pair @ %X skipped (base %X too far from anchor)",
                    candAddr, candBase);
                continue;
            }

            int candCount = LoadFromAddress(view_as<Address>(candBase + 12), NumberType_Int32);
            if (candCount < 0 || candCount > NAV_AREA_MAX_SANITY)
            {
                LogMessage("[BotRouteFix] [NavMeasure] Candidate pair @ %X skipped (count %d insane)",
                    candAddr, candCount);
                continue;
            }

            pairAddr = candAddr;
            break;
        }
    }

    if (pairAddr == Address_Null)
    {
        if (seenPairs == 0)
        {
            LogError("[BotRouteFix] [NavMeasure] Nav-array pair pattern not found in HuntState::OnUpdate!");
        }
        else
        {
            LogError("[BotRouteFix] [NavMeasure] %d nav-array pair(s) seen from anchor @ %X but none validated!",
                seenPairs, huntAddr);
        }
        return false;
    }

    int baseAddr = LoadFromAddress(pairAddr + view_as<Address>(2), NumberType_Int32);
    Address candBase  = view_as<Address>(baseAddr);
    Address candCount = view_as<Address>(baseAddr + 12);

    int areaCount = LoadFromAddress(candCount, NumberType_Int32);
    if (areaCount < 0 || areaCount > NAV_AREA_MAX_SANITY)
    {
        LogError("[BotRouteFix] [NavMeasure] Nav-area count is unreasonable (%d)!", areaCount);
        return false;
    }

    g_pTheNavAreasBase  = candBase;
    g_pTheNavAreasCount = candCount;

    LogMessage("[BotRouteFix] [NavMeasure] Resolved 32-bit TheNavAreas (Base: @%X, Count: @%X, Areas: %d)",
        g_pTheNavAreasBase, g_pTheNavAreasCount, areaCount);
    return true;
}

/**
 * 内部原语：读取并校验当前 TheNavAreas 数组基址与非空计数
 *
 * @param count         输出有效区域数量（> 0 且 <= NAV_AREA_MAX_SANITY）；不可用时置 0
 * @return              m_pMemory 基址；未解析/空容器/count 越界/数组空时返回 Address_Null
 */
static Address NavAreas_GetArray(int &count)
{
    count = 0;
    if (g_pTheNavAreasCount == Address_Null || g_pTheNavAreasBase == Address_Null)
    {
        return Address_Null;
    }

    int n = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
    if (n <= 0 || n > NAV_AREA_MAX_SANITY)
    {
        return Address_Null;
    }

    Address pArray = LoadAddressFromAddress(g_pTheNavAreasBase);
    if (pArray == Address_Null)
    {
        return Address_Null;
    }

    count = n;
    return pArray;
}

/**
 * 地图加载 (OnMapStart)：nav 网格重建，必须无条件清空并重解 TheNavAreas 容器
 *
 * 【第 0 层无票基建，不得受任何业务开关守卫】
 *
 * @return              true = 容器重解且计数在合法范围
 */
bool NavAreas_OnMapStart()
{
    g_pTheNavAreasCount = Address_Null;
    g_pTheNavAreasBase  = Address_Null;
    return ResolveTheNavAreas();
}

/**
 * 查询 TheNavAreas 全局容器地址是否已解析就绪（与测距 BotNavReady 独立）
 */
stock bool NavAreas_IsReady()
{
    return (g_pTheNavAreasCount != Address_Null && g_pTheNavAreasBase != Address_Null);
}

/**
 * 查询当前 TheNavAreas 中的已知区域总数
 *
 * @return              区域总数；未解析/异常或为空时返回 0
 */
stock int NavAreas_GetCount()
{
    int count;
    NavAreas_GetArray(count);
    return count;
}

/**
 * 按下标读取 TheNavAreas 容器中的 CNavArea 指针（外部遍历不再直接解引用裸数组）
 *
 * @param index         0-based 下标
 * @return              CNavArea 指针；容器未就绪或越界返回 Address_Null
 */
stock Address NavAreas_GetAt(int index)
{
    int count;
    Address pArray = NavAreas_GetArray(count);
    if (pArray == Address_Null || index < 0 || index >= count)
    {
        return Address_Null;
    }
    return LoadAddressFromAddress(pArray + view_as<Address>(index * g_iPtrSize));
}

/**
 * TheNavAreas 容器成员真值校验（M4 认领 / M8 排队钉刺 / M11 走廊钉刺写前门禁共用）
 *
 * 探针尾槽或结构字段里的陈旧野值（非 NULL/-1，哨兵拦不住）只有逐一比对容器才能认出；
 * 容器未解析、计数非法或未命中时静默返回 false（零日志、零计数器）。
 *
 * @param pArea         待校验的 CNavArea 指针
 * @return              true = 容器内已知区域
 */
stock bool NavAreas_IsKnown(Address pArea)
{
    if (!IsValidAddress(pArea))
    {
        return false;
    }

    int count;
    Address pArray = NavAreas_GetArray(count);
    if (pArray == Address_Null)
    {
        return false;
    }

    for (int i = 0; i < count; i++)
    {
        if (LoadAddressFromAddress(pArray + view_as<Address>(i * g_iPtrSize)) == pArea)
        {
            return true;
        }
    }
    return false;
}
