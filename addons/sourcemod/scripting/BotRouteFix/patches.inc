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

		// Check if already patched (0x8B 0x87 = mov eax, [rdi+1E50h]).
		// Harden: adopt live bytes as the restore baseline (a previous session
		// may have ended patched without restore running).
		if (firstByte == 0x8B && secondByte == 0x87)
		{
			for (int i = 0; i < 26; i++)
			{
				g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(i), NumberType_Int8);
			}
			LogMessage("[BotRouteFix] [Patch 1] Already patched (Morale math present @ %X), skipping", g_pGuardBombsitePatchAddress);
			g_bGuardBombsitePatched = false;
			delete gc;
			return;
		}

		// Verify the full 26-byte template (opcodes/ModRM only; address bytes
		// drift per build and are skipped): movd/movaps/cvtdq2ps/movaps/mulss
		// + RandomFloat call + comiss + jnb. Any drift -> refuse, never partial-write.
		int chkOff64[18] = {0,1,2,3,8,9,10,11,12,13,14,15,16,17,18,19,20,21};
		int chkVal64[18] = {0x66,0x0F,0x6E,0xB7,0x41,0x0F,0x28,0xC8,0x0F,0x5B,0xF6,0x0F,0x28,0xC7,0xF3,0x0F,0x59,0x35};
		for (int i = 0; i < 18; i++)
		{
			int got = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(chkOff64[i]), NumberType_Int8);
			if (got != chkVal64[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 1] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff64[i], chkVal64[i], got);
				return;
			}
		}

		// Semantic anchor: the mulss constant must read exactly -34.0
		int kRel64 = LoadFromAddress(g_pGuardBombsitePatchAddress + view_as<Address>(22), NumberType_Int32);
		Address kAddr64 = g_pGuardBombsitePatchAddress + view_as<Address>(26 + kRel64);
		if (view_as<float>(LoadFromAddress(kAddr64, NumberType_Int32)) != -34.0)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Constant mismatch (expected -34.0), patch aborted!");
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
		// 32-bit non-Steam: 55-byte in-place x87 rewrite. The site is NOT a plain
		// float sequence but FPU arg-slot code (56 bytes @ sigAddr + 19):
		//   fld [100.0] / sub esp,8 / mov eax,[edi+morale] / mov [ebp+t],eax
		//   fstp [esp+4] / fldz / fstp [esp] / call RandomFloat
		//   fild [ebp+t] / add esp,8 / fmul [-34.0] / fcomip / fstp st / jbe skip
		// i.e. guard iff RandomFloat(0,100) < -34.0*morale. The rewrite keeps the
		// call, the compare shape and the FPU contract (stack empty at the jump)
		// and swaps the chance math to 70-10*morale, flipping jbe->jae (same len):
		//   sub esp,8 / mov eax,[edi+morale] / imul eax,-10 / add eax,70
		//   mov [esp],eax / fild [esp] / fldz / fstp [esp] / fld [100.0]
		//   fstp [esp+4] / call RandomFloat / add esp,8 / fcomip / fstp st / jae skip
		// All absolute addresses are extracted from the original bytes, so v91/v92
		// share this code path with zero version forks.
		int patchOffset = GameConfGetOffset(gc, "GuardBombsite_PatchOffset");
		if (patchOffset == -1)
		{
			delete gc;
			SetFailState("[BotRouteFix] Failed to read GuardBombsite_PatchOffset!");
			return;
		}

		g_pGuardBombsitePatchAddress = sigAddr + view_as<Address>(patchOffset);
		g_iGuardBombsitePatchSize = 55;

		Address base = g_pGuardBombsitePatchAddress;

		// Already patched? (0x6B 0xC0 0xF6 = imul eax, -10 at +9).
		// Harden: adopt live bytes as the restore baseline.
		if (LoadFromAddress(base + view_as<Address>(9), NumberType_Int8) == 0x6B
			&& LoadFromAddress(base + view_as<Address>(10), NumberType_Int8) == 0xC0
			&& LoadFromAddress(base + view_as<Address>(11), NumberType_Int8) == 0xF6)
		{
			for (int i = 0; i < 55; i++)
			{
				g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(base + view_as<Address>(i), NumberType_Int8);
			}
			LogMessage("[BotRouteFix] [Patch 1] Already patched (70-10*Morale present @ %X), skipping", base);
			g_bGuardBombsitePatched = false;
			delete gc;
			return;
		}

		// Preflight: verify the original 56-byte template opcodes before touching anything
		int chkOff[33] = {0,1,6,7,8,9,10,15,16,18,19,20,21,22,23,24,25,26,27,28,33,34,36,37,38,39,40,45,46,47,48,49,50};
		int chkVal[33] = {0xD9,0x05,0x83,0xEC,0x08,0x8B,0x87,0x89,0x45,0xD9,0x5C,0x24,0x04,0xD9,0xEE,0xD9,0x1C,0x24,0xFF,0x15,0xDB,0x45,0x83,0xC4,0x08,0xD8,0x0D,0xDF,0xF1,0xDD,0xD8,0x0F,0x86};
		for (int i = 0; i < 33; i++)
		{
			int got = LoadFromAddress(base + view_as<Address>(chkOff[i]), NumberType_Int8);
			if (got != chkVal[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 1] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff[i], chkVal[i], got);
				return;
			}
		}

		// Extract version-specific addresses + jump displacement from the template
		int addr100 = LoadFromAddress(base + view_as<Address>(2), NumberType_Int32);
		int moraleOff = LoadFromAddress(base + view_as<Address>(11), NumberType_Int32);
		int rfAddr = LoadFromAddress(base + view_as<Address>(29), NumberType_Int32);
		int kAddr = LoadFromAddress(base + view_as<Address>(41), NumberType_Int32);
		int jbeRel = LoadFromAddress(base + view_as<Address>(51), NumberType_Int32);

		// Semantic anchors: the two float constants must read exactly 100.0 / -34.0
		if (view_as<float>(LoadFromAddress(view_as<Address>(addr100), NumberType_Int32)) != 100.0
			|| view_as<float>(LoadFromAddress(view_as<Address>(kAddr), NumberType_Int32)) != -34.0)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Constant mismatch (expected 100.0/-34.0), patch aborted!");
			return;
		}

		// Backup original 55 bytes (restore path is size-driven, see RestoreGuardBombsitePatch)
		for (int i = 0; i < 55; i++)
		{
			g_iOriginalGuardBombsiteBytes[i] = LoadFromAddress(base + view_as<Address>(i), NumberType_Int8);
		}

		// Machine code: 70 - 10 * Morale, chance kept on the FPU across the call
		int patchBytes32[55] = {
			0x83, 0xEC, 0x08,                   // +0  sub esp,8
			0x8B, 0x87, 0, 0, 0, 0,             // +3  mov eax,[edi+morale] (dword spliced below)
			0x6B, 0xC0, 0xF6,                   // +9  imul eax,-10
			0x83, 0xC0, 0x46,                   // +12 add eax,70
			0x89, 0x04, 0x24,                   // +15 mov [esp],eax
			0xDB, 0x04, 0x24,                   // +18 fild dword [esp] (st = chance)
			0xD9, 0xEE,                         // +21 fldz
			0xD9, 0x1C, 0x24,                   // +23 fstp dword [esp] ([esp] = 0.0)
			0xD9, 0x05, 0, 0, 0, 0,             // +26 fld [100.0] (dword spliced below)
			0xD9, 0x5C, 0x24, 0x04,             // +32 fstp dword [esp+4] ([esp+4] = 100.0)
			0xFF, 0x15, 0, 0, 0, 0,             // +36 call [RandomFloat] (dword spliced below)
			0x83, 0xC4, 0x08,                   // +42 add esp,8
			0xDF, 0xF1,                         // +45 fcomip st,st(1) (R vs chance)
			0xDD, 0xD8,                         // +47 fstp st
			0x0F, 0x83, 0, 0, 0, 0              // +49 jae skip (rel spliced below)
		};
		for (int b = 0; b < 4; b++)
		{
			patchBytes32[5 + b] = (moraleOff >> (b * 8)) & 0xFF;
			patchBytes32[28 + b] = (addr100 >> (b * 8)) & 0xFF;
			patchBytes32[38 + b] = (rfAddr >> (b * 8)) & 0xFF;
			patchBytes32[51 + b] = (jbeRel >> (b * 8)) & 0xFF;
		}

		for (int i = 0; i < 55; i++)
		{
			StoreToAddress(base + view_as<Address>(i), patchBytes32[i], NumberType_Int8, true);
		}

		g_bGuardBombsitePatched = true;
		LogMessage("[BotRouteFix] [Patch 1] Rewrote guardBombsite formula in-place (70 - 10 * Morale) @ %X (32-bit, 55B x87 rewrite)", base);
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

	// Check if already patched (0x90 = NOP) -> safely skip and avoid zero-overwrite on unload.
	// Harden: adopt live bytes as the restore baseline.
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		for (int i = 0; i < 6; i++)
		{
			g_iOriginalAntiRushBytes[i] = LoadFromAddress(g_pAntiRushPatchAddress + view_as<Address>(i), NumberType_Int8);
		}
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pAntiRushPatchAddress);
		g_bAntiRushPatched = false;
		delete gc;
		return;
	}

	// Verify the JNE site + trailing context (rel32 drifts per build, skipped):
	// 32-bit: 0F 85 <rel> 8B CB E8 (jne; mov ecx,ebx; call)
	// 64-bit: 0F 85 <rel> 48 8B CE E8 (jne; mov rcx,rsi; call)
	if (g_bIsWin64)
	{
		int chkOff2w64[6] = {0,1,6,7,8,9};
		int chkVal2w64[6] = {0x0F,0x85,0x48,0x8B,0xCE,0xE8};
		for (int i = 0; i < 6; i++)
		{
			int got = LoadFromAddress(g_pAntiRushPatchAddress + view_as<Address>(chkOff2w64[i]), NumberType_Int8);
			if (got != chkVal2w64[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 2] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff2w64[i], chkVal2w64[i], got);
				return;
			}
		}
	}
	else
	{
		int chkOff2w32[5] = {0,1,6,7,8};
		int chkVal2w32[5] = {0x0F,0x85,0x8B,0xCB,0xE8};
		for (int i = 0; i < 5; i++)
		{
			int got = LoadFromAddress(g_pAntiRushPatchAddress + view_as<Address>(chkOff2w32[i]), NumberType_Int8);
			if (got != chkVal2w32[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 2] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff2w32[i], chkVal2w32[i], got);
				return;
			}
		}
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
		// Harden: adopt the live bytes as the restore baseline. A previous session may have
		// ended with the lock applied (no restore ran); without this, a later Restore would
		// write back 19 zero bytes over live code.
		for (int i = 0; i < 19; i++)
		{
			g_iOriginalBombsiteLockBytes[i] = LoadFromAddress(g_pBombsiteLockPatchAddress + view_as<Address>(i), NumberType_Int8);
		}
		LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, adopted live bytes as restore baseline", g_pBombsiteLockPatchAddress);
		delete gc;
		return;
	}

	// Verify the 19-byte call-sequence head (opcodes only):
	// 64-bit: 45 33 C9 4C 8D (xor r9d,r9d; lea r8,[rsp+..])
	// 32-bit: 6A 00 6A 02 57 FF 90 (push 0; push 2; push edi; call [eax+..])
	if (g_bIsWin64)
	{
		int chkOff4w64[5] = {0,1,2,3,4};
		int chkVal4w64[5] = {0x45,0x33,0xC9,0x4C,0x8D};
		for (int i = 0; i < 5; i++)
		{
			int got = LoadFromAddress(g_pBombsiteLockPatchAddress + view_as<Address>(chkOff4w64[i]), NumberType_Int8);
			if (got != chkVal4w64[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 4] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff4w64[i], chkVal4w64[i], got);
				return;
			}
		}
	}
	else
	{
		int chkOff4w32[7] = {0,1,2,3,4,5,6};
		int chkVal4w32[7] = {0x6A,0x00,0x6A,0x02,0x57,0xFF,0x90};
		for (int i = 0; i < 7; i++)
		{
			int got = LoadFromAddress(g_pBombsiteLockPatchAddress + view_as<Address>(chkOff4w32[i]), NumberType_Int8);
			if (got != chkVal4w32[i])
			{
				delete gc;
				SetFailState("[BotRouteFix] [Patch 4] Template mismatch at +%d (expected %02X, got %02X), patch aborted!",
					chkOff4w32[i], chkVal4w32[i], got);
				return;
			}
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

	// Already patched (0x90 = NOP).
	// Harden: adopt live bytes as the restore baseline.
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		for (int i = 0; i < 2; i++)
		{
			g_iOriginalReloadHideBytes[i] = LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(i), NumberType_Int8);
		}
		LogMessage("[BotRouteFix] [Patch 5] Already patched (NOPs present @ %X), skipping", g_pReloadHidePatchAddress);
		g_bReloadHidePatched = false;
		delete gc;
		return;
	}

	// Verify the dice-loss jump + neighbor (rel8 drifts per build, skipped):
	// 32-bit: 76 <rel> A1 (jbe; mov eax,[..]) | 64-bit: 73 <rel> 48 8B (jb; mov rax,[..])
	bool isExpected = false;
	if (g_bIsWin64)
		isExpected = (firstByte == 0x73
			&& LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(2), NumberType_Int8) == 0x48
			&& LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(3), NumberType_Int8) == 0x8B);
	else
		isExpected = (firstByte == 0x76
			&& LoadFromAddress(g_pReloadHidePatchAddress + view_as<Address>(2), NumberType_Int8) == 0xA1);

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

//========================================================================================
// PATCH 6: HUNT RANDOM-OVERRIDE BYPASS (restore the engine's oldest-scan)
//
// HuntState::OnUpdate scans for the oldest-cleared area, then UNCONDITIONALLY
// overwrites it with a uniform-random pick (missing NULL guard). Our Post
// flush stamps claims, but with the reader dead the stamps do nothing.
// This patch NOPs the random loop's m_huntArea store only (the loop still
// iterates+breaks, RandomInt still runs, RNG stream unchanged):
//   32-bit (v91+v92, byte-identical): 89 4B 04 (mov [ebx+4],ecx) -> 3x NOP
//   64-bit: 49 89 56 08 (mov [r14+8],rdx) -> 4x NOP
// Effect: the oldest-scan result stands; claim stamps become live mutual
// exclusion (freshly stamped age 0 never wins the strict-> scan; ties go to
// vector order; release is automatic when a bot stops stamping).
// Enhancement-grade: any mismatch -> warning + skip (Pre+flush continue).
//========================================================================================

void PrepHuntRandomBypassPatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "HuntState_RandomBypass");
	if (sigAddr == Address_Null)
	{
		LogMessage("[BotRouteFix] [Patch 6] Warning: Failed to locate HuntState signature, bypass skipped!");
		delete gc;
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "HuntState_RandomBypass_PatchOffset");
	if (patchOffset == -1)
	{
		LogMessage("[BotRouteFix] [Patch 6] Warning: Failed to read HuntState_RandomBypass_PatchOffset, bypass skipped!");
		delete gc;
		return;
	}

	g_pHuntBypassPatchAddress = sigAddr + view_as<Address>(patchOffset);
	int patchSize = g_bIsWin64 ? 4 : 3;
	g_iHuntBypassPatchSize = patchSize;

	// Already bypassed (all NOP)? Adopt live bytes and skip.
	bool allNop = true;
	for (int i = 0; i < patchSize; i++)
	{
		if (LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(i), NumberType_Int8) != 0x90)
		{
			allNop = false;
			break;
		}
	}
	if (allNop)
	{
		for (int i = 0; i < patchSize; i++)
		{
			g_iOriginalHuntBypassBytes[i] = 0x90;
		}
		LogMessage("[BotRouteFix] [Patch 6] Already bypassed (NOPs present @ %X), skipping", g_pHuntBypassPatchAddress);
		g_bHuntBypassPatched = false;
		delete gc;
		return;
	}

	// Verify expected store (opcodes only, no addresses):
	// 32-bit: 89 4B 04 | 64-bit: 49 89 56 08
	bool isExpected = false;
	if (g_bIsWin64)
		isExpected = (LoadFromAddress(g_pHuntBypassPatchAddress, NumberType_Int8) == 0x49
			&& LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(1), NumberType_Int8) == 0x89
			&& LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(2), NumberType_Int8) == 0x56
			&& LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(3), NumberType_Int8) == 0x08);
	else
		isExpected = (LoadFromAddress(g_pHuntBypassPatchAddress, NumberType_Int8) == 0x89
			&& LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(1), NumberType_Int8) == 0x4B
			&& LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(2), NumberType_Int8) == 0x04);

	if (!isExpected)
	{
		LogMessage("[BotRouteFix] [Patch 6] Warning: template mismatch, bypass skipped (flush-only mode)!");
		delete gc;
		return;
	}

	// Backup original bytes
	for (int i = 0; i < patchSize; i++)
	{
		g_iOriginalHuntBypassBytes[i] = LoadFromAddress(g_pHuntBypassPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// NOP the store
	for (int i = 0; i < patchSize; i++)
	{
		StoreToAddress(g_pHuntBypassPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bHuntBypassPatched = true;
	LogMessage("[BotRouteFix] [Patch 6] Bypassed hunt random-override (oldest-scan restored) @ %X [%s]", g_pHuntBypassPatchAddress, g_bIsWin64 ? "64-bit" : "32-bit");
}

void RestoreHuntRandomBypassPatch()
{
	if (g_bHuntBypassPatched && g_pHuntBypassPatchAddress != Address_Null && g_iHuntBypassPatchSize > 0)
	{
		for (int i = 0; i < g_iHuntBypassPatchSize; i++)
		{
			StoreToAddress(g_pHuntBypassPatchAddress + view_as<Address>(i), g_iOriginalHuntBypassBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 6] Restored original hunt random-override store @ %X", g_pHuntBypassPatchAddress);
		g_bHuntBypassPatched = false;
	}
}
