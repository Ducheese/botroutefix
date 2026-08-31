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

// Module 3: Round Danger Reset (TheNavAreas Danger Zeroing)
int g_iOffset_Danger = 0;
int g_iOffset_DangerTimestamp = 0;

// Module 4: 1.2s Queue Breaker & Doorway Turnaround
int g_iOffset_PathLength = 0;
int g_iOffset_IsWaitingBehindFriend = 0;
int g_iOffset_PoliteTimer = 0;
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

// Module 7: Single Retriever & Tactical Cover (NoticeLooseBomb Post Detour)
Handle g_hNoticeLooseBombDetour = INVALID_HANDLE;

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

	g_iOffset_Danger = GameConfGetOffset(gc, "NavArea_Danger_Offset");
	g_iOffset_DangerTimestamp = GameConfGetOffset(gc, "NavArea_DangerTimestamp_Offset");

	g_iOffset_PathLength = GameConfGetOffset(gc, "CCSBot_PathLength_Offset");
	g_iOffset_IsWaitingBehindFriend = GameConfGetOffset(gc, "CCSBot_IsWaitingBehindFriend_Offset");
	g_iOffset_PoliteTimer = GameConfGetOffset(gc, "CCSBot_PoliteTimer_Offset");
	g_iOffset_IsStopping = GameConfGetOffset(gc, "CCSBot_IsStopping_Offset");
	g_iOffset_PathLadder = GameConfGetOffset(gc, "CCSBot_PathLadder_Offset");
	g_iOffset_Task = GameConfGetOffset(gc, "CCSBot_Task_Offset");

	g_iOffset_HuntArea = GameConfGetOffset(gc, "HuntState_HuntArea_Offset");
	g_iOffset_ClearedTimestamp = GameConfGetOffset(gc, "NavArea_ClearedTimestamp_Offset");

	if (g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1 ||
		g_iOffset_PathLength == -1 ||
		g_iOffset_IsWaitingBehindFriend == -1 || g_iOffset_PoliteTimer == -1 ||
		g_iOffset_IsStopping == -1 || g_iOffset_PathLadder == -1 ||
		g_iOffset_Task == -1 ||
		g_iOffset_HuntArea == -1 || g_iOffset_ClearedTimestamp == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read one or more offsets from gamedata!");
		return;
	}

	delete gc;
}
