#if defined _botroutefix_commands_included
    #endinput
#endif
#define _botroutefix_commands_included

//========================================================================================
// CONSOLE COMMANDS & DEBUG TELEMETRY
//========================================================================================

/**
 * 注册控制台调试指令
 */
void InitCommands()
{
    RegAdminCmd("sm_botroutefix_status", Command_Status, ADMFLAG_GENERIC, "View BotRouteFix module status and architecture info");
}

//========================================================================================
// MODULE STATE ROUTING
//
// 权责分离的唯一分流点：
//   业务开启态 -> IsModuleX_Active()
//   底层补丁/挂载态 -> Is...Patched() / Is...Hooked()
// 两个路由函数各自覆盖全部 15 个模块，保证每个状态接口都有确切调用点。
//========================================================================================

/**
 * 业务开启态路由（每个 IsModuleX_Active 的确切调用点）
 */
static bool Module_IsBusinessActive(int index)
{
    switch (index)
    {
        case 1:  return IsModule1_Active();
        case 2:  return IsModule2_Active();
        case 3:  return IsModule3_Active();
        case 4:  return IsModule4_Active();
        case 5:  return IsModule5_Active();
        case 6:  return IsModule6_Active();
        case 7:  return IsModule7_Active();
        case 8:  return IsModule8_Active();
        case 9:  return IsModule9_Active();
        case 10: return IsModule10_Active();
        case 11: return IsModule11_Active();
        case 12: return IsModule12_Active();
        case 13: return IsModule13_Active();
        case 14: return IsModule14_Active();
        case 15: return IsModule15_Active();
    }
    return false;
}

/**
 * 底层补丁/挂载态路由（每个 Is...Patched / Is...Hooked 的确切调用点）
 *
 * 准则：底层依赖齐全且可用才返回 true (ACTIVE)，任何核心缺失均返回 false (DEGRADED)。
 * "核心" = 运行期可能变化的底层资源：补丁字节 / Detour 挂载 / SDKCall 句柄 / 全局单例指针。
 * Init 期已 fail-fast 校验过的 gamedata 偏移不在此列（运行中不可能失效，写进判定没有信息量）。
 */
static bool Module_IsBackendReady(int index)
{
    switch (index)
    {
        case 1:  return IsGuardBombsitePatched() && IsDefenseRushPatched();
        case 2:  return IsBombsitePatchReady();
        case 3:  return IsNavDangerReady();
        case 4:  return IsHuntStateHooked() && IsHuntRandomBypassed();
        case 5:  return IsReloadHidePatched() && IsAttackDodgeHooked() && IsAttackRetreatHooked();
        case 6:  return IsInvestigateNoiseHooked();
        case 7:  return IsEscortReady();
        case 8:  return IsQueueBreakerHooked();
        case 9:  return IsLootHooked() && BotNavReady();
        case 10: return IsDefuseHooked() && BotNavReady();
        case 11: return IsSniperDeterrenceHooked();
        case 12: return IsCamp12Ready() && IsHideDurationHooked();  // 借钩：宿主未启用则门禁无效
        case 13: return IsSniperBuy13Patched();
        case 14: return IsDefendRush14Patched();
        case 15: return IsHideDurationHooked();
    }
    return false;
}

/**
 * 统一模块状态行
 *
 *   ACTIVE   = 业务开启 + 底层补丁/钩子就绪
 *   DEGRADED = 业务开启但底层缺失（签名待机 / 外来补丁占位 / 启用失败）
 *   INACTIVE = 业务已关闭
 *
 * @param client    接收方（0 = 服务端控制台）
 * @param index     模块编号 1-15
 * @param label     模块名
 * @param detail    模块专属遥测（可传空字符串）
 */
static void PrintModuleLine(int client, int index, const char[] label, const char[] detail)
{
    bool bActive  = Module_IsBusinessActive(index);
    bool bBackend = Module_IsBackendReady(index);

    char state[16];
    if (!bActive)
    {
        strcopy(state, sizeof(state), "INACTIVE");
    }
    else if (!bBackend)
    {
        strcopy(state, sizeof(state), "DEGRADED");
    }
    else
    {
        strcopy(state, sizeof(state), "ACTIVE");
    }

    if (detail[0] != '\0')
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s | %s", index, state, label, detail);
    }
    else
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s", index, state, label);
    }
}

//========================================================================================
// COMMAND: STATUS TELEMETRY
//========================================================================================

/**
 * 服务端状态遥测（sm_botroutefix_status），常显 + debug 专属 [Live] 两段：
 *   常显段 = 15 模块统一状态行（业务态 + 底层态分流）+ 累积计数器；
 *   [Live] 段（bDebug 开）= 瞬时战术上下文（包匪/炸弹/士气/三条概率公式）。
 * 口径声明：所有累积计数器自插件加载起单调递增，换图不清零（只随重载/重启清零）。
 */
public Action Command_Status(int client, int args)
{
    bool bDebug = (g_cvDebug != null && g_cvDebug.BoolValue);

    ReplyToCommand(client, "[BotRouteFix] ================== System Status Telemetry ==================");
    ReplyToCommand(client, "[BotRouteFix] Version       : %s | Author: %s", PLUGIN_VERSION, PLUGIN_AUTHOR);
    ReplyToCommand(client, "[BotRouteFix] Debug         : %s | Architecture: %s",
        bDebug ? "ON" : "OFF", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (v91/v92)");
    ReplyToCommand(client, "[BotRouteFix] Gamedata      : %s (%s)", GAMEDATA, (g_hGameConf != null) ? "Ready" : "Not Loaded");
    ReplyToCommand(client, "[BotRouteFix] ------------------------------------------------------------");

    char detail[128];

    PrintModuleLine(client, 1, "CT Guard Chance & Anti-Rush", "formula 70-10*M, anti-rush NOP");

    if (IsBombsiteLocked())
    {
        Format(detail, sizeof(detail), "live: LOCKED Zone #%d (delay %.1fs)", GetLockedBombsiteIndex(), GetCurrentPlantDelay());
    }
    else
    {
        Format(detail, sizeof(detail), "live: NATIVE GetClosestZone (delay %.1fs)", GetCurrentPlantDelay());
    }
    PrintModuleLine(client, 2, "T Plant Clock & Site Lock", detail);

    Format(detail, sizeof(detail), "live: %d areas tracked", GetTotalNavAreaCount());
    PrintModuleLine(client, 3, "NavArea Danger Soft-Decay", detail);

    Format(detail, sizeof(detail), "total: %d claims", GetTotalHuntClaims());
    PrintModuleLine(client, 4, "Hunt Target Claim & 45s Bypass", detail);

    Format(detail, sizeof(detail), "total: %d dodges, %d retreats", GetTotalDodgeForced(), GetTotalRetreatTriggered());
    PrintModuleLine(client, 5, "Combat Dodge / Reload Hide / Retreat", detail);

    Format(detail, sizeof(detail), "total: %d blocked, %d allowed", GetTotalNoiseIntercepted(), GetTotalNoiseAllowed());
    PrintModuleLine(client, 6, "T Morale Disposition & Noise Gate", detail);

    int carrier = Escort_GetCarrier();
    if (IsValidClient(carrier, true))
    {
        Format(detail, sizeof(detail), "live: %d/%d guards, carrier %N",
            Escort_GetBodyguardCount(), Escort_GetTargetCount(), carrier);
    }
    else
    {
        Format(detail, sizeof(detail), "live: %d/%d guards, no carrier",
            Escort_GetBodyguardCount(), Escort_GetTargetCount());
    }
    PrintModuleLine(client, 7, "Carrier Escort Roster", detail);

    Format(detail, sizeof(detail), "total: %d shattered, %d deadlock hunts", GetTotalQueueShattered(), GetTotalDeadlockHunt());
    PrintModuleLine(client, 8, "Polite Queue Breaker", detail);

    Format(detail, sizeof(detail), "live: %d/%d roster | total: %d elected, %d gated",
        Loot_GetRosterCount(), Loot_GetRosterSize(), GetTotalLootElections(), GetTotalLootGated());
    PrintModuleLine(client, 9, "T Loose-Bomb Fetch Roster", detail);

    int chief = GetChiefDefuser();
    if (chief != -1)
    {
        Format(detail, sizeof(detail), "live: chief %N | total: %d elected, %d gated", chief, GetTotalChiefs(), GetTotalScenarioGated());
    }
    else
    {
        Format(detail, sizeof(detail), "live: none | total: %d elected, %d gated", GetTotalChiefs(), GetTotalScenarioGated());
    }
    PrintModuleLine(client, 10, "CT Chief Defuser", detail);

    Format(detail, sizeof(detail), "total: %d deterred", GetTotalSniperDeterred());
    PrintModuleLine(client, 11, "Sniper Corridor Deterrence", detail);

    Format(detail, sizeof(detail), "total: %d redirected", GetTotalCampRedirected());
    PrintModuleLine(client, 12, "Sniper Camp Gate", detail);

    PrintModuleLine(client, 13, "Sniper Buy Chance", "formula 50-10*M, dynamic rifle dice");
    PrintModuleLine(client, 14, "Defend Rush Chance", "formula 40-10*M, 4 lea sites patched");

    Format(detail, sizeof(detail), "total: %d rescaled", GetTotalHideRescaled());
    PrintModuleLine(client, 15, "Hide Duration x(1-0.15*M)", detail);

    ReplyToCommand(client, "[BotRouteFix] ------------------------------------------------------------");

    if (bDebug)
    {
        int liveCarrier = Escort_GetCarrier();
        if (IsValidClient(liveCarrier, true))
        {
            char liveMoraleStr[16];
            int liveMorale = GetEffectiveCarrierMorale(liveCarrier);
            GetMoraleName(liveMorale, liveMoraleStr, sizeof(liveMoraleStr));

            if (IsHumanPlayer(liveCarrier))
            {
                ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: %N (Human, Team Morale: %d %s) | Escorts %d/%d | Plant delay %.1fs",
                    liveCarrier, liveMorale, liveMoraleStr, Escort_GetBodyguardCount(), Escort_GetTargetCount(), GetCurrentPlantDelay());
            }
            else
            {
                ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: %N (Morale: %d %s) | Escorts %d/%d | Plant delay %.1fs",
                    liveCarrier, liveMorale, liveMoraleStr, Escort_GetBodyguardCount(), Escort_GetTargetCount(), GetCurrentPlantDelay());
            }
        }
        else
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: -- (no active carrier)");
        }

        int tMorale  = GetTeamAverageBotMorale(2);
        int ctMorale = GetTeamAverageBotMorale(3);

        ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: %s | Morale AVG T %d / CT %d",
            g_bBombPlanted ? "PLANTED" : "NOT PLANTED", tMorale, ctMorale);
        ReplyToCommand(client, "[BotRouteFix] [Live] Dice @ AVG morale - CT Guard: %.0f%% | SniperBuy: T %.0f%% / CT %.0f%% | DefendRush: T %.0f%% / CT %.0f%%",
            CalculateGuardBombsiteChance(ctMorale),
            CalculateSniperBuyChance(tMorale), CalculateSniperBuyChance(ctMorale),
            CalculateDefendRushChance(tMorale), CalculateDefendRushChance(ctMorale));
    }

    ReplyToCommand(client, "[BotRouteFix] ============================================================");
    return Plugin_Handled;
}
