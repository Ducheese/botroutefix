#if defined _botroutefix_globals_included
 #endinput
#endif
#define _botroutefix_globals_included

// 全局统一战术心跳周期 (0.5s / 2Hz，统一驱动 Timer 与缓存节流)
#define TACTICAL_TICK_INTERVAL 0.5

// ---------------------------------------------------------------------------------------
// [COMPAT] SM 1.11 本地编译兼容：SM 1.11 仅 Int8/16/32 且无 Address 封装，1.12+ 原生提供
// 线上 64 位务必用 SM 1.12+ 编译，否则高位截断；此分支仅为过 1.11 编译检查。
// ---------------------------------------------------------------------------------------
#if defined SOURCEMOD_V_MAJOR && SOURCEMOD_V_MAJOR == 1 && SOURCEMOD_V_MINOR < 12
stock Address LoadAddressFromAddress(Address addr)
{
	return view_as<Address>(LoadFromAddress(addr, NumberType_Int32));
}
stock void StoreAddressToAddress(Address addr, Address val, bool updateMemAccess = true)
{
	StoreToAddress(addr, val, NumberType_Int32, updateMemAccess);
}
#endif

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

// ---------------------------------------------------------------------------------------
// [模块 9：闪避倾向热补丁 (AttackState Dodge Hot Patch)]
// ---------------------------------------------------------------------------------------
Handle g_hAttackDodgeDetour = INVALID_HANDLE;
int g_iOffset_AttackShouldDodge = 0;

// ---------------------------------------------------------------------------------------
// [模块 10：换弹掩体 100% 强制 (Reload Hide 100% Patch)]
// ---------------------------------------------------------------------------------------
Address g_pReloadHidePatchAddress;
int g_iOriginalReloadHideBytes[2];
bool g_bReloadHidePatched = false;

// ---------------------------------------------------------------------------------------
// [模块 11：血量过低强制后撤 (Low Health PinnedDown Retreat)]
// ---------------------------------------------------------------------------------------
Handle g_hAttackRetreatDetour = INVALID_HANDLE;
int g_iOffset_PinnedDownTimestamp = 0;
int g_iOffset_MRetreatTimer = 0;

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

	g_iOffset_AttackShouldDodge = GameConfGetOffset(gc, "AttackState_ShouldDodge_Offset");
	g_iOffset_PinnedDownTimestamp = GameConfGetOffset(gc, "AttackState_PinnedDownTimestamp_Offset");
	g_iOffset_MRetreatTimer = GameConfGetOffset(gc, "AttackState_MRetreatTimer_Offset");

	if (g_iOffset_ZoneCount == -1 || g_iOffset_ZoneBase == -1 || g_iZoneStride == -1 ||
		g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1 || g_iPtrSize == -1 ||
		g_iOffset_Path == -1 || g_iPathStride == -1 || g_iOffset_PathLength == -1 ||
		g_iOffset_IsWaitingBehindFriend == -1 || g_iOffset_PoliteTimer == -1 ||
		g_iOffset_TimerDuration == -1 || g_iOffset_TimerTimestamp == -1 ||
		g_iOffset_IsStopping == -1 || g_iOffset_PathLadder == -1 || g_iOffset_ConnectInfoPos == -1 ||
		g_iOffset_Task == -1 || g_iOffset_Morale == -1 || g_iOffset_EarliestBombPlant == -1 ||
		g_iOffset_HuntArea == -1 || g_iOffset_ClearedTimestamp == -1 ||
		g_iOffset_AttackShouldDodge == -1 || g_iOffset_PinnedDownTimestamp == -1 || g_iOffset_MRetreatTimer == -1)
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

	//========================================================================
	// 1. BACKUP BOT MOVEMENT STATE (NON-DESTRUCTIVE SNAPSHOT)
	//========================================================================
	int oldPathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	int oldPathIndex = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathIndex), NumberType_Int32);
	Address oldPathLadder = LoadAddressFromAddress(pBot + view_as<Address>(g_iOffset_PathLadder));
	int oldIsStopping = LoadFromAddress(pBot + view_as<Address>(g_iOffset_IsStopping), NumberType_Int8);

	// Backup Area Pointers (m_currentArea & m_lastKnownArea)
	int currentAreaOffset = g_iOffset_Task + 24;
	int lastKnownAreaOffset = currentAreaOffset + g_iPtrSize;
	Address oldCurrentArea = LoadAddressFromAddress(pBot + view_as<Address>(currentAreaOffset));
	Address oldLastKnownArea = LoadAddressFromAddress(pBot + view_as<Address>(lastKnownAreaOffset));

	// Backup Goal Position (Vector: 3 floats)
	int goalOffset = g_iOffset_Task + 8;
	int oldGoalX = LoadFromAddress(pBot + view_as<Address>(goalOffset), NumberType_Int32);
	int oldGoalY = LoadFromAddress(pBot + view_as<Address>(goalOffset + 4), NumberType_Int32);
	int oldGoalZ = LoadFromAddress(pBot + view_as<Address>(goalOffset + 8), NumberType_Int32);

	// Backup Repath Timer (base = PathLength + 12/16, m_duration @ +TimerDuration, m_timestamp @ +TimerTimestamp)
	// IDA x64: base=16664 (PathLength+16), duration@+8, timestamp@+12; x86: base=13364 (+12), duration@+4, timestamp@+8
	int repathOffset = g_iOffset_PathLength + (g_bIsWin64 ? 16 : 12);
	int oldRepathDuration = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerDuration), NumberType_Int32);
	int oldRepathTimestamp = LoadFromAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), NumberType_Int32);

	// Temporarily invalidate repath timer so ComputePath is guaranteed not to be throttled
	StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), view_as<int>(-1.0), NumberType_Int32);

	// Backup Path Buffer (up to 256 nodes)
	int dwordsPerNode = g_iPathStride / 4;
	int clampedOldLength = oldPathLength;
	if (clampedOldLength < 0) clampedOldLength = 0;
	if (clampedOldLength > 256) clampedOldLength = 256;

	int oldPathBuffer[2048]; // Support up to 8 dwords per node (32 bytes on x64) * 256 nodes
	int totalDwords = clampedOldLength * dwordsPerNode;
	if (totalDwords > 2048) totalDwords = 2048;

	for (int d = 0; d < totalDwords; d++)
	{
		oldPathBuffer[d] = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Path + d * 4), NumberType_Int32);
	}

	//========================================================================
	// 2. EXECUTE SDKCALL MEASUREMENT
	//========================================================================
	bool hasPath = SDKCall(g_hComputePathSDKCall, client, targetPos, 0); // 0 = FASTEST_ROUTE
	float totalDist = -1.0;

	if (hasPath)
	{
		int newPathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
		if (newPathLength > 0)
		{
			totalDist = 0.0;
			float prevPos[3];
			GetClientAbsOrigin(client, prevPos);

			int vecOffset = g_iOffset_ConnectInfoPos;

			for (int i = 0; i < newPathLength; i++)
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
		}
	}

	//========================================================================
	// 3. FULL SEAMLESS STATE RESTORATION
	//========================================================================
	// Restore Path Nodes
	for (int d = 0; d < totalDwords; d++)
	{
		StoreToAddress(pBot + view_as<Address>(g_iOffset_Path + d * 4), oldPathBuffer[d], NumberType_Int32);
	}

	// Restore Path Length & Index
	StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), oldPathLength, NumberType_Int32);
	StoreToAddress(pBot + view_as<Address>(g_iOffset_PathIndex), oldPathIndex, NumberType_Int32);

	// Restore Area Pointers
	StoreAddressToAddress(pBot + view_as<Address>(currentAreaOffset), oldCurrentArea);
	StoreAddressToAddress(pBot + view_as<Address>(lastKnownAreaOffset), oldLastKnownArea);

	// Restore Goal Position
	StoreToAddress(pBot + view_as<Address>(goalOffset), oldGoalX, NumberType_Int32);
	StoreToAddress(pBot + view_as<Address>(goalOffset + 4), oldGoalY, NumberType_Int32);
	StoreToAddress(pBot + view_as<Address>(goalOffset + 8), oldGoalZ, NumberType_Int32);

	// Restore Ladder & Stopping
	StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), oldPathLadder);
	StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), oldIsStopping, NumberType_Int8);

	// Restore Repath Timer
	StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerDuration), oldRepathDuration, NumberType_Int32);
	StoreToAddress(pBot + view_as<Address>(repathOffset + g_iOffset_TimerTimestamp), oldRepathTimestamp, NumberType_Int32);

	return totalDist;
}
