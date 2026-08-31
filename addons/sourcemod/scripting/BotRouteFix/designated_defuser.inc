#if defined _botroutefix_designated_defuser_included
 #endinput
#endif
#define _botroutefix_designated_defuser_included

//========================================================================================
// MODULE 8: CT DESIGNATED DEFUSER & CROSSFIRE GUARD
//========================================================================================

void PrepDefuseSDKCalls()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_Hide");
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain); // CNavArea *area
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);        // float duration
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);        // float holdPositionTime
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);         // bool isStealth
	g_hHideSDKCall = EndPrepSDKCall();
	if (g_hHideSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 8] Failed to create SDKCall for CCSBot_Hide!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 8] Hide SDKCall Prepared (CT Defuse & Crossfire Guard Activated)");
	delete gc;
}

void StartDefuseCoordTimer()
{
	if (g_hDefuseCoordTimer == INVALID_HANDLE)
	{
		g_hDefuseCoordTimer = CreateTimer(0.2, Timer_DefuseCoordination, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
	}
}

void StopDefuseCoordTimer()
{
	if (g_hDefuseCoordTimer != INVALID_HANDLE)
	{
		KillTimer(g_hDefuseCoordTimer);
		g_hDefuseCoordTimer = INVALID_HANDLE;
	}
}

public Action Timer_DefuseCoordination(Handle timer)
{
	if (!g_bBombPlanted)
		return Plugin_Continue;

	UpdateDesignatedDefuser();
	if (g_iDesignatedDefuser <= 0)
		return Plugin_Continue;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsPlayerAlive(i) || GetClientTeam(i) != 3 || !IsFakeClient(i))
			continue;

		if (i == g_iDesignatedDefuser)
			continue;

		float ctPos[3];
		GetClientAbsOrigin(i, ctPos);
		float distToBomb = GetVectorDistance(ctPos, g_fBombPlantedPos);

		// Distance Gating: Only switch to Hide/Guard when CT enters the bombsite perimeter (<= 1500 units)
		// If farther away, let them continue sprinting to the bombsite via MoveTo!
		if (distToBomb > 1500.0)
			continue;

		Address pBot = GetEntityAddress(i);
		if (pBot == Address_Null)
			continue;

		int task = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32);
		// If a non-defuser CT bot is attempting task 3 (DEFUSE_BOMB):
		if (task == 3) // DEFUSE_BOMB
		{
			// Switch task to GUARD_BOMB_DEFUSER (task 5) and take cover to set up crossfire
			StoreToAddress(pBot + view_as<Address>(g_iOffset_Task), 5, NumberType_Int32);
			if (g_hHideSDKCall != null)
			{
				SDKCall(g_hHideSDKCall, i, Address_Null, -1.0, 0.0, false);
				LogMessage("[BotRouteFix] [Module 8] CT %N reached site perimeter (%.0f units) -> assigned crossfire guard (Lead defuser is %N)",
					i, distToBomb, g_iDesignatedDefuser);
			}
		}
	}

	return Plugin_Continue;
}

void UpdateDesignatedDefuser()
{
	if (!g_bBombPlanted)
	{
		g_iDesignatedDefuser = 0;
		return;
	}

	int bestCT = 0;
	float bestScore = 99999999.0;

	for (int i = 1; i <= MaxClients; i++)
	{
		// Only evaluate Bots for the AI designated lead defuser slot to prevent AFK human deadlock
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 3 && IsFakeClient(i))
		{
			float pos[3];
			GetClientAbsOrigin(i, pos);
			float dist = GetVectorDistance(pos, g_fBombPlantedPos);

			// Bonus if CT has defusal kit (-500 units priority)
			bool hasKit = (GetEntProp(i, Prop_Send, "m_bHasDefuser") == 1);
			float score = hasKit ? (dist - 500.0) : dist;

			if (score < bestScore)
			{
				bestScore = score;
				bestCT = i;
			}
		}
	}

	if (bestCT != g_iDesignatedDefuser && bestCT > 0)
	{
		g_iDesignatedDefuser = bestCT;
		LogMessage("[BotRouteFix] [Module 8] Designated %N as lead defuser", g_iDesignatedDefuser);
	}
}
