#if defined _botroutefix_corridor_danger_included
 #endinput
#endif
#define _botroutefix_corridor_danger_included

//========================================================================================
// MODULE 3: DYNAMIC CORRIDOR DANGER RESERVATION (CCSBot::ComputePath Post Detour)
//========================================================================================

void PrepComputePathHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hComputePathDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hComputePathDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to create DHook detour for CCSBot_ComputePath!");
		return;
	}

	if (!DHookSetFromConf(g_hComputePathDetour, gc, SDKConf_Signature, "CCSBot_ComputePath"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to find signature for CCSBot_ComputePath in gamedata!");
		return;
	}

	DHookAddParam(g_hComputePathDetour, HookParamType_VectorPtr);
	DHookAddParam(g_hComputePathDetour, HookParamType_Int);

	if (!DHookEnableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to enable post detour for CCSBot_ComputePath!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 3] Hooked CCSBot::ComputePath Post (Dynamic Corridor Reservation Activated)");
	delete gc;
}

void RestoreComputePathHook()
{
	if (g_hComputePathDetour != null)
	{
		DHookDisableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post);
		delete g_hComputePathDetour;
		g_hComputePathDetour = null;
	}
}

public MRESReturn Hook_ComputePath_Post(int client, Handle hReturn, Handle hParams)
{
	// If pathfinding failed, do not inject any corridor reservation
	if (!DHookGetReturn(hReturn))
		return MRES_Ignored;

	if (client <= 0 || client > MaxClients || !IsClientInGame(client))
		return MRES_Ignored;

	int team = GetClientTeam(client);
	if (team != 2 && team != 3) // 2=T, 3=CT
		return MRES_Ignored;

	int teamIdx = team - 2; // T=0, CT=1 (CS:S internal MAX_NAV_TEAMS index)

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null)
		return MRES_Ignored;

	int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	if (pathLength <= 0)
		return MRES_Ignored;

	// Limit corridor reservation depth to first 5 nav areas ahead
	int maxNodes = pathLength;
	if (maxNodes > 5)
		maxNodes = 5;

	float curtime = GetGameTime();

	for (int i = 0; i < maxNodes; i++)
	{
		Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
		Address pArea = LoadAddressFromAddress(pConnectInfo);
		if (pArea == Address_Null)
			continue;

		Address pDangerAddr = pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4);
		Address pTimestampAddr = pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4);

		float currentDanger = view_as<float>(LoadFromAddress(pDangerAddr, NumberType_Int32));
		float lastTimestamp = view_as<float>(LoadFromAddress(pTimestampAddr, NumberType_Int32));

		// Decay danger based on elapsed time (rate: 1.0 / 120.0 per second)
		float deltaT = curtime - lastTimestamp;
		if (deltaT > 0.0)
		{
			float decay = (1.0 / 120.0) * deltaT;
			currentDanger -= decay;
			if (currentDanger < 0.0)
				currentDanger = 0.0;
		}

		// Inject dynamic reservation penalty (+0.8f per bot)
		currentDanger += 0.8;
		if (currentDanger > 2.5)
			currentDanger = 2.5;

		StoreToAddress(pDangerAddr, view_as<int>(currentDanger), NumberType_Int32);
		StoreToAddress(pTimestampAddr, view_as<int>(curtime), NumberType_Int32);
	}

	return MRES_Ignored;
}
