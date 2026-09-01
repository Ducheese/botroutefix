#if defined _botroutefix_patches_included
 #endinput
#endif
#define _botroutefix_patches_included

//========================================================================================
// PATCH 1: CT BOMBSITE DEFENSE (guardBombsiteChance)
//========================================================================================

void PrepGuardBombsitePatch()
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

	g_pGuardBombsitePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pGuardBombsitePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 1] Already patched (NOPs present @ %X), skipping", g_pGuardBombsitePatchAddress);
		g_bGuardBombsitePatched = false;
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
				g_pGuardBombsitePatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x86)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 0F 86, got %02X %02X), patch aborted!",
				g_pGuardBombsitePatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bGuardBombsitePatched = true;
	LogMessage("[BotRouteFix] [Patch 1] Replaced guardBombsite jump with 6x NOPs @ %X (CT Defense Unlocked)", g_pGuardBombsitePatchAddress);

	delete gc;
}

void RestoreGuardBombsitePatch()
{
	if (g_bGuardBombsitePatched && g_pGuardBombsitePatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), g_iOriginalGuardBombsiteBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 1] Restored original guardBombsite jump @ %X", g_pGuardBombsitePatchAddress);
		g_bGuardBombsitePatched = false;
	}
}

//========================================================================================
// PATCH 2: CT ANTI-RUSH (IsDefenseRushing)
//========================================================================================

void PrepAntiRushPatch()
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

	g_pAntiRushPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pAntiRushPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pAntiRushPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pAntiRushPatchAddress);
		g_bAntiRushPatched = false;
		delete gc;
		return;
	}

	// Verify expected jump opcode (both 32-bit and 64-bit are 0x0F 0x85)
	if (firstByte != 0x0F || secondByte != 0x85)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Patch 2] Byte mismatch at %X (expected 0F 85, got %02X %02X), patch aborted!",
				g_pAntiRushPatchAddress, firstByte, secondByte);
		return;
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalAntiRushBytes[i] = LoadFromAddress(g_pAntiRushPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pAntiRushPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bAntiRushPatched = true;
	LogMessage("[BotRouteFix] [Patch 2] Replaced IsDefenseRushing jump with 6x NOPs @ %X (Anti-Rush Activated)", g_pAntiRushPatchAddress);

	delete gc;
}

void RestoreAntiRushPatch()
{
	if (g_bAntiRushPatched && g_pAntiRushPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pAntiRushPatchAddress + view_as<Address>(i), g_iOriginalAntiRushBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 2] Restored original IsDefenseRushing jump @ %X", g_pAntiRushPatchAddress);
		g_bAntiRushPatched = false;
	}
}

//========================================================================================
// PATCH 3: T C4 CARRIER INSTANT PLANT (IsTimeToPlantBomb delay removal)
//========================================================================================

void PrepInstantPlantPatch()
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

	g_pInstantPlantPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pInstantPlantPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pInstantPlantPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 3] Already patched (NOPs present @ %X), skipping", g_pInstantPlantPatchAddress);
		g_bInstantPlantPatched = false;
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
				g_pInstantPlantPatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x82)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 3] Byte mismatch at %X (expected 0F 82, got %02X %02X), patch aborted!",
				g_pInstantPlantPatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalInstantPlantBytes[i] = LoadFromAddress(g_pInstantPlantPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pInstantPlantPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bInstantPlantPatched = true;
	LogMessage("[BotRouteFix] [Patch 3] Replaced IsTimeToPlantBomb delay jump with 6x NOPs @ %X (C4 Carrier Instant Plant Activated)", g_pInstantPlantPatchAddress);

	delete gc;
}

void RestoreInstantPlantPatch()
{
	if (g_bInstantPlantPatched && g_pInstantPlantPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pInstantPlantPatchAddress + view_as<Address>(i), g_iOriginalInstantPlantBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 3] Restored original C4 plant delay jump @ %X", g_pInstantPlantPatchAddress);
		g_bInstantPlantPatched = false;
	}
}

//========================================================================================
// PATCH 4: T ROUND BOMBSITE RANDOM TARGET LOCK (GetClosestZone In-Memory Patch)
//========================================================================================

void PrepBombsiteLockPatch()
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
		LogMessage("[BotRouteFix] [Patch 4] Resolved TheCSBotsPtr @ %X", g_pTheCSBotsPtr);
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

	g_pBombsiteLockPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pBombsiteLockPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pBombsiteLockPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x48 0xB8 on x64, 0xB8 on x86)
	if ((g_bIsWin64 && firstByte == 0x48 && secondByte == 0xB8) || (!g_bIsWin64 && firstByte == 0xB8))
	{
		LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping initialization", g_pBombsiteLockPatchAddress);
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
				g_pBombsiteLockPatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x6A || secondByte != 0x00)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 6A 00, got %02X %02X), patch aborted!",
				g_pBombsiteLockPatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 19 bytes
	for (int i = 0; i < 19; i++)
	{
		g_iOriginalBombsiteLockBytes[i] = LoadFromAddress(g_pBombsiteLockPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	LogMessage("[BotRouteFix] [Patch 4] Verified PlantBombClosestZone block @ %X", g_pBombsiteLockPatchAddress);
	delete gc;
}

void ApplyLockedBombsitePatch(int zoneIndex)
{
	if (g_pBombsiteLockPatchAddress == Address_Null || g_pTheCSBotsPtr == Address_Null)
		return;

	Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
	if (pTheCSBots == Address_Null)
		return;

	int offset = g_iOffset_ZoneBase + zoneIndex * g_iZoneStride;
	Address targetZone = pTheCSBots + view_as<Address>(offset);

	if (g_bIsWin64)
	{
		// 64-bit: mov rax, [imm64] (48 B8 [8-byte imm64]) + 9 NOPs (total 19 bytes)
		StoreToAddress(g_pBombsiteLockPatchAddress, 0x48, NumberType_Int8, true);
		StoreToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(1), 0xB8, NumberType_Int8, true);
		StoreAddressToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(2), targetZone, true);

		for (int i = 10; i < 19; i++)
		{
			StoreToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
		}
	}
	else
	{
		// 32-bit: mov eax, [imm32] (B8 [4-byte imm32]) + 14 NOPs (total 19 bytes)
		StoreToAddress(g_pBombsiteLockPatchAddress, 0xB8, NumberType_Int8, true);
		StoreAddressToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(1), targetZone, true);

		for (int i = 5; i < 19; i++)
		{
			StoreToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
		}
	}

	g_bBombsiteLockPatched = true;
	LogMessage("[BotRouteFix] [Patch 4] Locked T Carrier Target Bombsite to Zone #%d (Offset +%d) @ %X",
		zoneIndex, offset, g_pBombsiteLockPatchAddress);
}

void RestoreBombsiteLockPatch()
{
	if (g_bBombsiteLockPatched && g_pBombsiteLockPatchAddress != Address_Null)
	{
		for (int i = 0; i < 19; i++)
		{
			StoreToAddress(g_pBombsiteLockPatchAddress + view_as<Address>(i),
				g_iOriginalBombsiteLockBytes[i], NumberType_Int8, true);
		}
		g_bBombsiteLockPatched = false;
		LogMessage("[BotRouteFix] [Patch 4] Restored original GetClosestZone call @ %X", g_pBombsiteLockPatchAddress);
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
