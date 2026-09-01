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

// ========================================================================================
// DANGER DIAGNOSTICS & TELEMETRY
// ========================================================================================

bool GetClientNavAreaDanger(int client, float &dangerT, float &dangerCT, int &areaId)
{
	dangerT = 0.0;
	dangerCT = 0.0;
	areaId = -1;

	if (g_pTheNavAreasCount == Address_Null || g_pTheNavAreasBase == Address_Null)
		return false;

	int count = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
	Address pArray = LoadAddressFromAddress(g_pTheNavAreasBase);
	if (count <= 0 || pArray == Address_Null)
		return false;

	float clientPos[3];
	GetClientAbsOrigin(client, clientPos);

	int nwOffset = g_bIsWin64 ? 8 : 4;
	int seOffset = g_bIsWin64 ? 20 : 16;
	int ptrSize = g_iPtrSize;

	Address pBestArea = Address_Null;
	float bestDistSq = 99999999.0;

	for (int i = 0; i < count; i++)
	{
		Address pArea = LoadAddressFromAddress(pArray + view_as<Address>(i * ptrSize));
		if (pArea == Address_Null)
			continue;

		float nwX = view_as<float>(LoadFromAddress(pArea + view_as<Address>(nwOffset), NumberType_Int32));
		float nwY = view_as<float>(LoadFromAddress(pArea + view_as<Address>(nwOffset + 4), NumberType_Int32));
		float seX = view_as<float>(LoadFromAddress(pArea + view_as<Address>(seOffset), NumberType_Int32));
		float seY = view_as<float>(LoadFromAddress(pArea + view_as<Address>(seOffset + 4), NumberType_Int32));

		// Check if client position falls inside this area's 2D bounding box
		if (clientPos[0] >= (nwX - 15.0) && clientPos[0] <= (seX + 15.0) &&
			clientPos[1] >= (nwY - 15.0) && clientPos[1] <= (seY + 15.0))
		{
			pBestArea = pArea;
			areaId = i;
			break;
		}

		// Fallback: track closest center
		float centerX = (nwX + seX) * 0.5;
		float centerY = (nwY + seY) * 0.5;
		float distSq = (clientPos[0] - centerX) * (clientPos[0] - centerX) + (clientPos[1] - centerY) * (clientPos[1] - centerY);
		if (distSq < bestDistSq)
		{
			bestDistSq = distSq;
			pBestArea = pArea;
			areaId = i;
		}
	}

	if (pBestArea != Address_Null)
	{
		dangerT = view_as<float>(LoadFromAddress(pBestArea + view_as<Address>(g_iOffset_Danger), NumberType_Int32));
		dangerCT = view_as<float>(LoadFromAddress(pBestArea + view_as<Address>(g_iOffset_Danger + 4), NumberType_Int32));
		return true;
	}

	return false;
}

void DumpRoundDangerStats()
{
	int carrier = GetC4Carrier();
	int sampleT = 0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			if (i != carrier)
			{
				sampleT = i;
				break;
			}
		}
	}

	if (sampleT > 0)
	{
		float dangerT, dangerCT;
		int areaId;
		if (GetClientNavAreaDanger(sampleT, dangerT, dangerCT, areaId))
		{
			LogMessage("[BotRouteFix] [Danger-Diag] T-Bot '%N' @ Area #%d -> T-Danger: %.2f | CT-Danger: %.2f",
				sampleT, areaId, dangerT, dangerCT);
		}
	}

	if (carrier > 0 && IsPlayerAlive(carrier))
	{
		float dangerT, dangerCT;
		int areaId;
		if (GetClientNavAreaDanger(carrier, dangerT, dangerCT, areaId))
		{
			LogMessage("[BotRouteFix] [Danger-Diag] Carrier '%N' @ Area #%d -> T-Danger: %.2f | CT-Danger: %.2f",
				carrier, areaId, dangerT, dangerCT);
		}
	}
}
