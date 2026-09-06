//========================================================================================
// BotRouteFix - CS:S CSBot 决策链与战术寻路重构框架
//
// 模块化架构：
//   - BotRouteFix/global.inc          : 全局常量、宏定义、环境架构识别与公共状态
//   - BotRouteFix/memory.inc          : 底层内存安全补丁抽象与字节序列写入管理 (MemoryPatch)
//   - BotRouteFix/helpers.inc         : Bot 实体判定、Morale 属性与通用内存数据读取
//   - BotRouteFix/sdkcalls.inc        : 引擎级虚函数/静态函数底层调用封装 (SDKCall)
//   - BotRouteFix/nav_measure.inc     : 真实地面行走距离无损测量子系统 (NavMeasure)
//   - BotRouteFix/events.inc          : 游戏事件监听与多模块生命周期同步分发 (Events Hub)
//   - BotRouteFix/commands.inc        : 控制台指令、在线遥测与模块调试工具
//   - BotRouteFix/module1_defense.inc : [模块 1] CT 守点概率重构 (70-10*M) 与防前冲 (Anti-Rush)
//   - BotRouteFix/module2_bombsite.inc: [模块 2] T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
//   - BotRouteFix/module3_danger.inc  : [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
//   - BotRouteFix/module4_hunt.inc    : [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
//   - BotRouteFix/module5_combat.inc  : [模块 5] 战斗决策重构 (AttackState 闪避/换弹、残血、一打多强制后撤)
//   - BotRouteFix/module6_tmorale.inc : [模块 6] T 阵营士气接战策略与守包听觉出击梯度 (TMorale)
//   - BotRouteFix/module7_escort.inc  : [模块 7] T 包匪贴身保镖花名册 (Carrier Escort Roster)
//   - BotRouteFix/module8_queue.inc   : [模块 8] 礼貌排队打碎与门口掉头 (Queue Breaker, UpdatePathMovement Post)
//   - BotRouteFix/module9_loot.inc    : [模块 9] 掉包 Top3 抢包花名册 (Loot Roster, NoticeLooseBomb Pre)
//   - BotRouteFix/module10_defuse.inc : [模块 10] CT 首席拆包员 (Chief Defuser, IsDoingScenario Pre)
//   - BotRouteFix/module11_sniper.inc : [模块 11] 狙击手威慑与走廊加压 (Sniper Deterrence & Corridor Danger)
//========================================================================================

#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#include <cstrike>
#include <dhooks>

#include "BotRouteFix/global"
#include "BotRouteFix/memory"
#include "BotRouteFix/helpers"
#include "BotRouteFix/sdkcalls"
#include "BotRouteFix/nav_measure"
#include "BotRouteFix/module1_defense"
#include "BotRouteFix/module2_bombsite"
#include "BotRouteFix/module3_danger"
#include "BotRouteFix/module4_hunt"
#include "BotRouteFix/module5_combat"
#include "BotRouteFix/module6_tmorale"
#include "BotRouteFix/module7_escort"
#include "BotRouteFix/module8_queue"
#include "BotRouteFix/module9_loot"
#include "BotRouteFix/module10_defuse"
#include "BotRouteFix/module11_sniper"
#include "BotRouteFix/events"
#include "BotRouteFix/commands"

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo =
{
    name        = PLUGIN_NAME,
    author      = PLUGIN_AUTHOR,
    description = PLUGIN_DESCRIPTION,
    version     = PLUGIN_VERSION,
    url         = PLUGIN_URL
};

//========================================================================================
// LIFECYCLE
//========================================================================================

public void OnPluginStart()
{
    LogMessage("[BotRouteFix] ============== Initialization Start ==============");

    // 1. Register core ConVars
    CreateConVar("sm_botroutefix_version", PLUGIN_VERSION, "BotRouteFix plugin version", FCVAR_NOTIFY | FCVAR_DONTRECORD);
    g_cvEnable = CreateConVar("sm_botroutefix_enable", "1", "Enable or disable BotRouteFix core tactical overhaul", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvDebug  = CreateConVar("sm_botroutefix_debug",  "1", "Enable or disable verbose debug telemetry output", FCVAR_NONE, true, 0.0, true, 1.0);

    // 2. Load Gamedata and detect architecture
    g_hGameConf = LoadGameConfigFile(GAMEDATA);
    if (g_hGameConf == null)
    {
        SetFailState("[BotRouteFix] Fatal Error: Failed to load gamedata file %s.txt!", GAMEDATA);
        return;
    }

    // Determine 32-bit vs 64-bit via Gamedata (windows = 0, windows64 = 1)
    g_bIsWin64 = (GameConfGetOffset(g_hGameConf, "IsWin64") == 1);

    // Cache common offsets
    g_iOffset_Morale = GameConfGetOffset(g_hGameConf, "CCSBot_Morale_Offset");

    // 3. Register console commands and events hub
    InitCommands();
    InitEvents();

    // 4. Initialize reverse engineering infrastructure (fail-fast: escort needs Follow calls)
    if (!InitSDKCalls())
    {
        SetFailState("[BotRouteFix] Core SDKCall table failed to initialize! Aborting plugin startup.");
        return;
    }

    // 4b. NavMeasure soft init (standby on missing offsets, never fail-fast)
    InitNavMeasure();

    // 5. Initialize business modules (fail-fast: abort startup if any module fails)
    if (!InitModule1_Defense() || !InitModule2_Bombsite() || !InitModule3_Danger()
        || !InitModule4_Hunt() || !InitModule5_Combat() || !InitModule6_TMorale()
        || !InitModule7_Escort() || !InitModule8_Queue()
        || !InitModule9_Loot() || !InitModule10_Defuse()
        || !InitModule11_Sniper())
    {
        SetFailState("[BotRouteFix] One or more core tactical modules failed to initialize! Aborting plugin startup.");
        return;
    }

    // 6. Connect ConVar change hook and check initial state
    g_cvEnable.AddChangeHook(OnConVarChange_Enable);
    if (!g_cvEnable.BoolValue)
    {
        OnConVarChange_Enable(g_cvEnable, "1", "0");
    }

    LogMessage("[BotRouteFix] ========== Initialization Complete [%s] ==========",
        g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (v91/v92)");
}

public void OnPluginEnd()
{
    // Safely restore business modules in reverse order
    RestoreModule11_Sniper();
    RestoreModule10_Defuse();
    RestoreModule9_Loot();
    RestoreModule8_Queue();
    RestoreModule7_Escort();
    RestoreModule6_TMorale();
    RestoreModule5_Combat();
    RestoreModule4_Hunt();
    RestoreModule3_Danger();
    RestoreModule2_Bombsite();
    RestoreModule1_Defense();

    // Release infrastructure and events
    CleanupSDKCalls();
    CleanupEvents();

    if (g_hGameConf != null)
    {
        delete g_hGameConf;
        g_hGameConf = null;
    }

    LogMessage("[BotRouteFix] Plugin unloaded cleanly, all memory patches and hooks restored.");
}

public void OnConVarChange_Enable(ConVar convar, const char[] oldValue, const char[] newValue)
{
    bool bEnable = convar.BoolValue;
    LogMessage("[BotRouteFix] Core tactical overhaul %s by administrator.", bEnable ? "enabled" : "disabled");

    EnableModule1_Defense(bEnable);
    EnableModule2_Bombsite(bEnable);
    EnableModule3_Danger(bEnable);
    EnableModule4_Hunt(bEnable);
    EnableModule5_Combat(bEnable);
    EnableModule6_TMorale(bEnable);
    EnableModule7_Escort(bEnable);
    EnableModule8_Queue(bEnable);
    EnableModule9_Loot(bEnable);
    EnableModule10_Defuse(bEnable);
    EnableModule11_Sniper(bEnable);
}

public void OnMapStart()
{
    // 换图 = 实体索引全废 + nav 重建 + curtime 归零：各模块清脏状态（不清即跨图污染）
    // M1/M4/M8 无状态，M3 重解容器，M5 清时间数组，其余清名单/旗帜（与 round_start 同语义但独立命名）
    Module2_OnMapStart();
    Module3_OnMapStart();
    Module5_OnMapStart();
    Module6_OnMapStart();
    Module7_OnMapStart();
    Module9_OnMapStart();
    Module10_OnMapStart();
}

public void OnClientPutInServer(int client)
{
    // 槽位复用：重置 per-client 时间戳
    Module5_OnClientPutInServer(client);
}

public void OnClientDisconnect(int client)
{
    // 游戏事件 Hub 到不了 forward，直调各花名册除名/释放/接力与状态重置
    Module5_OnClientDisconnect(client);
    Module7_OnClientDisconnect(client);
    Module9_OnClientDisconnect(client);
    Module10_OnClientDisconnect(client);
}
