#if defined _botroutefix_globals_included
 #endinput
#endif
#define _botroutefix_globals_included

// 全局统一战术心跳周期 (0.5s / 2Hz，统一驱动 Timer 与缓存节流)
#define TACTICAL_TICK_INTERVAL 0.5

//========================================================================================
// HANDLES & GLOBAL VARIABLES
//========================================================================================

bool g_bIsWin64 = false;

// ---------------------------------------------------------------------------------------
// [模块 1 & 2：底层二进制内存补丁 (Binary Memory Patches)]
// ---------------------------------------------------------------------------------------
// Patch 1: CT 守包点士气公式原地修复 (guardBombsiteChance 汇编重写为 70 - 10 * Morale)
Address g_pGuardBombsitePatchAddress;
int g_iOriginalGuardBombsiteBytes[26];
int g_iGuardBombsitePatchSize = 0;
bool g_bGuardBombsitePatched = false;

// Patch 2: CT 防前冲白给 (IsDefenseRushing NOP 穿透)
Address g_pAntiRushPatchAddress;
int g_iOriginalAntiRushBytes[6];
bool g_bAntiRushPatched = false;

// Patch 3: T C4 即刻运包 (IsTimeToPlantBomb 延迟移除)
Address g_pInstantPlantPatchAddress;
int g_iOriginalInstantPlantBytes[6];
bool g_bInstantPlantPatched = false;

// Patch 4: T 目标包点锁定与防折返跑 (GetClosestZone 汇编即时数注入)
Address g_pBombsiteLockPatchAddress;
int g_iOriginalBombsiteLockBytes[19];
bool g_bBombsiteLockPatched = false;
Address g_pTheCSBotsPtr;
int g_iOffset_ZoneCount = 0;
int g_iOffset_ZoneBase = 0;
int g_iZoneStride = 0;

// ---------------------------------------------------------------------------------------
// [模块 3：回合开局 Danger 软衰减 (TheNavAreas Danger Reset)]
// ---------------------------------------------------------------------------------------
int g_iOffset_Danger = 0;
int g_iOffset_DangerTimestamp = 0;
int g_iPtrSize = 0;

// ---------------------------------------------------------------------------------------
// [模块 4：1.2秒礼貌排队打碎与门口掉头 (Queue Breaker & Anti-Jam Turnaround)]
// ---------------------------------------------------------------------------------------
int g_iOffset_PathLength = 0;
int g_iOffset_PathIndex = 0;
int g_iOffset_IsWaitingBehindFriend = 0;
int g_iOffset_PoliteTimer = 0;
int g_iOffset_TimerDuration = 0;
int g_iOffset_TimerTimestamp = 0;
int g_iOffset_IsStopping = 0;
int g_iOffset_PathLadder = 0;
Handle g_hQueueBreakerTimer = INVALID_HANDLE;

// ---------------------------------------------------------------------------------------
// [模块 5：T 包匪贴身保镖护航体系 (Carrier Escort System)]
// ---------------------------------------------------------------------------------------
Handle g_hFollowSDKCall = INVALID_HANDLE;
Handle g_hStopFollowingSDKCall = INVALID_HANDLE;

// ---------------------------------------------------------------------------------------
// [模块 6：搜敌目标即时抢占与全图打散 (Hunt Target Claim & Flush)]
// ---------------------------------------------------------------------------------------
Handle g_hHuntStateDetour = INVALID_HANDLE;
int g_iOffset_HuntArea = 0;
int g_iOffset_ClearedTimestamp = 0;

// ---------------------------------------------------------------------------------------
// [全局公共基础设施：NavMesh A* 寻路与路径节点解析 (Shared Pathfinding Infrastructure)]
// 供模块 4 (防卡门)、模块 5 (保镖)、模块 7 (捡包)、模块 8 (拆包) 共享基础寻路
// ---------------------------------------------------------------------------------------
Handle g_hComputePathSDKCall = INVALID_HANDLE;
int g_iOffset_Path = 0;
int g_iPathStride = 0;
int g_iOffset_ConnectInfoPos = 0;

enum struct CandidateT
{
	int client;
	float dist;
}

// ---------------------------------------------------------------------------------------
// [模块 7：C4 掉落 Top 3 战术抢包突击组 (Loose Bomb Top 3 Retriever)]
// ---------------------------------------------------------------------------------------
Handle g_hNoticeLooseBombDetour = INVALID_HANDLE;
float g_fLastLooseBombCalcTime = 0.0;
int g_iTop3Retrievers[3];
int g_iTop3RetrieverCount = 0;

// ---------------------------------------------------------------------------------------
// [模块 8：CT 首席拆包员与交叉火力架枪体系 (CT Designated Defuser & Crossfire Guard)]
// ---------------------------------------------------------------------------------------
Handle g_hHideSDKCall = INVALID_HANDLE;
int g_iOffset_Task = 0;
int g_iOffset_Morale = 0;
int g_iOffset_EarliestBombPlant = 0;
int g_iOffset_HasVisitedEnemySpawn = 0;
bool g_bBombPlanted = false;
float g_fBombPlantedPos[3];
int g_iDesignatedDefuser = 0;
Handle g_hDefuseCoordTimer = INVALID_HANDLE;

//========================================================================================
// CONFIGURATION & OFFSETS LOADER
//========================================================================================

void PrepOffsets()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
	{
		SetFailState("[BotRouteFix] Failed to load gamedata file: %s.txt", GAMEDATA);
		return;
	}

	g_bIsWin64 = (GameConfGetOffset(gc, "IsWin64") == 1);

	g_iOffset_ZoneCount = GameConfGetOffset(gc, "TheCSBots_ZoneCount_Offset");
	g_iOffset_ZoneBase = GameConfGetOffset(gc, "TheCSBots_ZoneBase_Offset");
	g_iZoneStride = GameConfGetOffset(gc, "TheCSBots_ZoneStride");

	g_iOffset_Danger = GameConfGetOffset(gc, "NavArea_Danger_Offset");
	g_iOffset_DangerTimestamp = GameConfGetOffset(gc, "NavArea_DangerTimestamp_Offset");
	g_iPtrSize = GameConfGetOffset(gc, "Pointer_Size");

	g_iOffset_Path = GameConfGetOffset(gc, "CCSBot_Path_Offset");
	g_iPathStride = GameConfGetOffset(gc, "CCSBot_PathStride");
	g_iOffset_PathLength = GameConfGetOffset(gc, "CCSBot_PathLength_Offset");
	g_iOffset_PathIndex = g_iOffset_PathLength + 4; // C++ 结构体紧随 m_pathLength 之后
	g_iOffset_IsWaitingBehindFriend = GameConfGetOffset(gc, "CCSBot_IsWaitingBehindFriend_Offset");
	g_iOffset_PoliteTimer = GameConfGetOffset(gc, "CCSBot_PoliteTimer_Offset");
	g_iOffset_TimerDuration = GameConfGetOffset(gc, "CountdownTimer_Duration_Offset");
	g_iOffset_TimerTimestamp = GameConfGetOffset(gc, "CountdownTimer_Timestamp_Offset");
	g_iOffset_IsStopping = GameConfGetOffset(gc, "CCSBot_IsStopping_Offset");
	g_iOffset_HasVisitedEnemySpawn = g_iOffset_IsStopping + 1; // C++ 结构体紧随 m_isStopping 之后 (1 字节 bool)
	g_iOffset_PathLadder = GameConfGetOffset(gc, "CCSBot_PathLadder_Offset");
	g_iOffset_ConnectInfoPos = GameConfGetOffset(gc, "ConnectInfo_Vector_Offset");
	g_iOffset_Task = GameConfGetOffset(gc, "CCSBot_Task_Offset");
	g_iOffset_Morale = GameConfGetOffset(gc, "CCSBot_Morale_Offset");
	g_iOffset_EarliestBombPlant = GameConfGetOffset(gc, "TheCSBots_EarliestBombPlant_Offset");

	g_iOffset_HuntArea = GameConfGetOffset(gc, "HuntState_HuntArea_Offset");
	g_iOffset_ClearedTimestamp = GameConfGetOffset(gc, "NavArea_ClearedTimestamp_Offset");

	if (g_iOffset_ZoneCount == -1 || g_iOffset_ZoneBase == -1 || g_iZoneStride == -1 ||
		g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1 || g_iPtrSize == -1 ||
		g_iOffset_Path == -1 || g_iPathStride == -1 || g_iOffset_PathLength == -1 ||
		g_iOffset_IsWaitingBehindFriend == -1 || g_iOffset_PoliteTimer == -1 ||
		g_iOffset_TimerDuration == -1 || g_iOffset_TimerTimestamp == -1 ||
		g_iOffset_IsStopping == -1 || g_iOffset_PathLadder == -1 || g_iOffset_ConnectInfoPos == -1 ||
		g_iOffset_Task == -1 || g_iOffset_Morale == -1 || g_iOffset_EarliestBombPlant == -1 ||
		g_iOffset_HuntArea == -1 || g_iOffset_ClearedTimestamp == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read one or more offsets from gamedata!");
		return;
	}

	// Shared SDKCall: CCSBot::ComputePath (to calculate true ground path distance)
	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_ComputePath");
	PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef);
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain); // RouteType (0 = FASTEST_ROUTE)
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	g_hComputePathSDKCall = EndPrepSDKCall();

	delete gc;
}

float GetNavMeshWalkDistance(int client, const float targetPos[3])
{
	if (g_hComputePathSDKCall == null)
		return -1.0;

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null)
		return -1.0;

	// Compute fastest path to the target position (invokes NavAreaBuildPath under the hood)
	bool hasPath = SDKCall(g_hComputePathSDKCall, client, targetPos, 0); // 0 = FASTEST_ROUTE
	if (!hasPath)
		return -1.0;

	int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	if (pathLength <= 0)
		return -1.0;

	float totalDist = 0.0;
	float prevPos[3];
	GetClientAbsOrigin(client, prevPos);

	int vecOffset = g_iOffset_ConnectInfoPos;

	for (int i = 0; i < pathLength; i++)
	{
		Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
		float nodePos[3];
		nodePos[0] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset), NumberType_Int32));
		nodePos[1] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset + 4), NumberType_Int32));
		nodePos[2] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset + 8), NumberType_Int32));

		totalDist += GetVectorDistance(prevPos, nodePos);
		prevPos = nodePos;
	}

	totalDist += GetVectorDistance(prevPos, targetPos);
	return totalDist;
}
