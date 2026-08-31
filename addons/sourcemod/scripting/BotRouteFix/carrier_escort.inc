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

	// Reset all active followers first to strictly enforce the bodyguard quota
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

	// Collect alive T teammates (excluding the carrier)
	int teammates[MAXPLAYERS + 1];
	int teammateCount = 0;
	float carrierOrigin[3];
	GetClientAbsOrigin(carrier, carrierOrigin);

	for (int i = 1; i <= MaxClients; i++)
	{
		if (i == carrier)
			continue;

		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			teammates[teammateCount++] = i;
		}
	}

	if (teammateCount <= 0)
		return;

	// Determine bodyguard count based on alive teammates ratio:
	// - 1 teammate left: 1 follows
	// - 2 teammates left: 1 follows, 1 free
	// - >= 3 teammates left: 2 follow, remaining free
	int bodyguardsToAssign = 1;
	if (teammateCount >= 3)
		bodyguardsToAssign = 2;

	// Sort teammates by distance to carrier (ascending)
	for (int i = 0; i < teammateCount - 1; i++)
	{
		for (int j = i + 1; j < teammateCount; j++)
		{
			float posI[3], posJ[3];
			GetClientAbsOrigin(teammates[i], posI);
			GetClientAbsOrigin(teammates[j], posJ);

			if (GetVectorDistance(carrierOrigin, posJ) < GetVectorDistance(carrierOrigin, posI))
			{
				int temp = teammates[i];
				teammates[i] = teammates[j];
				teammates[j] = temp;
			}
		}
	}

	// Assign the closest bodyguards
	for (int i = 0; i < bodyguardsToAssign && i < teammateCount; i++)
	{
		int bot = teammates[i];
		if (g_hFollowSDKCall != null)
		{
			SDKCall(g_hFollowSDKCall, bot, carrier);
			LogMessage("[BotRouteFix] [Module 5] Assigned %N to escort C4 carrier %N", bot, carrier);
		}
	}
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
