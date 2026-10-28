#if defined _botroutefix_module9_loot_included
    #endinput
#endif
#define _botroutefix_module9_loot_included

//========================================================================================
// MODULE 9: LOOSE-BOMB TOP3 FETCH ROSTER (NoticeLooseBomb pre-hook)
//
// 业务背景与引擎逆向分析：
//
// 1. 机制分析 (cs_bot.cpp:428-444):
//    NoticeLooseBomb() = GetLooseBomb() != NULL 即返回 true，注释写 "for T's only"
//    原生虽在现有调用点做了阵营包裹，但 NoticeLooseBomb 查询原语本身缺少阵营限制。
//    两个调用点（IdleState:228、HuntState:74）。
// 2. 卡门：只钩这一个查询函数，两条路全堵；本钩顺带补齐阵营门控作为纵深防御兜底：CT 一律 Supercede + false。
//    CT 剩余感知（视力 CanSeeLooseBomb、捡包 1000 听觉）均在其它函数，不受影响。
// 3. 花名册（与模块 7 同构，全事件驱动，无 timer）：
//    - bomb_dropped：递延一帧（等归属落定）后以掉包点（无主 weapon_c4 实体坐标，零耦合不读模块 7）为圆心，
//      真实地面行走距离（BotNavWalkDistance 无损实测）取最近 3 存活 T bot，不可达跳过；
//      选举失败（找不到掉包）或测距未就绪 → 空名册 = 全体放行回原生；
//    - player_death / client_disconnect：除名 + 按掉包点就近回补（共用同一函数；名单有效时 C4 必躺掉包点，
//      按掉包点招人即按 C4 位置招人）——必选项，否则减员到全灭即全员 false，包烂地上没人捡（比原版更糟的死锁）；
//    - item_pickup(c4) / round_start / OnMapStart：清（FetchBombState:58 有人捡包自己散伙；换图重置）。
// 4. fail-open 原则：钩子只会把 true 掐成 false，且仅在名册有效时掐；名册空即原生。
// 全程事件驱动，无 timer 轮询。
//========================================================================================

#define LOOT_ROSTER_SIZE 3

// 本模块私有状态（static，名册与句柄不出本文件）
static bool g_bModule9_Active = false;      // 业务开启态
static bool g_bLooseHooked = false;         // per-hook 真实启用态
static Handle g_hLooseDetour = null;
static int g_iLootRoster[LOOT_ROSTER_SIZE] = {-1, -1, -1};
static bool g_bHasRoster = false;
static float g_fDropPos[3];

// 遥测统计计数器
static int g_iTotalLootGated = 0;
static int g_iTotalLootElections = 0;

// 名册内查找（-1 槽位跳过）
static bool Loot_IsInRoster(int client)
{
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        if (g_iLootRoster[i] == client)
        {
            return true;
        }
    }
    return false;
}

// 清空名册（fail-open：无名册 = 全体放行回原生）
static void Loot_ClearRoster()
{
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        g_iLootRoster[i] = -1;
    }
    g_bHasRoster = false;
}

// 找无主掉包坐标（m_hOwnerEntity == -1 的 weapon_c4）
static bool Loot_FindDropPos(float dropPos[3])
{
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_c4")) != -1)
    {
        if (GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") == -1)
        {
            GetEntPropVector(ent, Prop_Send, "m_vecOrigin", dropPos);
            return true;
        }
    }
    return false;
}

// 选举：掉包点真实地面行走距离 (NavMeasure) 最近 3 存活 T bot
static void Loot_ElectRoster()
{
    Loot_ClearRoster();

    float dropPos[3];
    if (!Loot_FindDropPos(dropPos))
    {
        DebugLog("[BotRouteFix] [Module 9] Bomb dropped but loose C4 not found, fail-open to native");
        return;
    }
    g_fDropPos = dropPos;

    // 测距未就绪：无名单，全员放行回原生（不拿欧氏混分）
    if (!BotNavReady())
    {
        Loot_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] NavMeasure standby, fail-open to native");
        return;
    }

    float bestDist[LOOT_ROSTER_SIZE];
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        bestDist[i] = 9999999.0;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true))
        {
            continue;
        }

        // 真实地面行走距离选人；不可达跳过（跨楼层够不着的不选）
        float dist = BotNavWalkDistance(i, dropPos);
        if (dist < 0.0)
        {
            continue;
        }

        for (int slot = 0; slot < LOOT_ROSTER_SIZE; slot++)
        {
            if (dist < bestDist[slot])
            {
                for (int shift = LOOT_ROSTER_SIZE - 1; shift > slot; shift--)
                {
                    g_iLootRoster[shift] = g_iLootRoster[shift - 1];
                    bestDist[shift] = bestDist[shift - 1];
                }
                g_iLootRoster[slot] = i;
                bestDist[slot] = dist;
                break;
            }
        }
    }

    if (g_iLootRoster[0] == -1)
    {
        DebugLog("[BotRouteFix] [Module 9] No alive T bots to elect, fail-open to native");
        return;
    }

    g_bHasRoster = true;
    g_iTotalLootElections++;
    DebugLog("[BotRouteFix] [Module 9] Elected fetch roster: %N / %N / %N",
        g_iLootRoster[0],
        (g_iLootRoster[1] != -1) ? g_iLootRoster[1] : g_iLootRoster[0],
        (g_iLootRoster[2] != -1) ? g_iLootRoster[2] : g_iLootRoster[0]);
}

public MRESReturn Hook_NoticeLooseBomb_Pre(int client, DHookParam hParams)
{
    if (!g_bModule9_Active)
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    // 阵营门补漏：原生 NoticeLooseBomb 注释写 "for T's only"（虽底层调用点均有阵营包裹，此处作为纵深防御兜底）
    if (GetClientTeam(client) != 2)
    {
        DHookSetReturn(hParams, false);
        return MRES_Supercede;
    }

    if (!g_bHasRoster || Loot_IsInRoster(client))
    {
        return MRES_Ignored;
    }

    DHookSetReturn(hParams, false);
    g_iTotalLootGated++;
    // DebugLog("[BotRouteFix] [Module 9] Bot %N denied loose-bomb fetch (off-roster)", client);
    return MRES_Supercede;
}

/**
 * 查询掉包感知 Detour 是否处于就绪/已挂载状态
 */
bool IsLootHooked()
{
    return (g_hLooseDetour != null && g_bLooseHooked);
}

/**
 * 获取当前名册有效人数
 */
int Loot_GetRosterCount()
{
    int count = 0;
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        if (g_iLootRoster[i] != -1 && IsBot(g_iLootRoster[i], true))
        {
            count++;
        }
    }
    return count;
}

/**
 * 获取抢包花名册容量上限
 */
int Loot_GetRosterSize()
{
    return LOOT_ROSTER_SIZE;
}

/**
 * 获取累计选举次数
 */
int GetTotalLootElections()
{
    return g_iTotalLootElections;
}

/**
 * 获取累计拦截非名册哄抢次数
 */
int GetTotalLootGated()
{
    return g_iTotalLootGated;
}


/**
 * 初始化模块 9（挂 NoticeLooseBomb Pre）
 * 签名双架构现成，缺失即 fail-fast（与待机模块不同：这里缺 = gamedata 坏了）
 */
bool InitModule9_Loot()
{
    g_hLooseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hLooseDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] Failed to create DHook detour instance for NoticeLooseBomb!");
        return false;
    }

    if (!DHookSetFromConf(g_hLooseDetour, g_hGameConf, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
    {
        LogError("[BotRouteFix] [Module 9] Failed to set NoticeLooseBomb signature from gamedata!");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    // NoticeLooseBomb(void)：无参数，this 即 bot 本体
    EnableModule9_Loot(true);
    if (!g_bModule9_Active)
    {
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        LogError("[BotRouteFix] [Module 9] Failed to enable NoticeLooseBomb Pre detour!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 9] Loose-bomb Top3 roster online (NoticeLooseBomb Pre hook)");
    return true;
}

/**
 * 动态启用或停用模块 9（幂等；停用即清空名册 = 全放行回原生）
 */
void EnableModule9_Loot(bool enable)
{
    if (enable == g_bModule9_Active)
    {
        return;
    }

    if (enable)
    {
        g_bModule9_Active = SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked,
            false, Hook_NoticeLooseBomb_Pre, true, "Module 9");
        return;
    }

    SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked, false, Hook_NoticeLooseBomb_Pre, false, "Module 9");
    Loot_ClearRoster();
    g_bModule9_Active = false;
}

/**
 * 还原模块 9（插件卸载：摘钩并释放句柄）
 */
void RestoreModule9_Loot()
{
    EnableModule9_Loot(false);

    if (g_hLooseDetour != null)
    {
        delete g_hLooseDetour;
        g_hLooseDetour = null;
    }
}

/**
 * 查询模块 9 业务开启态
 */
bool IsModule9_Active()
{
    return g_bModule9_Active;
}

/**
 * 掉包事件：递延一帧再选举（等 C4 归属落定，模块 7 FrameRefresh 同款；直接查必 miss）
 */
void Module9_OnBombDropped()
{
    if (!g_bModule9_Active)
    {
        return;
    }

    RequestFrame(Module9_FrameElect);
}

/**
 * 递延选举帧（RequestFrame 回调）
 */
public void Module9_FrameElect(any data)
{
    // 递延帧可能在停用之后才到，必须二次校验业务态
    if (!g_bModule9_Active)
    {
        return;
    }

    Loot_ElectRoster();
}

/**
 * 死亡事件：成员阵亡递补下家（防死锁必选项）
 */
void Module9_OnPlayerDeath(int victim)
{
    Loot_RemoveAndBackfill(victim, "fallen member");
}

/**
 * 客户端掉线 (OnClientDisconnect)：无死亡事件，槽位会烂掉——同递补逻辑
 */
void Module9_OnClientDisconnect(int client)
{
    Loot_RemoveAndBackfill(client, "disconnect");
}

/**
 * 除名 + 按掉包点就近回补 + 全灭 fail-open（死亡与掉线共用）
 * 名单有效时 C4 必躺在掉包点（谁捡起名单即散），按掉包点招人即按 C4 位置招人
 */
static void Loot_RemoveAndBackfill(int client, const char[] reason)
{
    if (!g_bHasRoster || !Loot_IsInRoster(client))
    {
        return;
    }

    // 腾出槽位
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        if (g_iLootRoster[i] == client)
        {
            g_iLootRoster[i] = -1;
        }
    }

    int best = -1;
    float bestDist = 9999999.0;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true) || Loot_IsInRoster(i))
        {
            continue;
        }

        float dist = BotNavWalkDistance(i, g_fDropPos);
        if (dist < 0.0)
        {
            continue;
        }
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }

    if (best != -1)
    {
        for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
        {
            if (g_iLootRoster[i] == -1)
            {
                g_iLootRoster[i] = best;
                break;
            }
        }
        g_iTotalLootElections++;
        DebugLog("[BotRouteFix] [Module 9] Roster backfill (%s): %N joins", reason, best);
    }

    // 全灭则解散回原生（fail-open）
    if (Loot_GetRosterCount() == 0)
    {
        Loot_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Roster wiped, fail-open to native");
    }
}

/**
 * 捡包事件：有人捡起 C4 即解散（FetchBombState 自己散伙）
 */
void Module9_OnItemPickup(int client, const char[] item)
{
    if (!g_bHasRoster)
    {
        return;
    }

    // 冻结期系统发包不触发（与 module2/6/7 同守卫；此时名册恒空，本是 no-op，防后人加逻辑踩坑）
    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        Loot_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Bomb picked up by %N, roster disbanded", client);
    }
}

/**
 * 回合开始：清名册
 */
void Module9_OnRoundStart()
{
    Loot_ClearRoster();
}

/**
 * 地图加载 (OnMapStart)：名单索引随实体全废，必须清（round_start 也会清，显式正确）
 */
void Module9_OnMapStart()
{
    Loot_ClearRoster();
}
