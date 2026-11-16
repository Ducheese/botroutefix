#if defined _botroutefix_global_included
    #endinput
#endif
#define _botroutefix_global_included

//========================================================================================
// DEFINES & CONSTANTS
//========================================================================================

#define PLUGIN_NAME                 "BotRouteFix"
#define PLUGIN_VERSION              "2.0.0"
#define PLUGIN_AUTHOR               "Ducheese"
#define PLUGIN_DESCRIPTION          "CS:S CSBot decision chain & tactical routing overhaul framework"
#define PLUGIN_URL                  "https://space.bilibili.com/1889622121"

#define GAMEDATA                    "botroutefix.gamedata"

// 补丁缓冲与模板匹配常量
#define MAX_PATCH_SIZE              128

// 引擎原生 NavArea 危险度自然沉降速率 (1.0 每 120 秒)
// 沉降契约的唯一出处：模块 3(软衰减) / 模块 8(排队钉刺) / 模块 11(走廊钉刺) 共用
#define DANGER_DECAY_RATE_SEC       120.0

//========================================================================================
// HANDLES & GLOBAL VARIABLES
//========================================================================================

// Gamedata 句柄
Handle g_hGameConf = null;

// 运行环境架构标识 (true = 64-bit Steam x64, false = 32-bit v91/v92)
bool g_bIsWin64 = false;

// 全局 ConVar
ConVar g_cvDebug = null;
ConVar g_cvModules = null;

// 对局炸弹状态（唯一写入方：events.inc 事件总线；任何业务模块只读）
// 上收至全局：下包状态是整局流程的一等事实，不得随任一模块模块启停而断流
bool g_bBombPlanted = false;

// 常用 Bot 内部偏移缓存（跨模块共享；OnPluginStart 一次解析，事项 Prep 只做 > 0 门禁）
int g_iOffset_Morale = -1;
int g_iOffset_Task = -1;
int g_iOffset_Disposition = -1;
int g_iOffset_State = -1;
int g_iOffset_FetchBombState = -1;

// 全局单例指针引用（跨模块共享）
Address g_pTheCSBotsPtr;
Address g_pTheNavAreasCount;
Address g_pTheNavAreasBase;

// HideState 虚表指纹（跨模块真值：M15 共享 HideState::OnEnter 钩子首次目击时捕获，
// M9 名册落子前判"正处 Hide"用。虚表地址进程内稳定，换图不失效；捕获前恒为 Null，
// 此时返回 false 物理成立（无 Hide 进入即无 Hide 存活），M9 同时是宿主借钩方，M12/M15
// 关闭不会卸载宿主）
Address g_pHideStateVTable;   // 默认零 = Address_Null，首次目击 HideState 时由 M15 钩子写入

//========================================================================================
// COMPATIBILITY (SourceMod 1.11 / 1.12 地址读写兼容层)
//========================================================================================

#if defined SOURCEMOD_V_MAJOR && SOURCEMOD_V_MAJOR == 1 && SOURCEMOD_V_MINOR < 12
stock Address LoadAddressFromAddress(Address addr)
{
    return view_as<Address>(LoadFromAddress(addr, NumberType_Int32));
}

stock void StoreAddressToAddress(Address addr, Address val, bool updateMemAccess = true)
{
    StoreToAddress(addr, val, NumberType_Int32, updateMemAccess);
}
#endif

//========================================================================================
// LOGGING & TELEMETRY HELPER
//========================================================================================

/**
 * 输出调试级战术遥测日志（仅在 sm_brf_debug 开启时输出到控制台）
 *
 * @param format     格式化字符串
 * @param ...        格式化参数列表
 */
stock void DebugLog(const char[] format, any ...)
{
    if (g_cvDebug != null && g_cvDebug.BoolValue)
    {
        char buffer[256];
        VFormat(buffer, sizeof(buffer), format, 2);
        LogMessage("%s", buffer);
    }
}
