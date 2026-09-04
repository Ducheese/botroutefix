#if defined _botroutefix_t_morale_tactics_included
 #endinput
#endif
#define _botroutefix_t_morale_tactics_included

//========================================================================================
// MODULE 12: T-SIDE MORALE TACTICAL DISPOSITION & AUDIO INTERCEPTION
//
// 1. [下包前包匪行为 (Pre-Plant C4 Carrier)]
//    - 纯事件单次赋值：
//        * 低士气 (Morale <= -1): 设为 OPPORTUNITY_FIRE，心无旁骛运包；
//        * 高士气 (Morale >= 0): 保持原生 ENGAGE_AND_INVESTIGATE。
//    - MoveToState 运包全程不修改 disposition，单次事件即可自然维持至进点。
//
// 2. [下包后全队守点/追杀梯度 (Post-Plant Bomb Site Guard & Audio Interception)]
//    - 下包瞬间 (bomb_planted):
//        * 低士气 (Morale <= -1): 设为 OPPORTUNITY_FIRE，HideState 门控自动阻断出击，死守掩体；
//        * 高士气 (Morale >= 0): 保持 ENGAGE_AND_INVESTIGATE。
//    - 微秒级出击拦截 (DHook Pre on CCSBot::InvestigateNoise):
//        * 仅针对守包掩体任务 (Task 4: GUARD_TICKING_BOMB)；
//        * 针对保持 ENGAGE 的非负士气 T (Morale >= 0)，按士气阶梯约束最大调查距离：
//            Morale 0  -> maxNoiseDist = 500.0
//            Morale +1 -> maxNoiseDist = 1000.0
//            Morale +2 -> maxNoiseDist = 2000.0
//            Morale +3 -> maxNoiseDist = 3000.0
//        * 超距或概率未命中：直接 MRES_Supercede 阻止状态切换，原地保持架枪。
//========================================================================================

void PrepInvestigateNoiseHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hInvestigateNoiseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_CBaseEntity);
	if (g_hInvestigateNoiseDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 12] Failed to create DHook detour for CCSBot_InvestigateNoise!");
		return;
	}

	if (!DHookSetFromConf(g_hInvestigateNoiseDetour, gc, SDKConf_Signature, "CCSBot_InvestigateNoise"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 12] Failed to find signature for CCSBot_InvestigateNoise in gamedata!");
		return;
	}

	if (!DHookEnableDetour(g_hInvestigateNoiseDetour, false, Hook_InvestigateNoise_Pre))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 12] Failed to enable pre detour for CCSBot_InvestigateNoise!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 12] Hooked CCSBot::InvestigateNoise Pre (Zero-latency Morale Audio Interception Activated)");
	delete gc;
}

void RestoreInvestigateNoiseHook()
{
	if (g_hInvestigateNoiseDetour != null)
	{
		DHookDisableDetour(g_hInvestigateNoiseDetour, false, Hook_InvestigateNoise_Pre);
		delete g_hInvestigateNoiseDetour;
		g_hInvestigateNoiseDetour = null;
	}
}

public MRESReturn Hook_InvestigateNoise_Pre(int client, DHookParam hParams)
{
	if (!g_bBombPlanted)
		return MRES_Ignored;

	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
		return MRES_Ignored;

	if (GetClientTeam(client) != 2 || !IsFakeClient(client))
		return MRES_Ignored;

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null || g_iOffset_Task <= 0 || g_iOffset_Morale <= 0 || g_iOffset_NoisePosition <= 0)
		return MRES_Ignored;

	// 仅拦截守包任务 (Task 4: GUARD_TICKING_BOMB)
	// 逃跑任务 (Task 9: ESCAPE_FROM_BOMB) 与搜敌任务 (Task 0: SEEK_AND_DESTROY) 自然放行
	int task = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32);
	if (task != 4)
		return MRES_Ignored;

	// 能触发此处的守包 Bot 均为保持 ENGAGE 的非负士气 T (Morale >= 0)
	// 负士气 T 已在下包时置为 OPPORTUNITY_FIRE，在引擎 HideState 底层即被门控过滤，不会调用此处
	int morale = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Morale), NumberType_Int32);

	float maxNoiseDist = 500.0;
	if (morale == 1) maxNoiseDist = 1000.0;
	else if (morale == 2) maxNoiseDist = 2000.0;
	else if (morale >= 3) maxNoiseDist = 3000.0;

	float botPos[3], noisePos[3];
	GetClientAbsOrigin(client, botPos);
	noisePos[0] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition), NumberType_Int32));
	noisePos[1] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition + 4), NumberType_Int32));
	noisePos[2] = view_as<float>(LoadFromAddress(pBot + view_as<Address>(g_iOffset_NoisePosition + 8), NumberType_Int32));

	float dist = GetVectorDistance(botPos, noisePos);
	if (dist > maxNoiseDist)
	{
		if (g_iOffset_NoiseTimestamp > 0)
		{
			StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
		}
		LogMessage("[BotRouteFix] [Module 12] Intercepted distant InvestigateNoise (dist %.0f > %.0f) for T %N", dist, maxNoiseDist, client);
		return MRES_Supercede;
	}

	float chance = 100.0 * (1.0 - (dist / maxNoiseDist));
	if (GetRandomFloat(0.0, 100.0) > chance)
	{
		if (g_iOffset_NoiseTimestamp > 0)
		{
			StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
		}
		LogMessage("[BotRouteFix] [Module 12] Intercepted InvestigateNoise by probability (%.1f%%) for T %N", chance, client);
		return MRES_Supercede;
	}

	LogMessage("[BotRouteFix] [Module 12] T %N (Morale %d) allowed to investigate noise!", client, morale);
	return MRES_Ignored;
}

void ApplyCarrierMoraleDisposition()
{
	int carrier = GetC4Carrier();
	if (carrier <= 0 || !IsPlayerAlive(carrier) || !IsFakeClient(carrier))
		return;

	Address pCarrier = GetEntityAddress(carrier);
	if (pCarrier == Address_Null || g_iOffset_Morale <= 0 || g_iOffset_Disposition <= 0)
		return;

	int morale = LoadFromAddress(pCarrier + view_as<Address>(g_iOffset_Morale), NumberType_Int32);
	if (morale <= -1)
	{
		StoreToAddress(pCarrier + view_as<Address>(g_iOffset_Disposition), 1, NumberType_Int32); // OPPORTUNITY_FIRE
		LogMessage("[BotRouteFix] [Module 12] C4 Carrier %N (Morale %d) set to OPPORTUNITY_FIRE (Focused Plant)", carrier, morale);
	}
	else
	{
		StoreToAddress(pCarrier + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
		LogMessage("[BotRouteFix] [Module 12] C4 Carrier %N (Morale %d) kept as ENGAGE_AND_INVESTIGATE", carrier, morale);
	}
}

void ApplyPlantedBombTMoraleDisposition()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			Address pBot = GetEntityAddress(i);
			if (pBot == Address_Null || g_iOffset_Morale <= 0 || g_iOffset_Disposition <= 0)
				continue;

			int morale = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Morale), NumberType_Int32);
			if (morale <= -1)
			{
				StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), 1, NumberType_Int32); // OPPORTUNITY_FIRE
				if (g_iOffset_NoiseTimestamp > 0)
				{
					StoreToAddress(pBot + view_as<Address>(g_iOffset_NoiseTimestamp), view_as<int>(0.0), NumberType_Int32);
				}
				LogMessage("[BotRouteFix] [Module 12] Post-Plant T %N (Morale %d) -> OPPORTUNITY_FIRE (Site Camp)", i, morale);
			}
			else
			{
				StoreToAddress(pBot + view_as<Address>(g_iOffset_Disposition), 0, NumberType_Int32); // ENGAGE_AND_INVESTIGATE
				LogMessage("[BotRouteFix] [Module 12] Post-Plant T %N (Morale %d) -> ENGAGE_AND_INVESTIGATE (Dynamic Guard)", i, morale);
			}
		}
	}
}
