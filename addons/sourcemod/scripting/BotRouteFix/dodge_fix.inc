#if defined _botroutefix_dodge_fix_included
 #endinput
#endif
#define _botroutefix_dodge_fix_included

//========================================================================================
// MODULE 9: AttackState Dodge 热补丁 80*skill -> 100% 强制闪避
// 通过 DHook AttackState::OnEnter Post 重算 m_shouldDodge，全员 100% 进入 Dodge()
// 原逻辑: dodgeChance = 80*skill (0-80%) -> 新: 强制 1，后续灵敏度在 Dodge() 内部再按 skill 分化
// 偏移: CCSBot_Profile_Offset(6436/7512) -> BotProfile*, Profile_Skill_Offset(8/12) -> float skill, AttackState_ShouldDodge_Offset(50/58) -> bool
//========================================================================================

void PrepAttackDodgeHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hAttackDodgeDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
	if (g_hAttackDodgeDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 9] Failed to create DHook detour for AttackState_OnEnter!");
		return;
	}

	if (!DHookSetFromConf(g_hAttackDodgeDetour, gc, SDKConf_Signature, "AttackState_OnEnter"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 9] Failed to find signature for AttackState_OnEnter in gamedata!");
		return;
	}

	DHookAddParam(g_hAttackDodgeDetour, HookParamType_CBaseEntity);

	if (!DHookEnableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 9] Failed to enable post detour for AttackState_OnEnter!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 9] Hooked AttackState::OnEnter Post (Dodge forced 100%%)");
	delete gc;
}

void RestoreAttackDodgeHook()
{
	if (g_hAttackDodgeDetour != null)
	{
		DHookDisableDetour(g_hAttackDodgeDetour, true, Hook_AttackState_OnEnter_Post);
		delete g_hAttackDodgeDetour;
		g_hAttackDodgeDetour = null;
	}
}

public MRESReturn Hook_AttackState_OnEnter_Post(Address pThis, DHookParam hParams)
{
	if (pThis == Address_Null)
		return MRES_Ignored;

	int client = hParams.Get(1);
	if (client <= 0 || client > MaxClients || !IsClientInGame(client))
		return MRES_Ignored;

	if (g_iOffset_AttackShouldDodge <= 0)
		return MRES_Ignored;

	StoreToAddress(pThis + view_as<Address>(g_iOffset_AttackShouldDodge), 1, NumberType_Int8);

	LogMessage("[BotRouteFix] [Dodge] %N (%d) forced shouldDodge=1 [atkState %08x+%d]",
		client, client, view_as<int>(pThis), g_iOffset_AttackShouldDodge);

	return MRES_Ignored;
}
