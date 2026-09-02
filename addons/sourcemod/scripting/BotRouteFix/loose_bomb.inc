#if defined _botroutefix_loose_bomb_included
 #endinput
#endif
#define _botroutefix_loose_bomb_included

//========================================================================================
// MODULE 7: TACTICAL TOP 3 RETRIEVER (NavMesh Ground Walk Distance)
//========================================================================================

void PrepNoticeLooseBombHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	// Detour CCSBot::NoticeLooseBomb
	g_hNoticeLooseBombDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hNoticeLooseBombDetour != null)
	{
		if (DHookSetFromConf(g_hNoticeLooseBombDetour, gc, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
		{
			DHookEnableDetour(g_hNoticeLooseBombDetour, true, Hook_NoticeLooseBomb_Post);
			LogMessage("[BotRouteFix] [Module 7] Hooked CCSBot::NoticeLooseBomb Post (NavMesh Ground Distance Retriever Activated)");
		}
	}

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

void ResetLooseBombCache()
{
	g_fLastLooseBombCalcTime = 0.0;
	g_iTop3RetrieverCount = 0;
	for (int i = 0; i < 3; i++)
	{
		g_iTop3Retrievers[i] = 0;
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

	float curtime = GetGameTime();

	// Invalidate cache if tactical interval elapsed or any cached member died/disconnected
	bool needRecalc = (curtime - g_fLastLooseBombCalcTime >= TACTICAL_TICK_INTERVAL);
	if (!needRecalc)
	{
		for (int i = 0; i < g_iTop3RetrieverCount; i++)
		{
			int bot = g_iTop3Retrievers[i];
			if (bot <= 0 || !IsClientInGame(bot) || !IsPlayerAlive(bot) || GetClientTeam(bot) != 2)
			{
				needRecalc = true;
				break;
			}
		}
	}

	if (needRecalc)
	{
		// Gather all alive T bots and calculate NavMesh walkable distance (evaluated once per window: O(N))
		CandidateT candidates[MAXPLAYERS + 1];
		int candidateCount = 0;

		for (int i = 1; i <= MaxClients; i++)
		{
			if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
			{
				float walkDist = GetNavMeshWalkDistance(i, bombPos);
				if (walkDist < 0.0)
				{
					// Target is unreachable on NavMesh: set to massive distance so unreachable bot ranks dead last
					walkDist = 9999999.0;
				}

				candidates[candidateCount].client = i;
				candidates[candidateCount].dist = walkDist;
				candidateCount++;
			}
		}

		// Sort candidates ascending by distance
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

		// Cache the closest Top 3
		g_iTop3RetrieverCount = (candidateCount < 3) ? candidateCount : 3;
		for (int k = 0; k < g_iTop3RetrieverCount; k++)
		{
			g_iTop3Retrievers[k] = candidates[k].client;
		}
		g_fLastLooseBombCalcTime = curtime;
	}

	// If total alive Ts <= 3, everyone is allowed to retrieve
	if (g_iTop3RetrieverCount < 3)
	{
		return MRES_Ignored;
	}

	// O(1) membership check in the Top 3 cache
	bool isTop3 = false;
	for (int k = 0; k < g_iTop3RetrieverCount; k++)
	{
		if (g_iTop3Retrievers[k] == client)
		{
			isTop3 = true;
			break;
		}
	}

	if (isTop3)
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
