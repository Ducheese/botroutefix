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

//========================================================================================
// HANDLES & GLOBAL VARIABLES
//========================================================================================

// Gamedata 句柄
Handle g_hGameConf = null;

// 运行环境架构标识 (true = 64-bit Steam x64, false = 32-bit v91/v92)
bool g_bIsWin64 = false;

// 全局 ConVar
ConVar g_cvEnable = null;
ConVar g_cvDebug = null;

// 常用 Bot 内部偏移缓存（跨模块共享）
int g_iOffset_Morale = -1;

// 全局单例指针引用（跨模块共享）
Address g_pTheCSBotsPtr;
Address g_pTheNavAreasCount;
Address g_pTheNavAreasBase;

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
 * 输出调试级战术遥测日志（仅在 sm_botroutefix_debug 开启时输出到控制台）
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
