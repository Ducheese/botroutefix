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
    RegAdminCmd("sm_brf_status", Command_Status, ADMFLAG_GENERIC, "View BotRouteFix module status and architecture info");
    RegAdminCmd("sm_brf_dangerdist", Command_DangerDist, ADMFLAG_GENERIC, "Dump per-area danger distribution to data/botroutefix_danger_<tag>.csv");
}

/**
 * 手动落盘命令：sm_brf_dangerdist [tag]（缺省 manual）
 *
 * 命令入口集中于此（跨模块调用 M3 落盘函数）；注册见 InitCommands。
 */
public Action Command_DangerDist(int client, int args)
{
    char tag[32];
    if (args >= 1)
    {
        GetCmdArg(1, tag, sizeof(tag));
    }
    else
    {
        strcopy(tag, sizeof(tag), "manual");
    }
    Module3_DumpDangerDist(tag, client);
    return Plugin_Handled;
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
 * 前半：统一模块启停行（纯启停，不带 live）。
 *   ACTIVE   = label + counts 挂尾；
 *   DEGRADED / FAILED = label + failed 点名居中 + counts 挂尾（最后一个管道符后）；
 *   INACTIVE = label + disabled by rule（无计数）。
 * counts 挂尾依据：armed（已武装生效）≠ ready（后端就绪），两数不一致时可看出
 * Prep 过了但 Apply/Enable 没落定的事项。
 *
 * @param client    接收方（0 = 服务端控制台）
 * @param index     模块编号 1-15
 * @param label     模块名（含静态士气公式，不含实时值）
 */
static void PrintModuleState(int client, int index, const char[] label)
{
    bool ruleEnabled = IsModuleEnabledByRule(index);
    ItemModuleState state = Items_GetModuleState(index, ruleEnabled);

    char stateName[16];
    Items_GetStateName(state, stateName, sizeof(stateName));

    if (!ruleEnabled)
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s | disabled by rule", index, stateName, label);
        return;
    }

    int total = Items_CountForModule(index);
    int ready = Items_ReadyForModule(index);
    int armed = Items_ArmedForModule(index);

    if (state == ItemModule_Active)
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s | items %d/%d armed (%d/%d ready)",
            index, stateName, label, armed, total, ready, total);
        return;
    }

    char failedNames[128];
    Items_FailedNamesForModule(index, failedNames, sizeof(failedNames));
    if (failedNames[0] != '\0')
    {
        ReplyToCommand(client, "[BotRouteFix] [Module %02d] %-8s | %s | failed: %s | items %d/%d armed (%d/%d ready)",
            index, stateName, label, failedNames, armed, total, ready, total);
    }
    else
    {
        // 防御分支：状态非 ACTIVE 却点不出 failed 名（事项漏注册/漏名字），行文法退化为纯 counts，
        // 另记一条 error 日志——status 里看不见，不污染定稿格式。
        LogError("[BotRouteFix] [Module %d] state is %s but no failed item names (items %d/%d armed). Check items.inc registration.",
            index, stateName, armed, total);
    }
}

/**
 * 后半 live 行门禁：只给规则开启且 ACTIVE/DEGRADED 的模块出 live 行。
 * FAILED（0 事项武装）与 INACTIVE（规则关闭）的模块没有可信实时数据，不出 live 行，
 * 避免“停运模块还在产出数据”的假象。
 */
static bool ModuleHasLive(int index)
{
    bool ruleEnabled = IsModuleEnabledByRule(index);
    ItemModuleState state = Items_GetModuleState(index, ruleEnabled);
    return ruleEnabled && (state == ItemModule_Active || state == ItemModule_Degraded);
}

/**
 * per-bot 概率名单（M01/M12/M13/M14）：整队分布口径，不筛存活，空队显示 []。
 * 模块门禁在调用点；偏移丢失时不出行。
 *
 * @param ctOnly      true = 只列 CT（如 M1 守点是 CT 逻辑）
 */
static void PrintChanceRoster(int client, int module, const char[] tag, bool ctOnly)
{
    if (g_iOffset_Morale <= 0)
    {
        return;
    }

    char tList[384], ctList[384];
    tList[0] = '\0';
    ctList[0] = '\0';
    for (int i = 1; i <= MaxClients; i++)
    {
        // 名单只列 bot：真人/观察者直接跳过（IsTeamBot 本来也含 IsFakeClient，
        // 这里显式前置，避免给非 bot 算概率再丢掉的空转）。
        if (!IsBot(i, false))
        {
            continue;
        }

        // M12 名单只收狙击手：步枪手拿不到 task 19，概率无意义，以 "--" 占位保槽位对齐。
        // 引擎真值走 IsSniper 总入口（人机通用）；到这里 i 已必为 bot。
        char tmp[16];
        if (module == 12 && !IsSniper(i))
        {
            strcopy(tmp, sizeof(tmp), "--");
        }
        else
        {
            float chance;
            if (module == 1)
            {
                chance = Module1_CalculateGuardBombsiteChance(GetBotMorale(i));
            }
            else if (module == 12)
            {
                chance = Module12_CalculateSniperCampChance(GetBotMorale(i));
            }
            else if (module == 13)
            {
                chance = Module13_CalculateSniperBuyChance(GetBotMorale(i));
            }
            else
            {
                chance = Module14_CalculateDefendRushChance(GetBotMorale(i));
            }

            Format(tmp, sizeof(tmp), "%.0f%%", chance);
        }
        if (!ctOnly && IsTeamBot(i, 2, false))
        {
            if (tList[0] != '\0')
            {
                StrCat(tList, sizeof(tList), ", ");
            }
            StrCat(tList, sizeof(tList), tmp);
        }
        else if (IsTeamBot(i, 3, false))
        {
            if (ctList[0] != '\0')
            {
                StrCat(ctList, sizeof(ctList), ", ");
            }
            StrCat(ctList, sizeof(ctList), tmp);
        }
    }

    if (ctOnly)
    {
        ReplyToCommand(client, "[BotRouteFix] [Live] M%02d | CT %s: [%s]", module, tag, ctList);
    }
    else
    {
        ReplyToCommand(client, "[BotRouteFix] [Live] M%02d | T %s: [%s] | CT %s: [%s]", module, tag, tList, tag, ctList);
    }
}

//========================================================================================
// COMMAND: STATUS TELEMETRY
//========================================================================================

/**
 * 服务端状态遥测（sm_brf_status），分两段：
 *   前半 = 16 模块启停状态（ACTIVE 光杆 counts，DEGRADED/FAILED 点名 failed 事项居中、
 *          counts 挂尾，INACTIVE 显示 disabled by rule；不带 live，不报 active 名单）；
 *   后半 = Live：全局快照（炸弹三态 + 双方士气两行，与任何模块开关无关）+
 *          各模块快照 live（仅 ACTIVE/DEGRADED 模块；per-bot 现算值不进 live，
 *          M01/M12/M13/M14 例外：per-bot 概率名单是整队分布快照，可以进）。
 * 静态士气公式写在模块名里（源码侧掷骰与插件侧计算一律进名）。
 * 口径声明：所有累积计数器自插件加载起单调递增，换图不清零（只随重载/重启清零）。
 * 签名依赖声明：炸弹携带/掉落/已安放走事件 + 武器槽扫描；包点 A/B 暂无内存来源，
 * 不显示；士气与概率名单强依赖 CCSBot_Morale_Offset，丢失时士气行显示 unavailable，
 * 概率名单行不出（不出假数据）。
 * 注：本命令显示内容与 sm_brf_debug 无关；debug 只控制各模块内部
 * DebugLog 与调试可视化。头部的 Debug: ON/OFF 仅为开关状态展示。
 */
public Action Command_Status(int client, int args)
{
    bool bDebug = (g_cvDebug != null && g_cvDebug.BoolValue);

    ReplyToCommand(client, "[BotRouteFix] ================= System Status Telemetry ==================");
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

    // 前半：16 模块启停状态（只报 failed 事项名居中、counts 挂尾；静态士气公式在名字里）。
    PrintModuleState(client, 1, "CT Guard Chance (70-10*M) / Anti-Rush");
    PrintModuleState(client, 2, "T Plant Clock (30+10*M) / Site Lock");
    PrintModuleState(client, 3, "NavArea Danger Soft-Decay");
    PrintModuleState(client, 4, "Hunt Target Claim / 45s Bypass");
    PrintModuleState(client, 5, "Combat Dodge / Reload Hide / Retreat");
    PrintModuleState(client, 6, "Noise Interception & OPPO Guard (M<0/500/1000/2000/3000)");
    PrintModuleState(client, 7, "T Carrier Escort & Pointman (3-M)");
    PrintModuleState(client, 8, "Polite Queue Breaker / Deadlock Hunt");
    PrintModuleState(client, 9, "T Loose-Bomb Fetch Roster & OPPO Guard");
    PrintModuleState(client, 10, "CT Defuse Roster");
    PrintModuleState(client, 11, "Sniper Corridor Deterrence");
    PrintModuleState(client, 12, "Sniper Camp (60-10*M)");
    PrintModuleState(client, 13, "Sniper Buy Chance (50+10*M)");
    PrintModuleState(client, 14, "Defend Rush Chance (40-10*M)");
    PrintModuleState(client, 15, "Hide Duration Scaling x(1-0.15*M)");
    PrintModuleState(client, 16, "Escape Knife-to-Gun on Contact");

    // Live 分隔行与上分隔行等长（总长 74）：前缀 + 27×- + " Live " + 27×-。
    ReplyToCommand(client, "[BotRouteFix] --------------------------- Live ---------------------------");

    // 后半全局快照：炸弹三态、士气两行，与任何模块开关无关（M2 关闭也不消失）。
    if (g_bBombPlanted)
    {
        ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: PLANTED");
    }
    else
    {
        int bombCarrier = GetC4Carrier();
        if (IsValidClient(bombCarrier, true))
        {
            if (IsBot(bombCarrier, false))
            {
                int carrierMorale = GetBotMorale(bombCarrier);
                char carrierMoraleName[16];
                GetMoraleName(carrierMorale, carrierMoraleName, sizeof(carrierMoraleName));
                ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: CARRIED by %N (Morale: %s%d %s)",
                    bombCarrier, (carrierMorale >= 0) ? "+" : "", carrierMorale, carrierMoraleName);
            }
            else
            {
                int teamMorale = GetEffectiveCarrierMorale(bombCarrier);
                char teamMoraleName[16];
                GetMoraleName(teamMorale, teamMoraleName, sizeof(teamMoraleName));
                ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: CARRIED by %N (Human, Team Morale: %s%d %s)",
                    bombCarrier, (teamMorale >= 0) ? "+" : "", teamMorale, teamMoraleName);
            }
        }
        else
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] Bomb: DROPPED");
        }
    }

    // 后半士气两行：第一行 AVG 只统计存活；第二行 per-bot 名单不筛存活
    // （阵亡者冻结值也列入，名单长度可能大于 alive 数，属预期口径差）。
    // 强依赖 CCSBot_Morale_Offset：丢失时两行合并为一行 unavailable，不输出假数据。
    if (g_iOffset_Morale > 0)
    {
        int tAlive = 0, ctAlive = 0;
        for (int i = 1; i <= MaxClients; i++)
        {
            if (IsTeamBot(i, 2, true))
            {
                tAlive++;
            }
            else if (IsTeamBot(i, 3, true))
            {
                ctAlive++;
            }
        }

        char tAvgStr[16], ctAvgStr[16];
        if (tAlive > 0)
        {
            int tAvg = GetTeamAverageBotMorale(2);
            char tAvgName[16];
            GetMoraleName(tAvg, tAvgName, sizeof(tAvgName));
            Format(tAvgStr, sizeof(tAvgStr), "%s%d %s", (tAvg >= 0) ? "+" : "", tAvg, tAvgName);
        }
        else
        {
            strcopy(tAvgStr, sizeof(tAvgStr), "--");
        }
        if (ctAlive > 0)
        {
            int ctAvg = GetTeamAverageBotMorale(3);
            char ctAvgName[16];
            GetMoraleName(ctAvg, ctAvgName, sizeof(ctAvgName));
            Format(ctAvgStr, sizeof(ctAvgStr), "%s%d %s", (ctAvg >= 0) ? "+" : "", ctAvg, ctAvgName);
        }
        else
        {
            strcopy(ctAvgStr, sizeof(ctAvgStr), "--");
        }
        ReplyToCommand(client, "[BotRouteFix] [Live] T %d bots alive (AVG Morale: %s) | CT %d bots alive (AVG Morale: %s)",
            tAlive, tAvgStr, ctAlive, ctAvgStr);

        char tMoraleList[384], ctMoraleList[384];
        tMoraleList[0] = '\0';
        ctMoraleList[0] = '\0';
        for (int j = 1; j <= MaxClients; j++)
        {
            char tmp[16];
            Format(tmp, sizeof(tmp), "%s%d", (GetBotMorale(j) >= 0) ? "+" : "", GetBotMorale(j));
            if (IsTeamBot(j, 2, false))
            {
                if (tMoraleList[0] != '\0')
                {
                    StrCat(tMoraleList, sizeof(tMoraleList), ", ");
                }
                StrCat(tMoraleList, sizeof(tMoraleList), tmp);
            }
            else if (IsTeamBot(j, 3, false))
            {
                if (ctMoraleList[0] != '\0')
                {
                    StrCat(ctMoraleList, sizeof(ctMoraleList), ", ");
                }
                StrCat(ctMoraleList, sizeof(ctMoraleList), tmp);
            }
        }
        ReplyToCommand(client, "[BotRouteFix] [Live] T Morale: [%s] | CT Morale: [%s]", tMoraleList, ctMoraleList);
    }
    else
    {
        ReplyToCommand(client, "[BotRouteFix] [Live] Morale: unavailable (CCSBot_Morale_Offset missing)");
    }

    char live[160];

    // M01 名单归属 GUARD 事项（M01 双事项，但只有 GUARD 有 live）。
    if (ModuleHasLive(1) && Item_IsArmed(ITEM_M1_GUARD))
    {
        PrintChanceRoster(client, 1, "Guard", true);
    }

    // M02：包点锁 + 当前运包延迟（CLOCK 未武装但锁仍在时标注；per-bot 士气值不进 live）。
    // 约束：locked 蕴含 LOCK 已武装（锁状态只由锁补丁置位）。
    if (ModuleHasLive(2))
    {
        if (Item_IsArmed(ITEM_M2_CLOCK))
        {
            if (Module2_IsBombsiteLocked())
            {
                Format(live, sizeof(live), "LOCKED Zone #%d | delay %.1fs",
                    Module2_GetLockedBombsiteIndex(), Module2_GetCurrentPlantDelay());
            }
            else
            {
                Format(live, sizeof(live), "NATIVE GetClosestZone | delay %.1fs",
                    Module2_GetCurrentPlantDelay());
            }
        }
        else if (Module2_IsBombsiteLocked())
        {
            Format(live, sizeof(live), "LOCKED Zone #%d (clock disabled)",
                Module2_GetLockedBombsiteIndex());
        }
        else
        {
            Format(live, sizeof(live), "NATIVE GetClosestZone (clock disabled)");
        }
        ReplyToCommand(client, "[BotRouteFix] [Live] M02 | %s", live);
    }

    if (ModuleHasLive(3))
    {
        Format(live, sizeof(live), "%d areas tracked", Module3_GetTotalNavAreaCount());
        ReplyToCommand(client, "[BotRouteFix] [Live] M03 | %s", live);
    }

    // M04 认领数归属 HUNT 事项（M04 双事项，但只有 HUNT 有 live）。
    if (ModuleHasLive(4) && Item_IsArmed(ITEM_M4_HUNT))
    {
        Format(live, sizeof(live), "total %d claims", Module4_GetTotalHuntClaims());
        ReplyToCommand(client, "[BotRouteFix] [Live] M04 | %s", live);
    }

    // M05：只列已武装事项的计数器（reload-hide 事项无计数器；无可列时不出行）。
    if (ModuleHasLive(5))
    {
        live[0] = '\0';
        if (Item_IsArmed(ITEM_M5_DODGE))
        {
            Format(live, sizeof(live), "dodge total %d forced", Module5_GetTotalDodgeForced());
        }
        if (Item_IsArmed(ITEM_M5_RETREAT))
        {
            if (live[0] != '\0')
            {
                StrCat(live, sizeof(live), " | ");
            }
            char m5LiveTmp[64];
            Format(m5LiveTmp, sizeof(m5LiveTmp), "retreat total %d triggered", Module5_GetTotalRetreatTriggered());
            StrCat(live, sizeof(live), m5LiveTmp);
        }
        if (live[0] != '\0')
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] M05 | %s", live);
        }
    }

    // M06：只放聚合计数器，per-bot 拦截/放行是逐行日志的事，不进 live。
    if (ModuleHasLive(6))
    {
        Format(live, sizeof(live), "total %d blocked, %d allowed, %d oppo-guarded",
            Module6_GetTotalNoiseIntercepted(), Module6_GetTotalNoiseAllowed(),
            Module6_GetTotalOppoGuarded());
        ReplyToCommand(client, "[BotRouteFix] [Live] M06 | %s", live);
    }

    // M07：当前名册快照（持包人 + 保镖数 + 先锋）。单事项模块，ModuleHasLive 已足够；
    // 护送事项坏了即模块 FAILED，本来就不出行。
    if (ModuleHasLive(7))
    {
        int carrier = Module7_GetCarrier();
        int pointman = Module7_GetPointman();
        if (IsValidClient(carrier, true))
        {
            if (IsValidClient(pointman, true))
            {
                Format(live, sizeof(live), "%d/%d guards, pointman %N, carrier %N",
                    Module7_GetBodyguardCount(), Module7_GetTargetCount(), pointman, carrier);
            }
            else
            {
                Format(live, sizeof(live), "%d/%d guards, carrier %N",
                    Module7_GetBodyguardCount(), Module7_GetTargetCount(), carrier);
            }
        }
        else
        {
            Format(live, sizeof(live), "%d/%d guards, no carrier",
                Module7_GetBodyguardCount(), Module7_GetTargetCount());
        }
        ReplyToCommand(client, "[BotRouteFix] [Live] M07 | %s", live);
    }

    // M08：双事项，只列已武装事项的计数器（M05 同款拼法，无特例）。
    if (ModuleHasLive(8))
    {
        live[0] = '\0';
        if (Item_IsArmed(ITEM_M8_QUEUE))
        {
            Format(live, sizeof(live), "total %d shattered", Module8_GetTotalQueueShattered());
        }
        if (Item_IsArmed(ITEM_M8_DEADLOCK))
        {
            if (live[0] != '\0')
            {
                StrCat(live, sizeof(live), " | ");
            }
            char m8LiveTmp[64];
            Format(m8LiveTmp, sizeof(m8LiveTmp), "total %d deadlock hunts", Module8_GetTotalDeadlockHunt());
            StrCat(live, sizeof(live), m8LiveTmp);
        }
        if (live[0] != '\0')
        {
            ReplyToCommand(client, "[BotRouteFix] [Live] M08 | %s", live);
        }
    }

    if (ModuleHasLive(9))
    {
        Format(live, sizeof(live), "%d/%d roster | total %d elected, %d gated, %d oppo-guarded",
            Module9_GetRosterCount(), Module9_GetRosterSize(),
            Module9_GetTotalElections(), Module9_GetTotalGated(),
            Module9_GetTotalFetchGuarded());
        ReplyToCommand(client, "[BotRouteFix] [Live] M09 | %s", live);
    }

    if (ModuleHasLive(10))
    {
        Format(live, sizeof(live), "%d/%d roster | total %d elected, %d gated",
            Module10_GetRosterCount(), Module10_GetRosterSize(),
            Module10_GetTotalElections(), Module10_GetTotalGated());
        ReplyToCommand(client, "[BotRouteFix] [Live] M10 | %s", live);
    }

    if (ModuleHasLive(11))
    {
        Format(live, sizeof(live), "total %d requests", Module11_GetTotalSniperRequests());
        ReplyToCommand(client, "[BotRouteFix] [Live] M11 | %s", live);
    }

    // M12：重定向计数器 + 两队逐 bot 留守理论骰子（per-bot 重骰本身是逐行日志的事）。
    if (ModuleHasLive(12))
    {
        Format(live, sizeof(live), "total %d redirected", Module12_GetTotalSniperCampRedirected());
        ReplyToCommand(client, "[BotRouteFix] [Live] M12 | %s", live);
    }
    if (ModuleHasLive(12))
    {
        PrintChanceRoster(client, 12, "SniperCamp", false);
    }

    // M13：两队逐 bot 买狙理论骰子（引擎内掷骰，名单是整队分布快照）。
    if (ModuleHasLive(13))
    {
        PrintChanceRoster(client, 13, "Buy", false);
    }

    // M14：两队逐 bot 前沿理论骰子（引擎内掷骰，名单是整队分布快照）。
    if (ModuleHasLive(14))
    {
        PrintChanceRoster(client, 14, "Rush", false);
    }

    // M15：只放聚合计数器；per-bot 时长缩放是逐行日志的事，不进 live。
    if (ModuleHasLive(15))
    {
        Format(live, sizeof(live), "total %d rescaled, %d short",
            Module15_GetTotalHideRescaled(), Module15_GetTotalShortHides());
        ReplyToCommand(client, "[BotRouteFix] [Live] M15 | %s", live);
    }

    if (ModuleHasLive(16))
    {
        Format(live, sizeof(live), "total %d drawn, %d contact, %d suppressed",
            Module16_GetTotalEscapeArmed(), Module16_GetTotalEscapeContactArmed(),
            Module16_GetTotalEscapeSuppressed());
        ReplyToCommand(client, "[BotRouteFix] [Live] M16 | %s", live);
    }

    ReplyToCommand(client, "[BotRouteFix] ============================================================");
    return Plugin_Handled;
}
