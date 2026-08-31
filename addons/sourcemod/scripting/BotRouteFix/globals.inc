#if defined _botroutefix_globals_included
 #endinput
#endif
#define _botroutefix_globals_included

//========================================================================================
// HANDLES & GLOBAL VARIABLES
//========================================================================================

bool g_bIsWin64 = false;

// Patch 1: CT Bombsite Defense Unlock (guardBombsiteChance)
Address g_pDefensePatchAddress;
int g_iOriginalDefenseBytes[6];
bool g_bDefensePatched = false;

// Patch 2: CT Anti-Defense-Rush (IsDefenseRushing)
Address g_pDefenseRushPatchAddress;
int g_iOriginalDefenseRushBytes[6];
bool g_bDefenseRushPatched = false;

// Patch 3: T C4 Carrier Instant Plant (IsTimeToPlantBomb delay removal)
Address g_pC4PlantDelayPatchAddress;
int g_iOriginalC4PlantDelayBytes[6];
bool g_bC4PlantDelayPatched = false;

// Patch 4: T Bombsite Target Lock (GetClosestZone in-memory replacement)
Address g_pPlantBombClosestZonePatchAddress;
int g_iOriginalPlantBombClosestZoneBytes[19];
bool g_bPlantBombClosestZonePatched = false;
Address g_pTheCSBotsPtr;
int g_iOffset_ZoneCount = 0;
int g_iOffset_ZoneBase = 0;
int g_iZoneStride = 0;

// Module 3: Round Danger Reset (TheNavAreas Danger Zeroing)
int g_iOffset_Danger = 0;
int g_iOffset_DangerTimestamp = 0;
int g_iPtrSize = 0;

// Module 4: 1.2s Queue Breaker & Doorway Turnaround
int g_iOffset_PathLength = 0;
int g_iOffset_IsWaitingBehindFriend = 0;
int g_iOffset_PoliteTimer = 0;
int g_iOffset_TimerDuration = 0;
int g_iOffset_TimerTimestamp = 0;
int g_iOffset_IsStopping = 0;
int g_iOffset_PathLadder = 0;
Handle g_hQueueBreakerTimer = INVALID_HANDLE;

// Module 5: T Carrier Escort (Follow & StopFollowing SDKCalls)
Handle g_hFollowSDKCall = INVALID_HANDLE;
Handle g_hStopFollowingSDKCall = INVALID_HANDLE;

// Module 6: Hunt Target Claim & Flush (HuntState::OnUpdate Post Detour)
Handle g_hHuntStateDetour = INVALID_HANDLE;
int g_iOffset_HuntArea = 0;
int g_iOffset_ClearedTimestamp = 0;

// Module 7: C4 Loose Bomb Top 3 Tactical Retriever & Shared NavMesh Walk Distance
Handle g_hComputePathSDKCall = INVALID_HANDLE;
int g_iOffset_Path = 0;
int g_iPathStride = 0;
int g_iOffset_ConnectInfoPos = 0;

enum struct CandidateT
{
	int client;
	float dist;
}

// Module 8: CT Designated Defuser & Crossfire Guard
Handle g_hHideSDKCall = INVALID_HANDLE;
int g_iOffset_Task = 0;
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
	g_iOffset_IsWaitingBehindFriend = GameConfGetOffset(gc, "CCSBot_IsWaitingBehindFriend_Offset");
	g_iOffset_PoliteTimer = GameConfGetOffset(gc, "CCSBot_PoliteTimer_Offset");
	g_iOffset_TimerDuration = GameConfGetOffset(gc, "CountdownTimer_Duration_Offset");
	g_iOffset_TimerTimestamp = GameConfGetOffset(gc, "CountdownTimer_Timestamp_Offset");
	g_iOffset_IsStopping = GameConfGetOffset(gc, "CCSBot_IsStopping_Offset");
	g_iOffset_PathLadder = GameConfGetOffset(gc, "CCSBot_PathLadder_Offset");
	g_iOffset_ConnectInfoPos = GameConfGetOffset(gc, "ConnectInfo_Vector_Offset");
	g_iOffset_Task = GameConfGetOffset(gc, "CCSBot_Task_Offset");

	g_iOffset_HuntArea = GameConfGetOffset(gc, "HuntState_HuntArea_Offset");
	g_iOffset_ClearedTimestamp = GameConfGetOffset(gc, "NavArea_ClearedTimestamp_Offset");

	if (g_iOffset_ZoneCount == -1 || g_iOffset_ZoneBase == -1 || g_iZoneStride == -1 ||
		g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1 || g_iPtrSize == -1 ||
		g_iOffset_Path == -1 || g_iPathStride == -1 || g_iOffset_PathLength == -1 ||
		g_iOffset_IsWaitingBehindFriend == -1 || g_iOffset_PoliteTimer == -1 ||
		g_iOffset_TimerDuration == -1 || g_iOffset_TimerTimestamp == -1 ||
		g_iOffset_IsStopping == -1 || g_iOffset_PathLadder == -1 || g_iOffset_ConnectInfoPos == -1 ||
		g_iOffset_Task == -1 ||
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
