#if defined _botroutefix_health_retreat_included
 #endinput
#endif
#define _botroutefix_health_retreat_included

//========================================================================================
// MODULE 11: 血量过低强制后撤 (Low Health Forced Retreat via PinnedDown)
// Detour AttackState::OnUpdate Pre 写 m_pinnedDownTimestamp=0.0 使 isPinnedDown 恒真
// 原生：isPinnedDown = curtime > m_pinnedDownTimestamp( OnEnter+7~10s ) 触发 TryToRetreat
// 血检：health < 40 时预写 0.0，下一帧必走 341-360 TryToRetreat 同路，复用 3~15s 冷却与 IsLookingAtMe 校验
// 阈值：40 HP (对应 cs_bot_attack.cpp:341 m_pinnedDownTimestamp 比较)
// 偏移：AttackState_PinnedDownTimestamp_Offset(44/52) + MRetreatTimer(56/64) + CountdownTimer(8/12)
//========================================================================================

#define HEALTH_RETREAT_THRESHOLD 35

void PrepAttackRetreatHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hAttackRetreatDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
	if (g_hAttackRetreatDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 11] Failed to create DHook detour for AttackState_OnUpdate!");
		return;
	}

	if (!DHookSetFromConf(g_hAttackRetreatDetour, gc, SDKConf_Signature, "AttackState_OnUpdate"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 11] Failed to find signature for AttackState_OnUpdate in gamedata!");
		return;
	}

	DHookAddParam(g_hAttackRetreatDetour, HookParamType_CBaseEntity);

	if (!DHookEnableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 11] Failed to enable pre detour for AttackState_OnUpdate!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 11] Hooked AttackState::OnUpdate Pre (LowHealth -> PinnedDown forced retreat @ %d HP)", HEALTH_RETREAT_THRESHOLD);
	delete gc;
}

void RestoreAttackRetreatHook()
{
	if (g_hAttackRetreatDetour != null)
	{
		DHookDisableDetour(g_hAttackRetreatDetour, false, Hook_AttackState_OnUpdate);
		delete g_hAttackRetreatDetour;
		g_hAttackRetreatDetour = null;
	}
}

public MRESReturn Hook_AttackState_OnUpdate(Address pThis, DHookParam hParams)
{
	if (pThis == Address_Null)
		return MRES_Ignored;

	if (g_iOffset_PinnedDownTimestamp <= 0 || g_iOffset_MRetreatTimer <= 0)
		return MRES_Ignored;

	int client = hParams.Get(1);
	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client) || !IsFakeClient(client))
		return MRES_Ignored;

	if (GetClientHealth(client) >= HEALTH_RETREAT_THRESHOLD)
		return MRES_Ignored;

	// 1. 置 isPinnedDown 恒真 (curtime > 0.0)
	StoreToAddress(pThis + view_as<Address>(g_iOffset_PinnedDownTimestamp), view_as<int>(0.0), NumberType_Int32);

	// 2. 清 m_retreatTimer 冷却，使 334 IsElapsed 立即真，达血低立撤
	// CountdownTimer::m_timestamp @ MRetreatTimer + g_iOffset_TimerTimestamp (8/12)
	if (g_iOffset_TimerTimestamp > 0)
	{
		StoreToAddress(pThis + view_as<Address>(g_iOffset_MRetreatTimer + g_iOffset_TimerTimestamp), view_as<int>(0.0), NumberType_Int32);
		StoreToAddress(pThis + view_as<Address>(g_iOffset_MRetreatTimer + g_iOffset_TimerDuration), view_as<int>(0.0), NumberType_Int32);
	}

	return MRES_Ignored;
}
