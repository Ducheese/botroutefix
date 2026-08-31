//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点防前冲修复 (CT Bombsite Defense Fix - v0.1)]
//
// 逆向根因：
//   在 CS:S 官方源码 (cs_bot_idle.cpp) 的 IdleState::OnUpdate() 中，CT Bot 在开局
//   判定是否前往包点防守的公式为：
//       float guardBombsiteChance = -34.0f * me->GetMorale();
//   由于 Bot 开局初始士气 Morale = 0（连赢时 Morale > 0），计算出的守点概率恒为 0.0f 或负数！
//   对应汇编中的概率分支指令：
//       64-bit: 0x1803644AF  0F 83 A7 00 00 00  (jnb loc_18036455C)
//       32-bit: 0x102BBE59   0F 86 A3 00 00 00  (jbe loc_102BBF02)
//   导致该跳转在开局 100% 必定触发，直接跳过了引擎原生的守包点逻辑，掉入保底的 me->Hunt()，
//   在全图搜寻最老区域（即距离 CT 出生点最远的匪家 T Spawn），造成全员冲匪家白给。
//
// 修复原理：
//   在 IdleState::OnUpdate() 概率跳转点打入 6 字节 NOP (0x90) 就地补丁，抹平恶意跳过分支，
//   彻底释放引擎原生的完整守点体系：
//     1. TheCSBots()->GetRandomZone()        -> 自动在 A 包点与 B 包点之间对半均匀分流；
//     2. TheCSBots()->GetRandomAreaInZone()  -> 自动在包点地面选取合法、贴地的 NavArea；
//     3. me->Hide(area, -1.0, guardRange)    -> 自动进入 HideState 并在包点掩体后蹲点架枪；
//     4. me->GetChatter()->GuardingBombsite()-> 自动发送原汁原味的守点无线电语音。
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
#define PLUGIN_VERSION "0.1.0"

//========================================================================================
// HANDLES & VARIABLES
//========================================================================================

bool g_bIsWin64 = false;

// Module 1: CT Bombsite Defense Patch
Address g_pDefensePatchAddress;
int g_iOriginalDefenseBytes[6];
bool g_bDefensePatched = false;

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复 (v0.1: CT 原生守包点与掩体架枪)",
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

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	RestoreDefensePatch();
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
// MODULE 1: CT BOMBSITE DEFENSE PATCH
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

	// Read and verify original bytes before patching
	int firstByte = LoadFromAddress(g_pDefensePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch] Already patched (NOPs present @ %X), skipping", g_pDefensePatchAddress);
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
			SetFailState("[BotRouteFix] [Patch] Byte mismatch at %X (expected 0F 83, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x86)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch] Byte mismatch at %X (expected 0F 86, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseBytes[i] = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6-byte NOP patch (0x90)
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pDefensePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bDefensePatched = true;
	LogMessage("[BotRouteFix] [Patch] Successfully replaced jump with 6x NOPs @ %X (Architecture: %s)",
		g_pDefensePatchAddress, g_bIsWin64 ? "x64" : "x86");
	LogMessage("[BotRouteFix] [Patch] CT Bombsite Defense logic is now permanently unlocked for all rounds!");

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
		LogMessage("[BotRouteFix] [Patch] Restored original jump bytes @ %X", g_pDefensePatchAddress);
		g_bDefensePatched = false;
	}
}
