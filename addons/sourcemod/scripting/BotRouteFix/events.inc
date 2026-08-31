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
	ResetAllNavAreaDanger();
	SelectRandomBombsite();
}

public void Event_RoundFreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
	AssignCarrierBodyguards();
}

public void Event_ItemPickup(Event event, const char[] name, bool dontBroadcast)
{
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
	RestorePlantBombClosestZonePatch();
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
}

public void Event_BombEnded(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = false;
	g_iDesignatedDefuser = 0;
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
	int victim = GetClientOfUserId(event.GetInt("userid"));
	if (victim > 0 && victim == g_iDesignatedDefuser)
	{
		UpdateDesignatedDefuser();
	}
}
