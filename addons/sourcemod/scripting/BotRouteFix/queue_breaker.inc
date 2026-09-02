#if defined _botroutefix_queue_breaker_included
 #endinput
#endif
#define _botroutefix_queue_breaker_included

//========================================================================================
// MODULE 4: 1.2S QUEUE BREAKER & DOORWAY TURNAROUND
//========================================================================================

void StartQueueBreakerTimer()
{
	if (g_hQueueBreakerTimer == INVALID_HANDLE)
	{
		g_hQueueBreakerTimer = CreateTimer(TACTICAL_TICK_INTERVAL, Timer_CheckPoliteQueue, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
		LogMessage("[BotRouteFix] [Module 4] 1.2s Queue Breaker Timer Started (Doorway Jam Prevention Activated)");
	}
}

void StopQueueBreakerTimer()
{
	if (g_hQueueBreakerTimer != INVALID_HANDLE)
	{
		KillTimer(g_hQueueBreakerTimer);
		g_hQueueBreakerTimer = INVALID_HANDLE;
	}
}

public Action Timer_CheckPoliteQueue(Handle timer)
{
	float curtime = GetGameTime();

	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsPlayerAlive(i) || !IsFakeClient(i))
			continue;

		Address pBot = GetEntityAddress(i);
		if (pBot == Address_Null)
			continue;

		int isWaiting = LoadFromAddress(pBot + view_as<Address>(g_iOffset_IsWaitingBehindFriend), NumberType_Int8);
		if (!isWaiting)
			continue;

		int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
		if (pathLength <= 0)
			continue;

		// Read CountdownTimer m_duration and m_timestamp
		int durationOffset = g_iOffset_PoliteTimer + g_iOffset_TimerDuration;
		int timestampOffset = g_iOffset_PoliteTimer + g_iOffset_TimerTimestamp;

		float duration = view_as<float>(LoadFromAddress(pBot + view_as<Address>(durationOffset), NumberType_Int32));
		float timestamp = view_as<float>(LoadFromAddress(pBot + view_as<Address>(timestampOffset), NumberType_Int32));

		float startTime = timestamp - duration;
		float elapsedWait = curtime - startTime;

		// If bot has been politely waiting behind a friend for >= 1.2s, break the queue
		if (elapsedWait >= 1.2 && duration > 0.0)
		{
			// 1. Clear waiting behind friend flag
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsWaitingBehindFriend), 0, NumberType_Int8);

			// 2. Add temporary Danger (+0.8f, max 2.5) to the actually blocked doorway NavArea so A* avoids this chokepoint on immediate repath
			int team = GetClientTeam(i);
			if (team == 2 || team == 3)
			{
				int teamIdx = team - 2;

				// Accurately locate currently blocked node using m_pathIndex
				int pathIndex = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathIndex), NumberType_Int32);
				if (pathIndex < 0)
					pathIndex = 0;
				if (pathIndex >= pathLength)
					pathIndex = pathLength - 1;

				// Current blocked node
				Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + pathIndex * g_iPathStride);
				Address pArea = LoadAddressFromAddress(pConnectInfo);
				if (pArea != Address_Null)
				{
					float rawDanger = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), NumberType_Int32));
					float timestamp = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), NumberType_Int32));

					// Settle elapsed time decay first
					float effDanger = rawDanger - ((curtime - timestamp) / 120.0);
					if (effDanger < 0.0)
						effDanger = 0.0;

					float newDanger = effDanger + 0.8;
					if (newDanger > 2.5)
						newDanger = 2.5; // Cap to prevent permanent deadlock

					StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), view_as<int>(newDanger), NumberType_Int32);
					StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), view_as<int>(curtime), NumberType_Int32);
				}

				// Forward adjacent node (if path extends ahead)
				int nextIdx = pathIndex + 1;
				if (nextIdx < pathLength)
				{
					Address pNextConnectInfo = pBot + view_as<Address>(g_iOffset_Path + nextIdx * g_iPathStride);
					Address pNextArea = LoadAddressFromAddress(pNextConnectInfo);
					if (pNextArea != Address_Null)
					{
						float rawDanger = view_as<float>(LoadFromAddress(pNextArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), NumberType_Int32));
						float timestamp = view_as<float>(LoadFromAddress(pNextArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), NumberType_Int32));

						// Settle elapsed time decay first
						float effDanger = rawDanger - ((curtime - timestamp) / 120.0);
						if (effDanger < 0.0)
							effDanger = 0.0;

						float newDanger = effDanger + 0.8;
						if (newDanger > 2.5)
							newDanger = 2.5; // Cap to prevent permanent deadlock

						StoreToAddress(pNextArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), view_as<int>(newDanger), NumberType_Int32);
						StoreToAddress(pNextArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), view_as<int>(curtime), NumberType_Int32);
					}
				}
			}

			// 3. DestroyPath: clear stopping, pathLength, pathIndex, and pathLadder to force immediate repath
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
			StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
			StoreToAddress(pBot + view_as<Address>(g_iOffset_PathIndex), 0, NumberType_Int32);
			StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsWaitingBehindFriend), 0, NumberType_Int8);

			// Invalidate polite timer (-1.0f) to wipe out any waiting state residue
			// IDA: polite m_duration@+g_iOffset_TimerDuration, m_timestamp@+g_iOffset_TimerTimestamp
			StoreToAddress(pBot + view_as<Address>(g_iOffset_PoliteTimer + g_iOffset_TimerTimestamp), view_as<int>(-1.0), NumberType_Int32);
		}
	}

	return Plugin_Continue;
}
