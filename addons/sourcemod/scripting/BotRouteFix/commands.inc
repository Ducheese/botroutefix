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
    RegConsoleCmd("sm_botroutefix_status", Command_Status, "View BotRouteFix module status and architecture info");
}

/**
 * 服务端状态遥测（sm_botroutefix_status），常显 + debug 专属 [Live] 两段：
 *   常显段 = 静态基线（版本/开关/补丁挂载态）+ 累积计数器；
 *   [Live] 段（bDebug 开）= 瞬时战术上下文（包匪/炸弹/士气）。
 * 口径声明：所有累积计数器自插件加载起单调递增，换图不清零（只随重载/重启清零）。
 */
public Action Command_Status(int client, int args)
{
    bool bMaster = (g_cvEnable != null && g_cvEnable.BoolValue);
    bool bDebug  = (g_cvDebug != null && g_cvDebug.BoolValue);

    ReplyToCommand(client, "[BotRouteFix] ==================== System Status Telemetry ====================");
    ReplyToCommand(client, "[BotRouteFix] Version       : %s | Author: %s", PLUGIN_VERSION, PLUGIN_AUTHOR);
    ReplyToCommand(client, "[BotRouteFix] Master Switch : %s | Debug : %s", bMaster ? "ENABLED" : "DISABLED", bDebug ? "ON" : "OFF");
    ReplyToCommand(client, "[BotRouteFix] Architecture  : %s", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (v91/v92)");
    ReplyToCommand(client, "[BotRouteFix] Gamedata      : %s (%s)", GAMEDATA, (g_hGameConf != null) ? "Ready" : "Not Loaded");
    ReplyToCommand(client, "[BotRouteFix] ------------------------------------------------------------");
    ReplyToCommand(client, "[BotRouteFix] [Module 1] CT Guard Site Chance (70-10*M) : %s",
        (bMaster && IsGuardBombsitePatched()) ? "ACTIVE" : "INACTIVE");
    ReplyToCommand(client, "[BotRouteFix] [Module 1] CT Defense Rush NOP (Anti-Rush): %s",
        (bMaster && IsDefenseRushPatched()) ? "ACTIVE" : "INACTIVE");

    char lockStatus[64];
    if (bMaster && IsBombsiteLocked())
    {
        Format(lockStatus, sizeof(lockStatus), "LOCKED (Zone #%d)", GetLockedBombsiteIndex());
    }
    else
    {
        strcopy(lockStatus, sizeof(lockStatus), "NATIVE (GetClosestZone)");
    }
    ReplyToCommand(client, "[BotRouteFix] [Module 2] T Bombsite Selection Mode        : %s", lockStatus);
    ReplyToCommand(client, "[BotRouteFix] [Module 2] T Carrier Morale Plant Delay     : %.1fs", GetCurrentPlantDelay());
    ReplyToCommand(client, "[BotRouteFix] [Module 3] NavArea Danger 80%% Soft-Decay   : %s (%d Areas Tracked)",
        bMaster ? "ACTIVE" : "INACTIVE", GetTotalNavAreaCount());
    ReplyToCommand(client, "[BotRouteFix] [Module 4] Hunt Oldest-Scan (Random NOP)     : %s",
        (bMaster && IsHuntRandomBypassed()) ? "ACTIVE" : "INACTIVE");
    ReplyToCommand(client, "[BotRouteFix] [Module 4] Hunt Target Claim & 45s Bypass   : %s (%d Claims)",
        bMaster ? "ACTIVE" : "INACTIVE", GetTotalHuntClaims());
    ReplyToCommand(client, "[BotRouteFix] [Module 5] Attack Dodge 100%% Forced         : %s (%d Dodges)",
        (bMaster && IsAttackDodgeHooked()) ? "ACTIVE" : "INACTIVE", GetTotalDodgeForced());
    ReplyToCommand(client, "[BotRouteFix] [Module 5] Reload Hide 100%% Patch           : %s",
        (bMaster && IsReloadHidePatched()) ? "ACTIVE" : "INACTIVE");
    ReplyToCommand(client, "[BotRouteFix] [Module 5] Attack Forced Retreat (Low-HP/Outnumbered): %s (%d Retreats)",
        (bMaster && IsAttackRetreatHooked()) ? "ACTIVE" : "INACTIVE", GetTotalRetreatTriggered());
    ReplyToCommand(client, "[BotRouteFix] [Module 6] InvestigateNoise Audio Intercept  : %s (%d Blocked / %d Allowed)",
        (bMaster && IsInvestigateNoiseHooked()) ? "ACTIVE" : "INACTIVE", GetTotalNoiseIntercepted(), GetTotalNoiseAllowed());
    ReplyToCommand(client, "[BotRouteFix] [Module 7] Carrier Escort Roster          : %d/%d (Carrier #%d)",
        Escort_GetBodyguardCount(), Escort_GetTargetCount(), Escort_GetCarrier());

    char queueStatus[64];
    if (bMaster && IsQueueBreakerHooked())
    {
        Format(queueStatus, sizeof(queueStatus), "ACTIVE (%d Shattered)", GetTotalQueueShattered());
    }
    else
    {
        strcopy(queueStatus, sizeof(queueStatus), "INACTIVE (Pending Signature)");
    }
    ReplyToCommand(client, "[BotRouteFix] [Module 8] Polite Queue Breaker (1.2s)    : %s", queueStatus);

    char lootStatus[96];
    if (bMaster && IsLootHooked())
    {
        Format(lootStatus, sizeof(lootStatus), "ACTIVE (%d/3 roster, %d elections, %d gated, %d CT-denied)", Loot_GetRosterCount(), GetTotalLootElections(), GetTotalLootGated(), GetTotalCTDenied());
    }
    else
    {
        strcopy(lootStatus, sizeof(lootStatus), "INACTIVE (Hook Missing)");
    }
    ReplyToCommand(client, "[BotRouteFix] [Module 9] T Loose-Bomb Fetch Roster (Top3): %s", lootStatus);

    char defuseStatus[64];
    if (bMaster && IsDefuseHooked())
    {
        int chief = GetChiefDefuser();
        if (chief != -1)
        {
            Format(defuseStatus, sizeof(defuseStatus), "ACTIVE (Chief %N, %d chiefs, %d gated)", chief, GetTotalChiefs(), GetTotalScenarioGated());
        }
        else
        {
            Format(defuseStatus, sizeof(defuseStatus), "ACTIVE (no chief, %d chiefs, %d gated)", GetTotalChiefs(), GetTotalScenarioGated());
        }
    }
    else
    {
        strcopy(defuseStatus, sizeof(defuseStatus), "INACTIVE (Pending Signature)");
    }
    ReplyToCommand(client, "[BotRouteFix] [Module 10] CT Chief Defuser (Scenario)    : %s", defuseStatus);

    char sniperStatus[64];
    if (bMaster && IsSniperDeterrenceHooked())
    {
        Format(sniperStatus, sizeof(sniperStatus), "ACTIVE (%d deterred)", GetTotalSniperDeterred());
    }
    else
    {
        strcopy(sniperStatus, sizeof(sniperStatus), "INACTIVE (Pending Signature)");
    }
    ReplyToCommand(client, "[BotRouteFix] [Module 11] Sniper Corridor Danger & Retreat: %s", sniperStatus);

    if (bDebug)
    {
        int liveCarrier = Escort_GetCarrier();
        if (liveCarrier > 0)
        {
            char liveMoraleStr[16];
            int liveMorale = GetBotMorale(liveCarrier);
            GetMoraleName(liveMorale, liveMoraleStr, sizeof(liveMoraleStr));
            ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: %N (Morale: %d %s) | Escorts %d/%d | Plant %.1fs",
                liveCarrier, liveMorale, liveMoraleStr, Escort_GetBodyguardCount(), Escort_GetTargetCount(), GetCurrentPlantDelay());
        }
        else
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: -- (no C4 in play)");
        }
        ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: %s | Morale AVG T %d / CT %d",
            g_bBombPlanted ? "PLANTED" : "NOT PLANTED", GetTeamAverageBotMorale(2), GetTeamAverageBotMorale(3));
        ReplyToCommand(client, "[BotRouteFix] [Live] CT Guard AVG: %.0f%% (70-10*M)",
            CalculateGuardBombsiteChance(GetTeamAverageBotMorale(3)));
    }
    ReplyToCommand(client, "[BotRouteFix] ============================================================");
    return Plugin_Handled;
}
