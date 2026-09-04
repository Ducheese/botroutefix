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
    ReplyToCommand(client, "[BotRouteFix] [Module 5] Attack Low-HP Retreat (<35 HP)    : %s (%d Retreats)",
        (bMaster && IsAttackRetreatHooked()) ? "ACTIVE" : "INACTIVE", GetTotalRetreatTriggered());
    ReplyToCommand(client, "[BotRouteFix] [Module 6] InvestigateNoise Audio Intercept  : %s (%d Blocked / %d Allowed)",
        (bMaster && IsInvestigateNoiseHooked()) ? "ACTIVE" : "INACTIVE", GetTotalNoiseIntercepted(), GetTotalNoiseAllowed());
    ReplyToCommand(client, "[BotRouteFix] ============================================================");
    return Plugin_Handled;
}
