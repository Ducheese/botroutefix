#if defined _botroutefix_danger_reset_included
 #endinput
#endif
#define _botroutefix_danger_reset_included

//========================================================================================
// MODULE 3: ROUND DANGER RESET (CUtlVector<CNavArea*> TheNavAreas Danger Zeroing)
//========================================================================================

Address g_pTheNavAreasCount;
Address g_pTheNavAreasBase;

void PrepDangerReset()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "CCSBotManager_GetRandomArea");
	if (sigAddr != Address_Null)
	{
		if (g_bIsWin64)
		{
			// 64-bit: mov ecx, cs:dword_1807FF970 @ sigAddr + 14 (instruction length: 6 bytes)
			int relOffset = LoadFromAddress(sigAddr + view_as<Address>(16), NumberType_Int32);
			g_pTheNavAreasCount = sigAddr + view_as<Address>(20 + relOffset);
			g_pTheNavAreasBase = g_pTheNavAreasCount - view_as<Address>(16);
		}
		else
		{
			// 32-bit: mov ebx, dword_105E83A4 @ sigAddr + 4
			g_pTheNavAreasCount = LoadAddressFromAddress(sigAddr + view_as<Address>(6));
			if (g_pTheNavAreasCount != Address_Null)
			{
				g_pTheNavAreasBase = g_pTheNavAreasCount - view_as<Address>(12);
			}
		}

		LogMessage("[BotRouteFix] [Module 3] Resolved TheNavAreas (Count: @%X, Base: @%X)",
			g_pTheNavAreasCount, g_pTheNavAreasBase);
	}
	else
	{
		LogMessage("[BotRouteFix] [Module 3] Warning: Failed to locate CCSBotManager_GetRandomArea signature!");
	}

	delete gc;
}

void ResetAllNavAreaDanger()
{
	if (g_pTheNavAreasCount == Address_Null || g_pTheNavAreasBase == Address_Null)
		return;

	int count = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
	if (count <= 0)
		return;

	Address pArray = LoadAddressFromAddress(g_pTheNavAreasBase);
	if (pArray == Address_Null)
		return;

	int ptrSize = g_iPtrSize;
	float curtime = GetGameTime();

	for (int i = 0; i < count; i++)
	{
		Address pArea = LoadAddressFromAddress(pArray + view_as<Address>(i * ptrSize));
		if (pArea == Address_Null)
			continue;

		// 1. Read existing Terrorist (index 0) and CT (index 1) danger values
		float dangerT = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger), NumberType_Int32));
		float dangerCT = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + 4), NumberType_Int32));

		// 2. Soft decay: reduce danger by 80% (retain 20% residual memory for dynamic path variance)
		dangerT *= 0.2;
		dangerCT *= 0.2;

		if (dangerT < 0.05)
			dangerT = 0.0;
		if (dangerCT < 0.05)
			dangerCT = 0.0;

		// 3. Store decayed danger values
		StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger), view_as<int>(dangerT), NumberType_Int32);
		StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger + 4), view_as<int>(dangerCT), NumberType_Int32);

		// 4. Reset danger timestamps to curtime
		StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp), view_as<int>(curtime), NumberType_Int32);
		StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + 4), view_as<int>(curtime), NumberType_Int32);
	}

	LogMessage("[BotRouteFix] [Module 3] Soft-decayed danger (80%% reduction) on all %d NavAreas for round restart", count);
}
