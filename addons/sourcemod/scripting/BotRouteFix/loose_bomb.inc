#if defined _botroutefix_loose_bomb_included
 #endinput
#endif
#define _botroutefix_loose_bomb_included

//========================================================================================
// MODULE 7: TACTICAL TOP 3 RETRIEVER (NavMesh Ground Walk Distance)
//========================================================================================

Handle g_hNoticeLooseBombDetour = INVALID_HANDLE;
Handle g_hComputePathSDKCall = INVALID_HANDLE;

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

	// SDKCall CCSBot::ComputePath (to calculate true ground path distance)
	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_ComputePath");
	PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef);
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain); // RouteType (0 = FASTEST_ROUTE)
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	g_hComputePathSDKCall = EndPrepSDKCall();

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

enum struct CandidateT
{
	int client;
	float dist;
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

	// Gather all alive T bots and calculate NavMesh walkable distance
	CandidateT candidates[MAXPLAYERS + 1];
	int candidateCount = 0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			float walkDist = GetNavMeshWalkDistance(i, bombPos);
			if (walkDist < 0.0)
			{
				// Fallback to straight distance if pathfinding fails
				float botPos[3];
				GetClientAbsOrigin(i, botPos);
				walkDist = GetVectorDistance(botPos, bombPos) * 2.0;
			}

			candidates[candidateCount].client = i;
			candidates[candidateCount].dist = walkDist;
			candidateCount++;
		}
	}

	// If total alive Ts <= 3, everyone is allowed to retrieve
	if (candidateCount <= 3)
	{
		return MRES_Ignored;
	}

	// Sort candidates ascending by walkable distance
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

	// Allow top 3 closest Ts to retrieve the bomb
	bool isTop3 = false;
	for (int k = 0; k < 3 && k < candidateCount; k++)
	{
		if (candidates[k].client == client)
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

float GetNavMeshWalkDistance(int client, const float targetPos[3])
{
	if (g_hComputePathSDKCall == null)
		return -1.0;

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null)
		return -1.0;

	// Compute fastest path to the dropped bomb position (invokes NavAreaBuildPath under the hood)
	bool hasPath = SDKCall(g_hComputePathSDKCall, client, targetPos, 0); // 0 = FASTEST_ROUTE
	if (!hasPath)
		return -1.0;

	int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	if (pathLength <= 0)
		return -1.0;

	float totalDist = 0.0;
	float prevPos[3];
	GetClientAbsOrigin(client, prevPos);

	int vecOffset = g_iOffset_ConnectInfoPos;

	for (int i = 0; i < pathLength; i++)
	{
		Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
		float nodePos[3];
		nodePos[0] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset), NumberType_Int32));
		nodePos[1] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset + 4), NumberType_Int32));
		nodePos[2] = view_as<float>(LoadFromAddress(pConnectInfo + view_as<Address>(vecOffset + 8), NumberType_Int32));

		totalDist += GetVectorDistance(prevPos, nodePos);
		prevPos = nodePos;
	}

	totalDist += GetVectorDistance(prevPos, targetPos);
	return totalDist;
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
