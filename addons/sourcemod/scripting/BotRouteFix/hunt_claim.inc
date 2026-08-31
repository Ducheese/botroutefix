#if defined _botroutefix_hunt_claim_included
 #endinput
#endif
#define _botroutefix_hunt_claim_included

//========================================================================================
// MODULE 6: HUNT TARGET CLAIM & FLUSH (HuntState::OnUpdate Post Detour)
//========================================================================================

void PrepHuntStateHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hHuntStateDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
	if (g_hHuntStateDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to create DHook detour for HuntState_OnUpdate!");
		return;
	}

	if (!DHookSetFromConf(g_hHuntStateDetour, gc, SDKConf_Signature, "HuntState_OnUpdate"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to find signature for HuntState_OnUpdate in gamedata!");
		return;
	}

	DHookAddParam(g_hHuntStateDetour, HookParamType_CBaseEntity);

	if (!DHookEnableDetour(g_hHuntStateDetour, true, Hook_HuntState_OnUpdate_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to enable post detour for HuntState_OnUpdate!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 6] Hooked HuntState::OnUpdate Post (Target Claim & Anti-Swarm Activated)");
	delete gc;
}

void RestoreHuntStateHook()
{
	if (g_hHuntStateDetour != null)
	{
		DHookDisableDetour(g_hHuntStateDetour, true, Hook_HuntState_OnUpdate_Post);
		delete g_hHuntStateDetour;
		g_hHuntStateDetour = null;
	}
}

public MRESReturn Hook_HuntState_OnUpdate_Post(Address pThis, DHookParam hParams)
{
	if (pThis == Address_Null)
		return MRES_Ignored;

	int client = hParams.Get(1);
	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
		return MRES_Ignored;

	int team = GetClientTeam(client);
	if (team != 2 && team != 3) // 2=T, 3=CT
		return MRES_Ignored;

	int teamIdx = team - 2; // T=0, CT=1 (CS:S internal MAX_NAV_TEAMS index)

	// Read m_huntArea from HuntState instance (+4 on 32-bit, +8 on 64-bit)
	Address pArea = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_HuntArea));
	if (pArea == Address_Null)
		return MRES_Ignored;

	// Flush and claim this area immediately: set m_clearedTimestamp[teamIdx] = curtime
	float curtime = GetGameTime();
	Address pClearedTimestampAddr = pArea + view_as<Address>(g_iOffset_ClearedTimestamp + teamIdx * 4);
	StoreToAddress(pClearedTimestampAddr, view_as<int>(curtime), NumberType_Int32);

	return MRES_Ignored;
}
