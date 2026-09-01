#if defined _botroutefix_carrier_escort_included
 #endinput
#endif
#define _botroutefix_carrier_escort_included

//========================================================================================
// MODULE 5: T C4 CARRIER ESCORT & BODYGUARD SYSTEM
//========================================================================================

void PrepFollowSDKCalls()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_Follow");
	PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer);
	g_hFollowSDKCall = EndPrepSDKCall();
	if (g_hFollowSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 5] Failed to create SDKCall for CCSBot_Follow!");
		return;
	}

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_StopFollowing");
	g_hStopFollowingSDKCall = EndPrepSDKCall();
	if (g_hStopFollowingSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 5] Failed to create SDKCall for CCSBot_StopFollowing!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 5] Follow & StopFollowing SDKCalls Prepared (Carrier Escort Activated)");
	delete gc;
}

void AssignCarrierBodyguards()
{
	int carrier = GetC4Carrier();
	if (carrier <= 0 || !IsPlayerAlive(carrier))
		return;

	// Reset all active followers first (ensures newly promoted carrier clears old leader state)
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			if (g_hStopFollowingSDKCall != null)
			{
				SDKCall(g_hStopFollowingSDKCall, i);
			}
		}
	}

	// Collect alive T teammates (excluding the carrier) and calculate NavMesh distance once
	CandidateT candidates[MAXPLAYERS + 1];
	int candidateCount = 0;
	float carrierOrigin[3];
	GetClientAbsOrigin(carrier, carrierOrigin);

	for (int i = 1; i <= MaxClients; i++)
	{
		if (i == carrier)
			continue;

		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			float walkDist = GetNavMeshWalkDistance(i, carrierOrigin);
			if (walkDist < 0.0)
			{
				float pos[3];
				GetClientAbsOrigin(i, pos);
				walkDist = GetVectorDistance(carrierOrigin, pos) * 2.0;
			}

			candidates[candidateCount].client = i;
			candidates[candidateCount].dist = walkDist;
			candidateCount++;
		}
	}

	if (candidateCount <= 0)
		return;

	// 1. Read carrier's Morale
	int carrierMorale = 1; // Default POSITIVE
	Address pCarrier = GetEntityAddress(carrier);
	if (pCarrier != Address_Null && g_iOffset_Morale > 0)
	{
		carrierMorale = LoadFromAddress(pCarrier + view_as<Address>(g_iOffset_Morale), NumberType_Int32);
	}

	// 2. Linear Bodyguard formula: 3 - Morale (clamped to [0, 6])
	int bodyguardsToAssign = 3 - carrierMorale;
	if (bodyguardsToAssign < 0) bodyguardsToAssign = 0;
	if (bodyguardsToAssign > 6) bodyguardsToAssign = 6;

	// 3. Linear Plant Delay formula: 30.0 + 10.0 * Morale (clamped to [0.0, 60.0])
	float plantDelay = 30.0 + 10.0 * float(carrierMorale);
	if (plantDelay < 0.0) plantDelay = 0.0;
	if (plantDelay > 60.0) plantDelay = 60.0;

	// 4. Dynamically update TheCSBots()->m_earliestBombPlantTimestamp in engine memory
	if (g_pTheCSBotsPtr != Address_Null && g_iOffset_EarliestBombPlant > 0)
	{
		Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
		if (pTheCSBots != Address_Null)
		{
			float targetTimestamp = GetGameTime() + plantDelay;
			StoreToAddress(pTheCSBots + view_as<Address>(g_iOffset_EarliestBombPlant), view_as<int>(targetTimestamp), NumberType_Int32);
		}
	}

	// Sort candidates ascending by pre-calculated distance (pure in-memory sort, 0 extra A* calls)
	for (int i = 0; i < candidateCount - 1; i++)
	{
		for (int j = 0; j < candidateCount - i - 1; j++)
		{
			if (candidates[j].dist > candidates[j + 1].dist)
			{
				CandidateT temp;
				temp = candidates[j];
				candidates[j] = candidates[j + 1];
				candidates[j + 1] = temp;
			}
		}
	}

	// Assign the closest bodyguards
	int assignedCount = 0;
	for (int i = 0; i < bodyguardsToAssign && i < candidateCount; i++)
	{
		int bot = candidates[i].client;
		if (g_hFollowSDKCall != null)
		{
			SDKCall(g_hFollowSDKCall, bot, carrier);
			assignedCount++;
		}
	}

	char sign[2];
	sign[0] = (carrierMorale > 0) ? '+' : '\0';
	sign[1] = '\0';

	LogMessage("[BotRouteFix] [Module 5] Assigned %d/%d bodyguard(s) to C4 carrier %N (Morale: %s%d, Plant Delay: %.1fs)",
		assignedCount, bodyguardsToAssign, carrier, sign, carrierMorale, plantDelay);
}

int GetC4Carrier()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2)
		{
			int c4 = GetPlayerWeaponSlot(i, 4);
			if (c4 != -1 && IsValidEntity(c4))
				return i;
		}
	}
	return 0;
}
