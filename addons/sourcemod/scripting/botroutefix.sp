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
// 支持架构：
//   - 32-bit: Windows non-Steam (v91/v92) server.dll
//   - 64-bit: Windows Steam x64 server.dll
//========================================================================================

#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>

#define GAMEDATA "botroutefix.gamedata"
#define PLUGIN_VERSION "0.4.0"

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

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复 (CT 守包/防冲 + T 包匪即刻运包与 50% 随机包点)",
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

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	RestoreDefensePatch();
	RestoreDefenseRushPatch();
	RestoreC4PlantDelayPatch();
	RestoreC4RandomZonePatch();
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

	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 1] Already patched (NOPs present @ %X), skipping", g_pDefensePatchAddress);
		g_bDefensePatched = true;
		delete gc;
		return;
	}

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

	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseBytes[i] = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

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

	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = true;
		delete gc;
		return;
	}

	if (firstByte != 0x0F || secondByte != 0x85)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Patch 2] Byte mismatch at %X (expected 0F 85, got %02X %02X), patch aborted!",
			g_pDefenseRushPatchAddress, firstByte, secondByte);
		return;
	}

	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseRushBytes[i] = LoadFromAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

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

	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 3] Already patched (NOPs present @ %X), skipping", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = true;
		delete gc;
		return;
	}

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

	for (int i = 0; i < 6; i++)
	{
		g_iOriginalC4PlantDelayBytes[i] = LoadFromAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

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

		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x64 bytes: mov rcx, rbx; call GetRandomZone (sub_180362B90); 8x NOPs
		int patchBytes[16] = {
			0x48, 0x8B, 0xCB,             // mov rcx, rbx
			0xE8, 0x34, 0xED, 0xFF, 0xFF, // call sub_180362B90 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 8x NOPs
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

		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x86 bytes: mov ecx, esi; call sub_10290E50 (GetRandomZone); 16x NOPs
		int patchBytes[23] = {
			0x8B, 0xCE,                   // mov ecx, esi
			0xE8, 0xB5, 0x55, 0xFD, 0xFF, // call sub_10290E50 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 16x NOPs
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
