//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点与防发疯前冲修复 (CT Bombsite Defense & Anti-Rush Fix)]
//
// 逆向根因：
//   1. 在 IdleState::OnUpdate() 中，guardBombsiteChance = -34.0f * Morale。
//      开局默认 Morale = 0，算得守点概率恒为 0.0%，条件跳转指令跳过守点，导致 CT 开局不守包。
//   2. 在进入守点前，引擎先检查 TheCSBots()->IsDefenseRushing()（每回合有 33.3% 几率判定全队前冲）。
//      一旦触发，直接执行 me->Hunt() 导致全队 CT 无脑冲向匪家送人头。
//
// 修复原理：
//   1. 对 guardBombsiteChance 跳转打入 6 字节 NOP 补丁，彻底激活官方原生守包点与掩体架枪体系；
//   2. 对 IsDefenseRushing 跳转打入 6 字节 NOP 补丁，彻底消除官方 33.3% 的全员无脑冲锋局；
//   3. 保留个别独狼 Bot（IsRogue）的自主单兵行动，兼顾战术纪律与战场多样性。
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
#define PLUGIN_VERSION "0.2.0"

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

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复 (CT 原生守包点与全队防发疯前冲)",
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

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	RestoreDefensePatch();
	RestoreDefenseRushPatch();
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
