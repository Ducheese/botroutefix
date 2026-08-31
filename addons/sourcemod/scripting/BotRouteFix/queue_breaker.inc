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
		g_hQueueBreakerTimer = CreateTimer(0.1, Timer_CheckPoliteQueue, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
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

			// 2. DestroyPath: clear stopping, pathLength, and pathLadder to force immediate repath
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
			StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
			StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
		}
	}

	return Plugin_Continue;
}
