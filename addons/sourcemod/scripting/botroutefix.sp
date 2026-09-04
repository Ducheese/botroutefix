//========================================================================================
// BotRouteFix - CS:S CSBot 决策链与战术寻路重构框架
//
// 模块化架构：
//   - BotRouteFix/global.inc          : 全局常量、宏定义、环境架构识别与公共状态
//   - BotRouteFix/memory.inc          : 底层内存安全补丁抽象与字节序列写入管理 (MemoryPatch)
//   - BotRouteFix/helpers.inc         : Bot 实体判定、Morale 属性与通用内存数据读取
//   - BotRouteFix/sdkcalls.inc        : 引擎级虚函数/静态函数底层调用封装 (SDKCall)
//   - BotRouteFix/events.inc          : 游戏事件监听与多模块生命周期同步分发 (Events Hub)
//   - BotRouteFix/commands.inc        : 控制台指令、在线遥测与模块调试工具
//   - BotRouteFix/module1_defense.inc : [模块 1] CT 守点概率重构 (70-10*M) 与防前冲 (Anti-Rush)
//   - BotRouteFix/module2_bombsite.inc: [模块 2] T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
//   - BotRouteFix/module3_danger.inc  : [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
//   - BotRouteFix/module4_hunt.inc    : [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
//   - BotRouteFix/module5_combat.inc  : [模块 5] 战斗决策重构 (AttackState 闪避 100% 强制与换弹进掩体 100% 强制)
//   - BotRouteFix/module6_tmorale.inc : [模块 6] T 阵营士气接战策略与守包听觉出击梯度 (TMorale)
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
#include "BotRouteFix/module1_defense"
#include "BotRouteFix/module2_bombsite"
#include "BotRouteFix/module3_danger"
#include "BotRouteFix/module4_hunt"
#include "BotRouteFix/module5_combat"
#include "BotRouteFix/module6_tmorale"
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
    // 1. Register core ConVars
    CreateConVar("sm_botroutefix_version", PLUGIN_VERSION, "BotRouteFix plugin version", FCVAR_NOTIFY | FCVAR_DONTRECORD);
    g_cvEnable = CreateConVar("sm_botroutefix_enable", "1", "Enable or disable BotRouteFix core tactical overhaul", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    g_cvDebug  = CreateConVar("sm_botroutefix_debug",  "0", "Enable or disable verbose debug telemetry output", FCVAR_NONE, true, 0.0, true, 1.0);

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

    // 4. Initialize reverse engineering infrastructure
    InitSDKCalls();

    // 5. Initialize business modules
    InitModule1_Defense();
    InitModule2_Bombsite();
    InitModule3_Danger();
    InitModule4_Hunt();
    InitModule5_Combat();
    InitModule6_TMorale();

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
}

public void OnMapStart()
{
    // 地图重置逻辑
}

public void OnClientPutInServer(int client)
{
    // 客户端进服初始化
}

public void OnClientDisconnect(int client)
{
    // 客户端离线清理
}
