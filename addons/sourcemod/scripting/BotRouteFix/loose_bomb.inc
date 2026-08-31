#if defined _botroutefix_loose_bomb_included
 #endinput
#endif
#define _botroutefix_loose_bomb_included

//========================================================================================
// MODULE 7: SINGLE RETRIEVER & TACTICAL COVER (CCSBot::NoticeLooseBomb Post Detour)
//========================================================================================

void PrepNoticeLooseBombHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hNoticeLooseBombDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hNoticeLooseBombDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to create DHook detour for CCSBot_NoticeLooseBomb!");
		return;
	}

	if (!DHookSetFromConf(g_hNoticeLooseBombDetour, gc, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to find signature for CCSBot_NoticeLooseBomb in gamedata!");
		return;
	}

	if (!DHookEnableDetour(g_hNoticeLooseBombDetour, true, Hook_NoticeLooseBomb_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to enable post detour for CCSBot_NoticeLooseBomb!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 7] Hooked CCSBot::NoticeLooseBomb Post (Single Retriever & Tactical Cover Activated)");
	delete gc;
}

void RestoreNoticeLooseBombHook()
{
	if (g_hNoticeLooseBombDetour != null)
	{
		DHookDisableDetour(g_hNoticeLooseBombDetour, true, Hook_NoticeLooseBomb_Post);
		delete g_hNoticeLooseBombDetour;
		g_hNoticeLooseBombDetour = null;
	}
}

public MRESReturn Hook_NoticeLooseBomb_Post(int client, Handle hReturn)
{
	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
		return MRES_Ignored;

	if (GetClientTeam(client) != 2) // Terrorists only
		return MRES_Ignored;

	// Only intercept if the official logic returned true (meaning loose C4 exists)
	if (!DHookGetReturn(hReturn))
		return MRES_Ignored;

	int looseBomb = GetLooseBombEntity();
	if (looseBomb == -1)
		return MRES_Ignored;

	float bombPos[3];
	GetEntPropVector(looseBomb, Prop_Data, "m_vecAbsOrigin", bombPos);

	int closestT = 0;
	float closestDist = 99999999.0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2)
		{
			float pos[3];
			GetClientAbsOrigin(i, pos);
			float dist = GetVectorDistance(pos, bombPos);
			if (dist < closestDist)
			{
				closestDist = dist;
				closestT = i;
			}
		}
	}

	// Only the single closest T retrieves the bomb; all other Ts maintain HuntState to provide cover!
	if (client == closestT)
	{
		return MRES_Ignored;
	}
	else
	{
		DHookSetReturn(hReturn, false);
		return MRES_Override;
	}
}

int GetLooseBombEntity()
{
	int ent = -1;
	while ((ent = FindEntityByClassname(ent, "weapon_c4")) != -1)
	{
		if (IsValidEntity(ent))
		{
			int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
			if (owner <= 0 || owner > MaxClients)
			{
				return ent;
			}
		}
	}
	return -1;
}
