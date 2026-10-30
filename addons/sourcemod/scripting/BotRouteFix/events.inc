#if defined _botroutefix_events_included
    #endinput
#endif
#define _botroutefix_events_included

//========================================================================================
// GAME EVENTS HUB
//
// 统一监听 CS:S 核心游戏事件，并向各业务模块分发时序通知（如回合开始、冻结结束、下包等）。
//
// 职责边界：
//   1. 事件总线无条件转发 —— 不再设有任何"总开关粗暴拦截"，模块启停由各自业务态负责；
//   2. 对局炸弹状态 g_bBombPlanted 的唯一写入方 —— 即使任何一个业务模块被关闭，
//      下包/拆包/爆炸/换图的全局状态流转也绝不断流。
//
// 接收方契约（模块侧，统一口径）：
//   a) 有业务副作用的处理器，函数首行必须自守：if (!g_bModuleX_Active) return;
//   b) 纯幂等清理（模块关闭态下本就是 no-op：清计数/清名单/释放跟随）可豁免守卫，无需逐处标注；
//   c) 换图必须执行的底层重建（如 M3 重解导航容器）【不得】加守卫——
//      关闭态下若不重建，全局指针会停在已释放的旧网格上，下次启用即成野指针写。
//========================================================================================

/**
 * 注册核心游戏事件监听器
 */
void InitEvents()
{
    HookEvent("round_start", Event_RoundStart, EventHookMode_PostNoCopy);
    HookEvent("round_freeze_end", Event_RoundFreezeEnd, EventHookMode_PostNoCopy);
    HookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
    HookEvent("item_pickup", Event_ItemPickup, EventHookMode_Post);
    HookEvent("bomb_dropped", Event_BombDropped, EventHookMode_PostNoCopy);
    HookEvent("bomb_planted", Event_BombPlanted, EventHookMode_PostNoCopy);
    HookEvent("bomb_defused", Event_BombDefused, EventHookMode_PostNoCopy);
    HookEvent("bomb_exploded", Event_BombExploded, EventHookMode_PostNoCopy);
}

/**
 * 卸载核心游戏事件监听器
 */
void CleanupEvents()
{
    UnhookEvent("round_start", Event_RoundStart, EventHookMode_PostNoCopy);
    UnhookEvent("round_freeze_end", Event_RoundFreezeEnd, EventHookMode_PostNoCopy);
    UnhookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
    UnhookEvent("item_pickup", Event_ItemPickup, EventHookMode_Post);
    UnhookEvent("bomb_dropped", Event_BombDropped, EventHookMode_PostNoCopy);
    UnhookEvent("bomb_planted", Event_BombPlanted, EventHookMode_PostNoCopy);
    UnhookEvent("bomb_defused", Event_BombDefused, EventHookMode_PostNoCopy);
    UnhookEvent("bomb_exploded", Event_BombExploded, EventHookMode_PostNoCopy);
}

/**
 * 地图加载：换图 = 新对局，旧 planted 旗不得跨图残留
 * （由主插件 OnMapStart 直调，保证 g_bBombPlanted 的全部写入点都收敛在本文件）
 */
void Events_OnMapStart()
{
    g_bBombPlanted = false;
}

//========================================================================================
// EVENT HANDLERS
//========================================================================================

public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
    // 新回合：炸弹状态复位（必须在分发前完成，模块读到的是本回合的确定事实）
    g_bBombPlanted = false;

    // 回合重置，通知各模块执行新回合初始化
    Module2_OnRoundStart();
    Module3_OnRoundStart();
    Module4_OnRoundStart();
    Module5_OnRoundStart();
    Module7_OnRoundStart();
    Module8_OnRoundStart();
    Module9_OnRoundStart();
    Module10_OnRoundStart();
}

public void Event_RoundFreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
    // 冻结时间结束，Bot 决策树正式激活
    Module2_OnRoundFreezeEnd();
    Module6_OnRoundFreezeEnd();
    Module7_OnRoundFreezeEnd();
}

public void Event_ItemPickup(Event event, const char[] name, bool dontBroadcast)
{
    int client = GetClientOfUserId(event.GetInt("userid"));
    if (!IsValidClient(client, false))
    {
        return;
    }

    char item[32];
    event.GetString("item", item, sizeof(item));

    Module2_OnItemPickup(client, item);
    Module6_OnItemPickup(client, item);
    Module7_OnItemPickup(client, item);
    Module9_OnItemPickup(client, item);
}

public void Event_BombDropped(Event event, const char[] name, bool dontBroadcast)
{
    Module2_OnBombDropped();
    Module7_OnBombDropped();
    Module9_OnBombDropped();
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (IsValidClient(victim, false))
    {
        Module2_OnPlayerDeath(victim);
        Module5_OnPlayerDeath(victim);
        Module7_OnPlayerDeath(victim);
        Module9_OnPlayerDeath(victim);
        Module10_OnPlayerDeath(victim);
    }
}

public void Event_BombPlanted(Event event, const char[] name, bool dontBroadcast)
{
    // C4 已安放：先落全局状态，再分发（拆包/守包决策链据此判断）
    g_bBombPlanted = true;

    Module2_OnBombPlanted();
    Module6_OnBombPlanted();
    Module7_OnBombPlanted();
    Module10_OnBombPlanted();
}

public void Event_BombDefused(Event event, const char[] name, bool dontBroadcast)
{
    g_bBombPlanted = false;

    Module7_OnBombDefused();
    Module10_OnBombDefused();
}

public void Event_BombExploded(Event event, const char[] name, bool dontBroadcast)
{
    g_bBombPlanted = false;

    Module7_OnBombExploded();
    Module10_OnBombExploded();
}
