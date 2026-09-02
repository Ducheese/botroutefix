#if defined _botroutefix_patches_included
 #endinput
#endif
#define _botroutefix_patches_included

//========================================================================================
// PATCH 1: CT BOMBSITE DEFENSE (guardBombsiteChance Assembly Rewrite)
// Formula: GuardChance = 70.0 - 10.0 * Morale
//   Morale +3 (EXCELLENT): 40% Guard, 60% Roam/Hunt
//   Morale +1 (POSITIVE / Round Start Default): 60% Guard, 40% Roam/Hunt
//   Morale  0 (NEUTRAL):   70% Guard, 30% Roam/Hunt
//   Morale -1 (NEGATIVE):  80% Guard, 20% Roam/Hunt
//   Morale -3 (TERRIBLE):  100% Guard (Full Site Camp)
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

	if (g_bIsWin64)
	{
		// 64-bit Steam: Rewrite the 26-byte float math sequence at sigAddr in-place
		g_pGuardBombsitePatchAddress = sigAddr;
		g_iGuardBombsitePatchSize = 26;

		int firstByte = LoadFromAddress(g_pGuardBombsitePatchAddress, NumberType_Int8);
		int secondByte = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(1), NumberType_Int8);

		// Check if already patched (0x8B 0x87 = mov eax, [rdi+1E50h])
		if (firstByte == 0x8B && secondByte == 0x87)
		{
			LogMessage("[BotRouteFix] [Patch 1] Already patched (Morale math present @ %X), skipping", g_pGuardBombsitePatchAddress);
			g_bGuardBombsitePatched = false;
			delete gc;
			return;
		}

		// Verify expected original opcode (0x66 0x0F = movd xmm6, [rdi+1E50h])
		if (firstByte != 0x66 || secondByte != 0x0F)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 66 0F, got %02X %02X), patch aborted!",
				g_pGuardBombsitePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 26 bytes
		for (int i = 0; i < 26; i++)
		{
			g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Machine code: 70.0 - 10.0 * Morale (26 bytes exact alignment)
		int patchBytes64[26] = {
			0x8B, 0x87, 0x50, 0x1E, 0x00, 0x00, // mov eax, [rdi+1E50h] (m_morale)
			0x6B, 0xC0, 0xF6,                   // imul eax, -10
			0x83, 0xC0, 0x46,                   // add eax, 70 (70 - 10 * morale)
			0xF3, 0x0F, 0x2A, 0xF0,             // cvtsi2ss xmm6, eax
			0x41, 0x0F, 0x28, 0xC8,             // movaps xmm1, xmm8 (100.0f)
			0x0F, 0x28, 0xC7,                   // movaps xmm0, xmm7 (0.0f)
			0x90, 0x90, 0x90                    // 3x NOPs padding
		};

		for (int i = 0; i < 26; i++)
		{
			StoreToAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), patchBytes64[i], NumberType_Int8, true);
		}

		g_bGuardBombsitePatched = true;
		LogMessage("[BotRouteFix] [Patch 1] Rewrote guardBombsite formula in-place (70 - 10 * Morale) @ %X (64-bit)", g_pGuardBombsitePatchAddress);
	}
	else
	{
		// 32-bit non-Steam: Rewrite the 26-byte float math sequence at sigAddr + offset in-place
		int patchOffset = GameConfGetOffset(gc, "GuardBombsite_PatchOffset");
		if (patchOffset == -1)
		{
			delete gc;
			SetFailState("[BotRouteFix] Failed to read GuardBombsite_PatchOffset!");
			return;
		}

		g_pGuardBombsitePatchAddress = sigAddr + view_as<Address>(patchOffset);
		g_iGuardBombsitePatchSize = 26;

		int firstByte = LoadFromAddress(g_pGuardBombsitePatchAddress, NumberType_Int8);
		int secondByte = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(1), NumberType_Int8);

		// Check if already patched (0x83 0xEC = sub esp, 8)
		if (firstByte == 0x83 && secondByte == 0xEC)
		{
			LogMessage("[BotRouteFix] [Patch 1] Already patched (Morale math present @ %X), skipping", g_pGuardBombsitePatchAddress);
			g_bGuardBombsitePatched = false;
			delete gc;
			return;
		}

		// Verify expected original opcode (0xD9 0x05 = fld dword ptr [-34.0f])
		if (firstByte != 0xD9 || secondByte != 0x05)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected D9 05, got %02X %02X), patch aborted!",
				g_pGuardBombsitePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 26 bytes
		for (int i = 0; i < 26; i++)
		{
			g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Machine code: 70.0 - 10.0 * Morale (26 bytes exact alignment)
		// 83 EC 08                sub     esp, 8               ; (3B)
		// 8B 87 08 1A 00 00       mov     eax, [edi+1A08h]     ; (6B) eax = m_morale
		// 6B C0 F6                imul    eax, -10             ; (3B) eax = morale * -10
		// 83 C0 46                add     eax, 70              ; (3B) eax = 70 - 10 * morale
		// 89 04 24                mov     [esp], eax           ; (3B)
		// DB 04 24                fild    dword ptr [esp]      ; (3B) st(0) = float(chance)
		// D9 1C 24                fstp    dword ptr [esp]      ; (3B) store float chance at [esp]
		// 90 90                   nop     nop                  ; (2B)
		int patchBytes32[26] = {
			0x83, 0xEC, 0x08,
			0x8B, 0x87, 0x08, 0x1A, 0x00, 0x00,
			0x6B, 0xC0, 0xF6,
			0x83, 0xC0, 0x46,
			0x89, 0x04, 0x24,
			0xDB, 0x04, 0x24,
			0xD9, 0x1C, 0x24,
			0x90, 0x90
		};

		for (int i = 0; i < 26; i++)
		{
			StoreToAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), patchBytes32[i], NumberType_Int8, true);
		}

		g_bGuardBombsitePatched = true;
		LogMessage("[BotRouteFix] [Patch 1] Rewrote guardBombsite formula in-place (70 - 10 * Morale) @ %X (32-bit)", g_pGuardBombsitePatchAddress);
	}

	delete gc;
}

void RestoreGuardBombsitePatch()
{
	if (g_bGuardBombsitePatched && g_pGuardBombsitePatchAddress != Address_Null && g_iGuardBombsitePatchSize > 0)
	{
		for (int i = 0; i < g_iGuardBombsitePatchSize; i++)
		{
			StoreToAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), g_iOriginalGuardBombsiteBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 1] Restored original guardBombsite code (%d bytes) @ %X",
			g_iGuardBombsitePatchSize, g_pGuardBombsitePatchAddress);
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
//========================================================================================
// PATCH 3: T C4 CARRIER DYNAMIC PLANT TIMING (IsTimeToPlantBomb Morale Gating)
// Formula: Plant Delay = 30.0 + 10.0 * Morale (0.0s ~ 60.0s)
//   - Morale -3:  0.0s Delay (Instant Rush Plant)
//   - Morale  0: 30.0s Delay
//   - Morale +3: 60.0s Delay (Full Map Control & Hunting)
// Dynamically controlled in memory via TheCSBots()->m_earliestBombPlantTimestamp
//========================================================================================

void PrepInstantPlantPatch()
{
	LogMessage("[BotRouteFix] [Patch 3] Dynamic C4 Plant Delay Activated (0s ~ 60s Morale Gating)");
}

void RestoreInstantPlantPatch()
{
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

//========================================================================================
// PATCH 5: RELOAD HIDE 100% (hideChance 25+100*skill -> forced 100%)
// Original: RandomFloat(0,100) < hideChance (25+100*skill)  ~25%-125% skill gating
// Patched : NOP the conditional jump (jbe/jnb) so hide path is always taken
//           -> nearby enemy && !IsHiding() && TimeSinceLastSawEnemy<5.0  => 必进掩体
// 32-bit: DF F1 DD D8 76 ??  (jbe)  -> 90 90   @ sig+4
// 64-bit: FF15 .. 0F2FC6 73 ?? 48.. -> 90 90   @ sig+9
//========================================================================================

void PrepReloadHidePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "ReloadCheck_HideChance");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for ReloadCheck_HideChance!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "ReloadHide_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read ReloadHide_PatchOffset!");
		return;
	}

	g_pReloadHidePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pReloadHidePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 5] Already patched (NOPs present @ %X), skipping", g_pReloadHidePatchAddress);
		g_bReloadHidePatched = false;
		delete gc;
		return;
	}

	// Verify expected jump opcode: 32-bit 0x76 (jbe short), 64-bit 0x73 (jnb/jnc short)
	bool isExpected = false;
	if (g_bIsWin64)
		isExpected = (firstByte == 0x73);
	else
		isExpected = (firstByte == 0x76);

	if (!isExpected)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Patch 5] Byte mismatch at %X (expected %s, got %02X %02X), patch aborted!",
			g_pReloadHidePatchAddress, g_bIsWin64 ? "73" : "76", firstByte, secondByte);
		return;
	}

	// Backup original 2 bytes
	for (int i = 0; i < 2; i++)
	{
		g_iOriginalReloadHideBytes[i] = LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 2x NOPs -> hideChance branch always falls through to Hide()
	for (int i = 0; i < 2; i++)
	{
		StoreToAddress(g_pReloadHidePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bReloadHidePatched = true;
	LogMessage("[BotRouteFix] [Patch 5] Reload HideChance -> 100%% (NOPed %02X %02X @ %X) [%s]",
		g_iOriginalReloadHideBytes[0], g_iOriginalReloadHideBytes[1], g_pReloadHidePatchAddress, g_bIsWin64 ? "64-bit" : "32-bit");

	delete gc;
}

void RestoreReloadHidePatch()
{
	if (g_bReloadHidePatched && g_pReloadHidePatchAddress != Address_Null)
	{
		for (int i = 0; i < 2; i++)
		{
			StoreToAddress(g_pReloadHidePatchAddress + view_as<Address>(i), g_iOriginalReloadHideBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 5] Restored original Reload HideChance jump @ %X", g_pReloadHidePatchAddress);
		g_bReloadHidePatched = false;
	}
}
