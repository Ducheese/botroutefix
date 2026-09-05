#if defined _botroutefix_events_included
    #endinput
#endif
#define _botroutefix_events_included

//========================================================================================
// GAME EVENTS HUB
//
// 统一监听 CS:S 核心游戏事件，并向各业务模块分发时序通知（如回合开始、冻结结束、下包等）。
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

//========================================================================================
// EVENT HANDLERS
//========================================================================================

public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    // 回合重置，通知各模块执行新回合初始化
    Module2_OnRoundStart();
    Module3_OnRoundStart();
    Module6_OnRoundStart();
    Module7_OnRoundStart();
}

public void Event_RoundFreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    // 冻结时间结束，Bot 决策树正式激活
    Module2_OnRoundFreezeEnd();
    Module6_OnRoundFreezeEnd();
    Module7_OnRoundFreezeEnd();
}

public void Event_ItemPickup(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

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
}

public void Event_BombDropped(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    Module2_OnBombDropped();
    Module7_OnBombDropped();
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (IsValidClient(victim, false))
    {
        Module2_OnPlayerDeath(victim);
        Module7_OnPlayerDeath(victim);
    }
}

public void Event_BombPlanted(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    // C4 已安放，拆包/守包决策链激活
    Module2_OnBombPlanted();
    Module6_OnBombPlanted();
    Module7_OnBombPlanted();
}

public void Event_BombDefused(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    Module6_OnBombDefused();
    Module7_OnBombDefused();
}

public void Event_BombExploded(Event event, const char[] name, bool dontBroadcast)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return;
    }

    Module6_OnBombExploded();
    Module7_OnBombExploded();
}
