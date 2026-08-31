//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点与防发疯前冲修复 (CT Bombsite Defense & Anti-Rush Fix)]
//   1. 对 guardBombsiteChance 跳转打入 6 字节 NOP，激活官方原生 CT 守包点体系；
//   2. 对 IsDefenseRushing 跳转打入 6 字节 NOP，彻底消除官方 33.3% 的全员前冲白给局。
//
// [模块 2：T 包匪开局即刻运包与 50% 随机包点修复 (T C4 Carrier Instant Plant & 50/50 Bombsite Fix)]
//   3. 对 IsTimeToPlantBomb 开局 10~30 秒强制延迟跳转打入 6 字节 NOP，消除包匪开局
//      不运包反而带 C4 冲 CT 家送人头的致命缺陷，开局即刻前往包点；
//   4. 对 GetClosestZone 调用打入补丁重定向为 GetRandomZone，打破死板的“永远去最近包点”
//      机制，实现开局在 A 包点与 B 包点之间以 50%/50% 真实随机轮换进攻。
//
// [模块 3：全动态走廊 Danger 预约与多路分流 (Dynamic Corridor Reservation & Route Splitting)]
//   5. Detour CCSBot::ComputePath Post 阶段，在刚规划好路线的前沿 5 个 NavArea 节点
//      动态注入 +0.8f 的 m_danger 占用阻力，驱使后续队友的 A* 算法自动分支转向副道/侧翼，
//      全图自然实现两翼包抄、兵分多路，彻底消除单一路线开火车拥堵。
//
// 支持架构：
//   - 32-bit: Windows non-Steam (v91/v92) server.dll
//   - 64-bit: Windows Steam x64 server.dll
//========================================================================================

#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>
#include <dhooks>

#define GAMEDATA "botroutefix.gamedata"
#define PLUGIN_VERSION "0.5.0"

//========================================================================================
// HANDLES & VARIABLES
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

// Patch 4: T C4 Carrier Bombsite Randomization (GetClosestZone -> GetRandomZone)
Address g_pC4RandomZonePatchAddress;
int g_iC4RandomZonePatchLength = 0;
int g_iOriginalC4RandomZoneBytes[24];
bool g_bC4RandomZonePatched = false;

// Module 3: Dynamic Corridor Danger Reservation (CCSBot::ComputePath Post Detour)
Handle g_hComputePathDetour = INVALID_HANDLE;
int g_iOffset_Path = 0;
int g_iPathStride = 0;
int g_iOffset_PathLength = 0;
int g_iOffset_Danger = 0;
int g_iOffset_DangerTimestamp = 0;

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复 (CT 守包/防冲 + T 运包/50%选点 + 全动态分流)",
	version     = PLUGIN_VERSION,
	url         = "https://github.com/Ducheese"
};

//========================================================================================
// LIFECYCLE
//========================================================================================

public void OnPluginStart()
{
	LogMessage("[BotRouteFix] ========== Initializing BotRouteFix (v%s) ==========", PLUGIN_VERSION);

	PrepOffsets();
	PrepDefensePatch();
	PrepDefenseRushPatch();
	PrepC4PlantDelayPatch();
	PrepC4RandomZonePatch();
	PrepComputePathHook();

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	RestoreDefensePatch();
	RestoreDefenseRushPatch();
	RestoreC4PlantDelayPatch();
	RestoreC4RandomZonePatch();
	RestoreComputePathHook();
}

//========================================================================================
// CONFIGURATION & OFFSETS
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

	g_iOffset_Path = GameConfGetOffset(gc, "CCSBot_Path_Offset");
	g_iPathStride = GameConfGetOffset(gc, "CCSBot_PathStride");
	g_iOffset_PathLength = GameConfGetOffset(gc, "CCSBot_PathLength_Offset");
	g_iOffset_Danger = GameConfGetOffset(gc, "NavArea_Danger_Offset");
	g_iOffset_DangerTimestamp = GameConfGetOffset(gc, "NavArea_DangerTimestamp_Offset");

	if (g_iOffset_Path == -1 || g_iPathStride == -1 || g_iOffset_PathLength == -1 ||
		g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read one or more offsets for Corridor Reservation from gamedata!");
		return;
	}

	delete gc;
}

//========================================================================================
// PATCH 1: CT BOMBSITE DEFENSE (guardBombsiteChance)
//========================================================================================

void PrepDefensePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_GuardBombsiteChance");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_GuardBombsiteChance!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "GuardBombsite_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read GuardBombsite_PatchOffset!");
		return;
	}

	g_pDefensePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pDefensePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 1] Already patched (NOPs present @ %X), skipping", g_pDefensePatchAddress);
		g_bDefensePatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (32-bit: 0x0F 0x86, 64-bit: 0x0F 0x83)
	if (g_bIsWin64)
	{
		if (firstByte != 0x0F || secondByte != 0x83)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 0F 83, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x86)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 0F 86, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseBytes[i] = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pDefensePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bDefensePatched = true;
	LogMessage("[BotRouteFix] [Patch 1] Replaced guardBombsite jump with 6x NOPs @ %X (CT Defense Unlocked)", g_pDefensePatchAddress);

	delete gc;
}

void RestoreDefensePatch()
{
	if (g_bDefensePatched && g_pDefensePatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pDefensePatchAddress + view_as<Address>(i), g_iOriginalDefenseBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 1] Restored original guardBombsite jump @ %X", g_pDefensePatchAddress);
		g_bDefensePatched = false;
	}
}

//========================================================================================
// PATCH 2: CT ANTI-DEFENSE-RUSH (IsDefenseRushing)
//========================================================================================

void PrepDefenseRushPatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_DefenseRush");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_DefenseRush!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "DefenseRush_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read DefenseRush_PatchOffset!");
		return;
	}

	g_pDefenseRushPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pDefenseRushPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pDefenseRushPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (both 32-bit and 64-bit are 0x0F 0x85)
	if (firstByte != 0x0F || secondByte != 0x85)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Patch 2] Byte mismatch at %X (expected 0F 85, got %02X %02X), patch aborted!",
			g_pDefenseRushPatchAddress, firstByte, secondByte);
		return;
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseRushBytes[i] = LoadFromAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bDefenseRushPatched = true;
	LogMessage("[BotRouteFix] [Patch 2] Replaced IsDefenseRushing jump with 6x NOPs @ %X (Anti-Rush Activated)", g_pDefenseRushPatchAddress);

	delete gc;
}

void RestoreDefenseRushPatch()
{
	if (g_bDefenseRushPatched && g_pDefenseRushPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), g_iOriginalDefenseRushBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 2] Restored original IsDefenseRushing jump @ %X", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = false;
	}
}

//========================================================================================
// PATCH 3: T C4 CARRIER INSTANT PLANT (IsTimeToPlantBomb delay removal)
//========================================================================================

void PrepC4PlantDelayPatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_C4PlantDelay");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_C4PlantDelay!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "C4PlantDelay_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read C4PlantDelay_PatchOffset!");
		return;
	}

	g_pC4PlantDelayPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pC4PlantDelayPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 3] Already patched (NOPs present @ %X), skipping", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (32-bit: 0x0F 0x82, 64-bit: 0x0F 0x87)
	if (g_bIsWin64)
	{
		if (firstByte != 0x0F || secondByte != 0x87)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 3] Byte mismatch at %X (expected 0F 87, got %02X %02X), patch aborted!",
				g_pC4PlantDelayPatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x82)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 3] Byte mismatch at %X (expected 0F 82, got %02X %02X), patch aborted!",
				g_pC4PlantDelayPatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalC4PlantDelayBytes[i] = LoadFromAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bC4PlantDelayPatched = true;
	LogMessage("[BotRouteFix] [Patch 3] Replaced IsTimeToPlantBomb delay jump with 6x NOPs @ %X (C4 Carrier Instant Plant Activated)", g_pC4PlantDelayPatchAddress);

	delete gc;
}

void RestoreC4PlantDelayPatch()
{
	if (g_bC4PlantDelayPatched && g_pC4PlantDelayPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), g_iOriginalC4PlantDelayBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 3] Restored original C4 plant delay jump @ %X", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = false;
	}
}

//========================================================================================
// PATCH 4: T C4 CARRIER BOMBSITE RANDOMIZATION (GetClosestZone -> GetRandomZone)
//========================================================================================

void PrepC4RandomZonePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_C4PlantDelay");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_C4PlantDelay for Patch 4!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "C4RandomZone_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read C4RandomZone_PatchOffset!");
		return;
	}

	g_pC4RandomZonePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pC4RandomZonePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(1), NumberType_Int8);

	if (g_bIsWin64)
	{
		g_iC4RandomZonePatchLength = 16;

		// Check if already patched: 48 8B CB E8 (mov rcx, rbx; call GetRandomZone)
		if (firstByte == 0x48 && secondByte == 0x8B)
		{
			LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping", g_pC4RandomZonePatchAddress);
			g_bC4RandomZonePatched = true;
			delete gc;
			return;
		}

		// Verify expected original bytes: 4C 8D (lea r8, [rsp+50h])
		if (firstByte != 0x4C || secondByte != 0x8D)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 4C 8D, got %02X %02X), patch aborted!",
				g_pC4RandomZonePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 16 bytes
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x64 bytes: mov rcx, rbx; call GetRandomZone (sub_180362B90); 8x NOPs
		int patchBytes[16] = {
			0x48, 0x8B, 0xCB,             // mov rcx, rbx (TheCSBots)
			0xE8, 0x34, 0xED, 0xFF, 0xFF, // call sub_180362B90 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 8x NOPs (pad out unused PathCost parameter prep)
		};

		for (int i = 0; i < 16; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), patchBytes[i], NumberType_Int8, true);
		}
	}
	else
	{
		g_iC4RandomZonePatchLength = 23;

		// Check if already patched: 8B CE E8 (mov ecx, esi; call GetRandomZone)
		if (firstByte == 0x8B && secondByte == 0xCE)
		{
			LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping", g_pC4RandomZonePatchAddress);
			g_bC4RandomZonePatched = true;
			delete gc;
			return;
		}

		// Verify expected original bytes: 8B 07 (mov eax, [edi])
		if (firstByte != 0x8B || secondByte != 0x07)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 8B 07, got %02X %02X), patch aborted!",
				g_pC4RandomZonePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 23 bytes
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x86 bytes: mov ecx, esi; call sub_10290E50 (GetRandomZone); 16x NOPs
		int patchBytes[23] = {
			0x8B, 0xCE,                   // mov ecx, esi (TheCSBots)
			0xE8, 0xB5, 0x55, 0xFD, 0xFF, // call sub_10290E50 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 16x NOPs (pad out unused PathCost parameter prep)
		};

		for (int i = 0; i < 23; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), patchBytes[i], NumberType_Int8, true);
		}
	}

	g_bC4RandomZonePatched = true;
	LogMessage("[BotRouteFix] [Patch 4] Replaced GetClosestZone with GetRandomZone @ %X (C4 50/50 Bombsite Randomization Activated)", g_pC4RandomZonePatchAddress);

	delete gc;
}

void RestoreC4RandomZonePatch()
{
	if (g_bC4RandomZonePatched && g_pC4RandomZonePatchAddress != Address_Null)
	{
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), g_iOriginalC4RandomZoneBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 4] Restored original GetClosestZone @ %X", g_pC4RandomZonePatchAddress);
		g_bC4RandomZonePatched = false;
	}
}

//========================================================================================
// MODULE 3: DYNAMIC CORRIDOR DANGER RESERVATION (CCSBot::ComputePath Post Detour)
//========================================================================================

void PrepComputePathHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hComputePathDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hComputePathDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to create DHook detour for CCSBot_ComputePath!");
		return;
	}

	if (!DHookSetFromConf(g_hComputePathDetour, gc, SDKConf_Signature, "CCSBot_ComputePath"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to find signature for CCSBot_ComputePath in gamedata!");
		return;
	}

	DHookAddParam(g_hComputePathDetour, HookParamType_VectorPtr);
	DHookAddParam(g_hComputePathDetour, HookParamType_Int);

	if (!DHookEnableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to enable post detour for CCSBot_ComputePath!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 3] Hooked CCSBot::ComputePath Post (Dynamic Corridor Reservation Activated)");
	delete gc;
}

void RestoreComputePathHook()
{
	if (g_hComputePathDetour != null)
	{
		DHookDisableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post);
		delete g_hComputePathDetour;
		g_hComputePathDetour = null;
	}
}

public MRESReturn Hook_ComputePath_Post(int client, Handle hReturn, Handle hParams)
{
	// If pathfinding failed, do not inject any corridor reservation
	if (!DHookGetReturn(hReturn))
		return MRES_Ignored;

	if (client <= 0 || client > MaxClients || !IsClientInGame(client))
		return MRES_Ignored;

	int team = GetClientTeam(client);
	if (team != 2 && team != 3) // 2=T, 3=CT
		return MRES_Ignored;

	int teamIdx = team - 2; // T=0, CT=1 (CS:S internal MAX_NAV_TEAMS index)

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null)
		return MRES_Ignored;

	int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	if (pathLength <= 0)
		return MRES_Ignored;

	// Limit corridor reservation depth to first 5 nav areas ahead
	int maxNodes = pathLength;
	if (maxNodes > 5)
		maxNodes = 5;

	float curtime = GetGameTime();

	for (int i = 0; i < maxNodes; i++)
	{
		Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
		Address pArea = LoadAddressFromAddress(pConnectInfo);
		if (pArea == Address_Null)
			continue;

		Address pDangerAddr = pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4);
		Address pTimestampAddr = pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4);

		float currentDanger = view_as<float>(LoadFromAddress(pDangerAddr, NumberType_Int32));
		float lastTimestamp = view_as<float>(LoadFromAddress(pTimestampAddr, NumberType_Int32));

		// Decay danger based on elapsed time (rate: 1.0 / 120.0 per second)
		float deltaT = curtime - lastTimestamp;
		if (deltaT > 0.0)
		{
			float decay = (1.0 / 120.0) * deltaT;
			currentDanger -= decay;
			if (currentDanger < 0.0)
				currentDanger = 0.0;
		}

		// Inject dynamic reservation penalty (+0.8f per bot)
		currentDanger += 0.8;
		if (currentDanger > 2.5)
			currentDanger = 2.5;

		StoreToAddress(pDangerAddr, view_as<int>(currentDanger), NumberType_Int32);
		StoreToAddress(pTimestampAddr, view_as<int>(curtime), NumberType_Int32);
	}

	return MRES_Ignored;
}
