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
// 唯一状态源 = items.inc 的 21 件事项能力表。
//   INACTIVE = 模块规则关闭
//   ACTIVE   = 全部事项武装
//   DEGRADED = 部分事项武装（仅多事项模块可能出现）
//   FAILED   = 0 个事项武装
// 底层 Is...Patched()/Is...Hooked() 只是事项后端的实现细节，不再直接拼状态词。
//========================================================================================

/**
 * 统一模块状态行
 *
 * @param client    接收方（0 = 服务端控制台）
 * @param index     模块编号 1-15
 * @param label     模块名
 * @param detail    模块专属遥测（可传空字符串）
 */
static void PrintModuleLine(int client, int index, const char[] label, const char[] detail)
{
    bool ruleEnabled = IsModuleEnabledByRule(index);
    ItemModuleState state = Items_GetModuleState(index, ruleEnabled);

    char stateName[16];
    Items_GetStateName(state, stateName, sizeof(stateName));

    int total = Items_CountForModule(index);
    int ready = Items_ReadyForModule(index);
    int armed = Items_ArmedForModule(index);

    char finalDetail[192];
    if (!ruleEnabled)
    {
        strcopy(finalDetail, sizeof(finalDetail), "disabled by rule");
    }
    else if (state != ItemModule_Active)
    {
        Format(finalDetail, sizeof(finalDetail), "items %d/%d armed (%d/%d ready)", armed, total, ready, total);

        char failedNames[128];
        Items_FailedNamesForModule(index, failedNames, sizeof(failedNames));
        if (failedNames[0] != '\0')
        {
            StrCat(finalDetail, sizeof(finalDetail), " | failed: ");
            StrCat(finalDetail, sizeof(finalDetail), failedNames);
        }

        // DEGRADED 仍可能有活跃事项在产出实时数据（例如 M2 包点锁已锁定），
        // 保留 detail；FAILED 是 0 事项武装，detail 一律不拼，避免宣传性假话。
        if (state == ItemModule_Degraded && detail[0] != '\0')
        {
            StrCat(finalDetail, sizeof(finalDetail), " | ");
            StrCat(finalDetail, sizeof(finalDetail), detail);
        }
    }
    else
    {
        strcopy(finalDetail, sizeof(finalDetail), detail);
    }

    if (finalDetail[0] != '\0')
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s | %s", index, stateName, label, finalDetail);
    }
    else
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s", index, stateName, label);
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

    char modulesRule[128];
    if (g_cvModules != null)
    {
        g_cvModules.GetString(modulesRule, sizeof(modulesRule));
    }
    else
    {
        strcopy(modulesRule, sizeof(modulesRule), "all");
    }
    ReplyToCommand(client, "[BotRouteFix] Modules Rule  : %s", modulesRule);
    ReplyToCommand(client, "[BotRouteFix] ------------------------------------------------------------");

    char detail[128];

    // M1 detail 按实际武装的事项生成，DEGRADED 时不再宣称失败的那一项。
    char m1Detail[64];
    m1Detail[0] = '\0';
    if (Item_IsArmed(ITEM_M1_GUARD))
    {
        StrCat(m1Detail, sizeof(m1Detail), "formula 70-10*M");
    }
    if (Item_IsArmed(ITEM_M1_ANTI_RUSH))
    {
        if (m1Detail[0] != '\0')
        {
            StrCat(m1Detail, sizeof(m1Detail), ", ");
        }
        StrCat(m1Detail, sizeof(m1Detail), "anti-rush NOP");
    }
    PrintModuleLine(client, 1, "CT Guard Chance & Anti-Rush", m1Detail);

    char m2Detail[128];
    if (Item_IsArmed(ITEM_M2_CLOCK))
    {
        if (Module2_IsBombsiteLocked())
        {
            Format(m2Detail, sizeof(m2Detail), "live: LOCKED Zone #%d (delay %.1fs)", Module2_GetLockedBombsiteIndex(), Module2_GetCurrentPlantDelay());
        }
        else
        {
            Format(m2Detail, sizeof(m2Detail), "live: NATIVE GetClosestZone (delay %.1fs)", Module2_GetCurrentPlantDelay());
        }
    }
    else if (Module2_IsBombsiteLocked())
    {
        Format(m2Detail, sizeof(m2Detail), "live: LOCKED Zone #%d (clock disabled)", Module2_GetLockedBombsiteIndex());
    }
    else
    {
        strcopy(m2Detail, sizeof(m2Detail), "clock disabled");
    }
    PrintModuleLine(client, 2, "T Plant Clock & Site Lock", m2Detail);

    Format(detail, sizeof(detail), "live: %d areas tracked", Module3_GetTotalNavAreaCount());
    PrintModuleLine(client, 3, "NavArea Danger Soft-Decay", detail);

    // M4 detail 按实际武装的事项生成：HuntState 失败时不再展示累计认领数，避免“失败事项还在产出数据”的假象。
    char m4Detail[128];
    m4Detail[0] = '\0';
    if (Item_IsArmed(ITEM_M4_NOP))
    {
        StrCat(m4Detail, sizeof(m4Detail), "random-overwrite NOP");
    }
    if (Item_IsArmed(ITEM_M4_HUNT))
    {
        if (m4Detail[0] != '\0')
        {
            StrCat(m4Detail, sizeof(m4Detail), ", ");
        }
        StrCat(m4Detail, sizeof(m4Detail), "HuntState writes");

        char m4Claims[48];
        Format(m4Claims, sizeof(m4Claims), " | total: %d claims", Module4_GetTotalHuntClaims());
        StrCat(m4Detail, sizeof(m4Detail), m4Claims);
    }
    PrintModuleLine(client, 4, "Hunt Target Claim & 45s Bypass", m4Detail);

    // M5 detail 按实际武装的事项生成：只展示已武装事项的计数器，避免失败事项的数据残留。
    char m5Detail[128];
    m5Detail[0] = '\0';
    if (Item_IsArmed(ITEM_M5_RELOAD))
    {
        StrCat(m5Detail, sizeof(m5Detail), "reload-hide NOP");
    }
    if (Item_IsArmed(ITEM_M5_DODGE))
    {
        if (m5Detail[0] != '\0')
        {
            StrCat(m5Detail, sizeof(m5Detail), ", ");
        }
        char m5Dodge[48];
        Format(m5Dodge, sizeof(m5Dodge), "dodge writes (total %d forced)", Module5_GetTotalDodgeForced());
        StrCat(m5Detail, sizeof(m5Detail), m5Dodge);
    }
    if (Item_IsArmed(ITEM_M5_RETREAT))
    {
        if (m5Detail[0] != '\0')
        {
            StrCat(m5Detail, sizeof(m5Detail), ", ");
        }
        char m5Retreat[48];
        Format(m5Retreat, sizeof(m5Retreat), "retreat (total %d triggered)", Module5_GetTotalRetreatTriggered());
        StrCat(m5Detail, sizeof(m5Detail), m5Retreat);
    }
    PrintModuleLine(client, 5, "Combat Dodge / Reload Hide / Retreat", m5Detail);

    Format(detail, sizeof(detail), "total: %d blocked, %d allowed", Module6_GetTotalNoiseIntercepted(), Module6_GetTotalNoiseAllowed());
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

    Format(detail, sizeof(detail), "total: %d requests", GetTotalSniperRequests());
    PrintModuleLine(client, 11, "Sniper Corridor Deterrence", detail);

    Format(detail, sizeof(detail), "total: %d redirected", GetTotalCampRedirected());
    PrintModuleLine(client, 12, "Sniper Camp Gate", detail);

    PrintModuleLine(client, 13, "Sniper Buy Chance", "formula 50-10*M, dynamic rifle dice");
    PrintModuleLine(client, 14, "Defend Rush Chance", "formula 40-10*M, 4 lea sites patched");

    Format(detail, sizeof(detail), "total: %d rescaled, %d short", GetTotalHideRescaled(), GetTotalShortHides());
    PrintModuleLine(client, 15, "Hide Duration x(1-0.15*M)", detail);

    ReplyToCommand(client, "[BotRouteFix] ------------------------------------------------------------");

    if (bDebug)
    {
        char m2PlantDelay[48];
        m2PlantDelay[0] = '\0';
        if (Item_IsArmed(ITEM_M2_CLOCK))
        {
            Format(m2PlantDelay, sizeof(m2PlantDelay), " | Plant delay %.1fs", Module2_GetCurrentPlantDelay());
        }

        // M7 名册是事项数据：武装时读名册缓存，未武装时读全局持包状态并标记 escort 不可用，
        // 避免展示 M7 停运后的 stale 名册。
        char m7Escorts[48];
        int liveCarrier;
        if (Item_IsArmed(ITEM_M7_ESCORT))
        {
            liveCarrier = Escort_GetCarrier();
            Format(m7Escorts, sizeof(m7Escorts), "%d/%d",
                Escort_GetBodyguardCount(), Escort_GetTargetCount());
        }
        else
        {
            liveCarrier = GetC4Carrier();
            strcopy(m7Escorts, sizeof(m7Escorts), "--/-- (escort disabled)");
        }

        if (IsValidClient(liveCarrier, true))
        {
            char liveMoraleStr[16];
            int liveMorale = GetEffectiveCarrierMorale(liveCarrier);
            GetMoraleName(liveMorale, liveMoraleStr, sizeof(liveMoraleStr));

            if (IsHumanPlayer(liveCarrier))
            {
                ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: %N (Human, Team Morale: %d %s) | Escorts %s%s",
                    liveCarrier, liveMorale, liveMoraleStr, m7Escorts, m2PlantDelay);
            }
            else
            {
                ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: %N (Morale: %d %s) | Escorts %s%s",
                    liveCarrier, liveMorale, liveMoraleStr, m7Escorts, m2PlantDelay);
            }
        }
        else
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] T Carrier: -- (no active carrier)");
        }

        int tMorale  = GetTeamAverageBotMorale(2);
        int ctMorale = GetTeamAverageBotMorale(3);

        char m1GuardChance[32];
        if (Item_IsArmed(ITEM_M1_GUARD))
        {
            Format(m1GuardChance, sizeof(m1GuardChance), "%.0f%%", Module1_CalculateGuardBombsiteChance(ctMorale));
        }
        else
        {
            strcopy(m1GuardChance, sizeof(m1GuardChance), "disabled");
        }

        char m13BuyChance[48];
        if (Item_IsArmed(ITEM_M13_BUY))
        {
            Format(m13BuyChance, sizeof(m13BuyChance), "T %.0f%% / CT %.0f%%",
                CalculateSniperBuyChance(tMorale), CalculateSniperBuyChance(ctMorale));
        }
        else
        {
            strcopy(m13BuyChance, sizeof(m13BuyChance), "disabled");
        }

        char m14RushChance[48];
        if (Item_IsArmed(ITEM_M14_DEFEND))
        {
            Format(m14RushChance, sizeof(m14RushChance), "T %.0f%% / CT %.0f%%",
                CalculateDefendRushChance(tMorale), CalculateDefendRushChance(ctMorale));
        }
        else
        {
            strcopy(m14RushChance, sizeof(m14RushChance), "disabled");
        }

        ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: %s | Morale AVG T %d / CT %d",
            g_bBombPlanted ? "PLANTED" : "NOT PLANTED", tMorale, ctMorale);
        ReplyToCommand(client, "[BotRouteFix] [Live] Dice @ AVG morale - CT Guard: %s | SniperBuy: %s | DefendRush: %s",
            m1GuardChance, m13BuyChance, m14RushChance);
    }

    ReplyToCommand(client, "[BotRouteFix] ============================================================");
    return Plugin_Handled;
}
