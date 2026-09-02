#if defined _botroutefix_events_included
 #endinput
#endif
#define _botroutefix_events_included

//========================================================================================
// GAME EVENTS
//========================================================================================

void HookGameEvents()
{
	HookEvent("round_freeze_end", Event_RoundFreezeEnd, EventHookMode_Post);
	HookEvent("item_pickup", Event_ItemPickup, EventHookMode_Post);
	HookEvent("bomb_dropped", Event_BombDropped, EventHookMode_Post);
	HookEvent("bomb_planted", Event_BombPlanted, EventHookMode_Post);
	HookEvent("bomb_defused", Event_BombEnded, EventHookMode_Post);
	HookEvent("bomb_exploded", Event_BombEnded, EventHookMode_Post);
	HookEvent("round_start", Event_RoundStart, EventHookMode_Post);
	HookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
}

public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = false;
	g_iDesignatedDefuser = 0;
	StopDefuseCoordTimer();
	ResetAllNavAreaDanger();
	SelectRandomBombsite();
	ResetLooseBombCache();
}

public void Event_RoundFreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
	AssignCarrierBodyguards();
	DumpRoundDangerStats();
	DumpRoundMoraleStats();
}

void DumpRoundMoraleStats()
{
	char ctMoraleBuf[256];
	char tMoraleBuf[256];
	ctMoraleBuf[0] = '\0';
	tMoraleBuf[0] = '\0';
	int ctCount = 0;
	int tCount = 0;
	int carrier = GetC4Carrier();

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && IsFakeClient(i))
		{
			Address pBot = GetEntityAddress(i);
			if (pBot != Address_Null && g_iOffset_Morale > 0)
			{
				int morale = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Morale), NumberType_Int32);
				int team = GetClientTeam(i);
				char name[32];
				GetClientName(i, name, sizeof(name));

				if (team == 3) // CT
				{
					float chance = 70.0 - 10.0 * float(morale);
					if (chance > 100.0) chance = 100.0;
					if (chance < 0.0) chance = 0.0;

					char entry[64];
					if (morale > 0)
						Format(entry, sizeof(entry), "%s: +%d (%.0f%%)  ", name, morale, chance);
					else
						Format(entry, sizeof(entry), "%s: %d (%.0f%%)  ", name, morale, chance);

					StrCat(ctMoraleBuf, sizeof(ctMoraleBuf), entry);
					ctCount++;
				}
				else if (team == 2) // T
				{
					char entry[64];
					if (i == carrier)
					{
						int escorts = 3 - morale;
						if (escorts < 0) escorts = 0;
						if (escorts > 6) escorts = 6;
						float delay = 30.0 + 10.0 * float(morale);
						if (delay < 0.0) delay = 0.0;
						if (delay > 60.0) delay = 60.0;

						if (morale > 0)
							Format(entry, sizeof(entry), "[Carrier]%s: +%d (%d Escorts, %.0fs Delay)  ", name, morale, escorts, delay);
						else
							Format(entry, sizeof(entry), "[Carrier]%s: %d (%d Escorts, %.0fs Delay)  ", name, morale, escorts, delay);
					}
					else
					{
						if (morale > 0)
							Format(entry, sizeof(entry), "%s: +%d  ", name, morale);
						else
							Format(entry, sizeof(entry), "%s: %d  ", name, morale);
					}

					StrCat(tMoraleBuf, sizeof(tMoraleBuf), entry);
					tCount++;
				}
			}
		}
	}

	if (ctCount > 0)
	{
		LogMessage("[BotRouteFix] [CT Morale Telemetry] %s", ctMoraleBuf);
	}
	if (tCount > 0)
	{
		LogMessage("[BotRouteFix] [T  Morale Telemetry] %s", tMoraleBuf);
	}
}

public void Event_ItemPickup(Event event, const char[] name, bool dontBroadcast)
{
	// 开局 GiveC4 触发的 c4 pickup 会在 BuyState 0.25s 内打断购买，3T 时 2 保镖 100% 不买枪，故冻结期内不触发
	if (GameRules_GetProp("m_bFreezePeriod"))
		return;

	char item[32];
	event.GetString("item", item, sizeof(item));

	if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
	{
		RequestFrame(Frame_AssignCarrierBodyguards);
	}
}

public void Frame_AssignCarrierBodyguards(any data)
{
	AssignCarrierBodyguards();
}

public void Event_BombDropped(Event event, const char[] name, bool dontBroadcast)
{
	// 1. When C4 is dropped on the floor, release bodyguards so they can react and cover
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

	// 2. Tactical Retake Adaptability: restore native GetClosestZone so new carrier adapts to current field position
	RestoreBombsiteLockPatch();

	// 3. Reset loose bomb retriever cache
	ResetLooseBombCache();
}

public void Event_BombPlanted(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = true;

	int bomb = FindEntityByClassname(-1, "planted_c4");
	if (bomb != -1 && IsValidEntity(bomb))
	{
		GetEntPropVector(bomb, Prop_Data, "m_vecAbsOrigin", g_fBombPlantedPos);
	}

	UpdateDesignatedDefuser();
	StartDefuseCoordTimer();
}

public void Event_BombEnded(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = false;
	g_iDesignatedDefuser = 0;
	StopDefuseCoordTimer();
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
	int victim = GetClientOfUserId(event.GetInt("userid"));
	if (victim > 0 && victim == g_iDesignatedDefuser)
	{
		UpdateDesignatedDefuser();
	}
}
