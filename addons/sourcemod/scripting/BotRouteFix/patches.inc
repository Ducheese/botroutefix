#if defined _botroutefix_patches_included
 #endinput
#endif
#define _botroutefix_patches_included

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

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 1] Already patched (NOPs present @ %X), skipping", g_pDefensePatchAddress);
		g_bDefensePatched = false;
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

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = false;
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

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 3] Already patched (NOPs present @ %X), skipping", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = false;
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
// PATCH 4: T ROUND BOMBSITE RANDOM TARGET LOCK (GetClosestZone In-Memory Patch)
//========================================================================================

void PrepPlantBombClosestZonePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	// Resolve TheCSBotsPtr pointer address from IdleState_DefenseRush
	Address sigRush = GameConfGetAddress(gc, "IdleState_DefenseRush");
	if (sigRush != Address_Null)
	{
		if (g_bIsWin64)
		{
			int relOffset = LoadFromAddress(sigRush + view_as<Address>(3), NumberType_Int32);
			g_pTheCSBotsPtr = sigRush + view_as<Address>(7 + relOffset);
		}
		else
		{
			g_pTheCSBotsPtr = view_as<Address>(LoadFromAddress(sigRush + view_as<Address>(1), NumberType_Int32));
		}
	}

	Address sigAddr = GameConfGetAddress(gc, "IdleState_PlantBombClosestZone");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_PlantBombClosestZone!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "PlantBombClosestZone_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read PlantBombClosestZone_PatchOffset!");
		return;
	}

	g_pPlantBombClosestZonePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pPlantBombClosestZonePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x48 0xB8 on x64, 0xB8 on x86)
	if ((g_bIsWin64 && firstByte == 0x48 && secondByte == 0xB8) || (!g_bIsWin64 && firstByte == 0xB8))
	{
		LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping initialization", g_pPlantBombClosestZonePatchAddress);
		delete gc;
		return;
	}

	// Verify expected opcodes (64-bit: 45 33, 32-bit: 6A 00)
	if (g_bIsWin64)
	{
		if (firstByte != 0x45 || secondByte != 0x33)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 45 33, got %02X %02X), patch aborted!",
				g_pPlantBombClosestZonePatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x6A || secondByte != 0x00)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 6A 00, got %02X %02X), patch aborted!",
				g_pPlantBombClosestZonePatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 19 bytes
	for (int i = 0; i < 19; i++)
	{
		g_iOriginalPlantBombClosestZoneBytes[i] = LoadFromAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	LogMessage("[BotRouteFix] [Patch 4] PlantBombClosestZone Verified @ %X (TheCSBots @ %X, Target Lock Ready)",
		g_pPlantBombClosestZonePatchAddress, g_pTheCSBotsPtr);
	delete gc;
}

void ApplyLockedBombsitePatch(int zoneIndex)
{
	if (g_pPlantBombClosestZonePatchAddress == Address_Null || g_pTheCSBotsPtr == Address_Null)
		return;

	int offset = g_iOffset_ZoneBase + zoneIndex * g_iZoneStride;

	if (g_bIsWin64)
	{
		// 64-bit: mov rax, [imm64] (48 B8 [8-byte imm64]) + 9 NOPs (total 19 bytes)
		int lowDword = LoadFromAddress(g_pTheCSBotsPtr, NumberType_Int32) + offset;
		int highDword = LoadFromAddress(g_pTheCSBotsPtr + view_as<Address>(4), NumberType_Int32);

		StoreToAddress(g_pPlantBombClosestZonePatchAddress, 0x48, NumberType_Int8, true);
		StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(1), 0xB8, NumberType_Int8, true);
		StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(2), lowDword, NumberType_Int32, true);
		StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(6), highDword, NumberType_Int32, true);

		for (int i = 10; i < 19; i++)
		{
			StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
		}
	}
	else
	{
		// 32-bit: mov eax, [imm32] (B8 [4-byte imm32]) + 14 NOPs (total 19 bytes)
		int lowDword = LoadFromAddress(g_pTheCSBotsPtr, NumberType_Int32) + offset;

		StoreToAddress(g_pPlantBombClosestZonePatchAddress, 0xB8, NumberType_Int8, true);
		StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(1), lowDword, NumberType_Int32, true);

		for (int i = 5; i < 19; i++)
		{
			StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
		}
	}

	g_bPlantBombClosestZonePatched = true;
	LogMessage("[BotRouteFix] [Patch 4] Locked T Carrier Target Bombsite to Zone #%d (Offset +%d) @ %X",
		zoneIndex, offset, g_pPlantBombClosestZonePatchAddress);
}

void RestorePlantBombClosestZonePatch()
{
	if (g_bPlantBombClosestZonePatched && g_pPlantBombClosestZonePatchAddress != Address_Null)
	{
		for (int i = 0; i < 19; i++)
		{
			StoreToAddress(g_pPlantBombClosestZonePatchAddress + view_as<Address>(i),
				g_iOriginalPlantBombClosestZoneBytes[i], NumberType_Int8, true);
		}
		g_bPlantBombClosestZonePatched = false;
		LogMessage("[BotRouteFix] [Patch 4] Restored original GetClosestZone call @ %X", g_pPlantBombClosestZonePatchAddress);
	}
}

void SelectRandomBombsite()
{
	if (g_pTheCSBotsPtr == Address_Null)
		return;

	Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
	if (pTheCSBots == Address_Null)
		return;

	int zoneCount = LoadFromAddress(pTheCSBots + view_as<Address>(g_iOffset_ZoneCount), NumberType_Int32);
	if (zoneCount <= 0)
		return;

	int randomIdx = GetRandomInt(0, zoneCount - 1);
	ApplyLockedBombsitePatch(randomIdx);
}
