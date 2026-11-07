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
//      当选/回补时 task 19 的蹲守者强制 Hunt 解蹲一次（蹲坑 Hide 里到不了 NoticeLooseBomb，
//      Idle 的 loose 检查又排在狙击手守点/架枪掷骰之后，不踢就瘫痪；Hunt 落 SEEK_AND_DESTROY，M12 不认）；
//      选举失败（找不到掉包）或测距未就绪 → 空名册 = 全体放行回原生；
//    - player_death / client_disconnect：除名 + 按掉包点就近回补（共用同一函数；名单有效时 C4 必躺掉包点，
//      按掉包点招人即按 C4 位置招人）——必选项，否则减员到全灭即全员 false，包烂地上没人捡（比原版更糟的死锁）；
//    - item_pickup(c4) / round_start / OnMapStart：清（FetchBombState:58 有人捡包自己散伙；换图重置）。
// 4. fail-open 原则：钩子只会把 true 掐成 false，且仅在名册有效时掐；名册空即原生。
// 全程事件驱动，无 timer 轮询。
//========================================================================================

//========================================================================================
// MODULE 9 DEFINES & GLOBALS
//========================================================================================

#define LOOT_ROSTER_SIZE 3

// 蹲守承诺任务（与 module12 的 BOT_TASK_MOVE_TO_SNIPER_SPOT 同值 19；module9 先于
// module12 被 include，不能复用它的宏，只能本地再定一遍，改值时两处同步）
#define LOOT_CAMP_TASK 19

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

//========================================================================================
// MODULE 9 ITEM BACKEND: LOOSE-BOMB FETCH ROSTER (NoticeLooseBomb pre-hook + NavMeasure)
//========================================================================================

/**
 * 事项后端：掉包抢包花名册（ITEM_M9_LOOT）
 *
 * 只创建 Detour、绑定签名、校验测距子系统，不启用钩子（启用由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把抢包事项标记为 FAILED。
 *
 * @return              true = 抢包事项后端就绪
 */
static bool PrepModule9_LootBackend()
{
    g_hLooseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hLooseDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb detour creation failed; loot item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hLooseDetour, g_hGameConf, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb signature missing; loot item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    if (!BotNavReady())
    {
        LogError("[BotRouteFix] [Module 9] NavMeasure unavailable; loot item FAILED (no fail-open roster).");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    // NoticeLooseBomb(void)：无参数，this 即 bot 本体
    return true;
}


//========================================================================================
// MODULE 9 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 9（挂 NoticeLooseBomb Pre）
 * 签名双架构现成，缺失即 fail-fast（与待机模块不同：这里缺 = gamedata 坏了）
 */
bool InitModule9_Loot()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 9] GameConf missing; loot item FAILED.");
        Item_SetReady(ITEM_M9_LOOT, false);
        return false;
    }

    Item_SetReady(ITEM_M9_LOOT, PrepModule9_LootBackend());
    EnableModule9_Loot(true);

    LogModuleArmed(9, "Loose-bomb (NoticeLooseBomb Pre hook)",
        Item_IsArmed(ITEM_M9_LOOT) ? "1" : "0");
    return g_bModule9_Active;
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

    if (!enable)
    {
        RestoreModule9_LootBackend();
        g_bModule9_Active = false;
        return;
    }

    g_bModule9_Active = ApplyModule9_LootBackend();
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


//========================================================================================
// MODULE 9 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M9 抢包花名册的名单查询（模块 5 后撤档位裁决复用）
 *
 * -1 槽位跳过；纯内存判断。
 */
bool Module9_IsInRoster(int client)
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

/**
 * 获取当前名册有效人数
 */
int Module9_GetRosterCount()
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
int Module9_GetRosterSize()
{
    return LOOT_ROSTER_SIZE;
}

/**
 * 获取累计选举次数
 */
int Module9_GetTotalElections()
{
    return g_iTotalLootElections;
}

/**
 * 获取累计拦截非名册哄抢次数
 */
int Module9_GetTotalGated()
{
    return g_iTotalLootGated;
}

/**
 * 获取当前捡包花名册 Bot 列表与掉落 C4 坐标 (供可视化子系统渲染牵引光束)
 *
 * @param roster        输出候选人 client 索引数组 (容量 LOOT_ROSTER_SIZE)
 * @param c4Pos         输出掉落 C4 的世界坐标
 * @return              实际有效候选人数量
 */
int Module9_GetFetchRoster(int roster[LOOT_ROSTER_SIZE], float c4Pos[3])
{
    if (!g_bHasRoster)
    {
        return 0;
    }

    if (!Loot_FindDropPos(c4Pos))
    {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        int bot = g_iLootRoster[i];
        if (IsValidClient(bot, true))
        {
            roster[count++] = bot;
        }
    }
    return count;
}


//========================================================================================
// MODULE 9 EVENT HANDLERS
//========================================================================================

/**
 * 掉包事件：递延一帧再选举（等 C4 归属落定，模块 7 FrameRefresh 同款；直接查必 miss）
 */
void Module9_OnBombDropped()
{
    if (!Item_IsArmed(ITEM_M9_LOOT))
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
    if (!Item_IsArmed(ITEM_M9_LOOT))
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


//========================================================================================
// MODULE 9 DETOUR & HOOKS (NOTICELOOSEBOMB::PRE)
//========================================================================================

/**
 * 事项 M9 抢包花名册的唯一入口钩子（NoticeLooseBomb Pre）
 *
 * ITEM_M9_LOOT 武装且名册有效时才掐非名册者；只会把 true 掐成 false，
 * 且 CT 一律 Supercede（阵营门补漏）。名册空即全体放行回原生。
 */
public MRESReturn Hook_NoticeLooseBomb_Pre(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M9_LOOT))
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    // 阵营门补漏：原生 NoticeLooseBomb 注释写 "for T's only"（虽底层调用点均有阵营包裹，此处作为纵深防御兜底）
    if (!IsTeamBot(client, 2, true))
    {
        DHookSetReturn(hParams, false);
        return MRES_Supercede;
    }

    if (!g_bHasRoster || Module9_IsInRoster(client))
    {
        return MRES_Ignored;
    }

    DHookSetReturn(hParams, false);
    g_iTotalLootGated++;
    // DebugLog("[BotRouteFix] [Module 9] Bot %N denied loose-bomb fetch (off-roster)", client);
    return MRES_Supercede;
}


//========================================================================================
// MODULE 9 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用抢包事项：只有 Prep 已就绪才启用 Pre 钩子
 *
 * 事项 ITEM_M9_LOOT 的应用入口；不做 Detour 创建与签名绑定（那是 Prep 的职责）。
 * 失败时该事项 FAILED。
 *
 * @return              true = 抢包钩子已驻留
 */
static bool ApplyModule9_LootBackend()
{
    if (!Item_IsReady(ITEM_M9_LOOT) || g_hLooseDetour == null)
    {
        Item_SetArmed(ITEM_M9_LOOT, false);
        return false;
    }

    if (!SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked,
        false, Hook_NoticeLooseBomb_Pre, true, "Module 9"))
    {
        // 挂钩失败属动态环境抖动，不证伪探测结论：只清 Armed 保留 Ready，下次 Enable 重试
        Item_SetArmed(ITEM_M9_LOOT, false);
        LogError("[BotRouteFix] [Module 9] Loot detour enable failed; loot item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M9_LOOT, true);
    return true;
}

/**
 * 还原抢包事项：摘钩、清空名册（= 全体放行回原生）并解除武装
 */
static void RestoreModule9_LootBackend()
{
    SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked, false, Hook_NoticeLooseBomb_Pre, false, "Module 9");
    Loot_ClearRoster();
    Item_SetArmed(ITEM_M9_LOOT, false);
}


//========================================================================================
// MODULE 9 ROSTER PRIMITIVES
//========================================================================================

/**
 * 事项 M9 抢包花名册的清空：fail-open（无名册 = 全体放行回原生）
 *
 * 停用 / 捡包 / 回合重置 / 换图共用；只清名单不动钩子。
 */
static void Loot_ClearRoster()
{
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        g_iLootRoster[i] = -1;
    }
    g_bHasRoster = false;
}

/**
 * 事项 M9 抢包花名册的掉包定位：找无主掉包（m_hOwnerEntity == -1 的 weapon_c4）
 *
 * 找不到返回 false，调用方按 fail-open 回原生（不等、不猜）。
 */
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

/**
 * 事项 M9 抢包花名册的蹲守承诺判定：task == 19 即视为蹲坑中
 *
 * task 是任务不是姿态：Hide 不改 task，所以这里只能抓到"狙击手式蹲坑"
 * （MOVE_TO_SNIPER_SPOT，M12 自己也认这个值）；残血等其他 task 的长蹲抓不到，
 * 靠 Hide 到期自愈。偏移缺失/实体无效一律按"非蹲守"处理（宁可漏踢，不错杀）。
 *
 * @param bot           客户端索引
 * @return              true = 正蹲守承诺中，捡包指望不上它
 */
static bool Loot_IsCampCommitted(int bot)
{
    if (g_iOffset_Task <= 0)
    {
        return false;
    }

    Address pBot = GetEntityAddress(bot);
    if (pBot == Address_Null)
    {
        return false;
    }

    return LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == LOOT_CAMP_TASK;
}

/**
 * 事项 M9 抢包花名册的蹲守者解蹲：对名册里 task 19 的，一人一次强制 Hunt
 *
 * 蹲坑者 Hide 里永远到不了 NoticeLooseBomb（Idle 的 loose 检查排在狙击手
 * 守点/架枪掷骰之后，M12 再把 task 19 的 Hide 留守），不踢出来名册就是摆设。
 * Hunt 进去 task 落 SEEK_AND_DESTROY，M12 不认（只认 19），交接干净；
 * Hunt SDKCall 缺失则跳过（名册照转被动门禁，不拓宽 FAILED 面）。
 * 调用点保证帧上下文（选举/回补都在 RequestFrame 或事件里，不在钩子里），直调安全。
 *
 * @param bot           名册成员客户端索引
 */
static void Loot_ForceHuntIfCamping(int bot)
{
    if (!Loot_IsCampCommitted(bot))
    {
        return;
    }

    if (BotHunt(bot))
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N was camping (task 19), forced Hunt to fetch", bot);
    }
    else
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N camping but Hunt SDKCall unavailable, left passive", bot);
    }
}

/**
 * 事项 M9 抢包花名册的选举：掉包点真实地面行走距离最近 3 存活 T bot
 *
 * 测距未就绪 / 找不到掉包 / 无存活 T bot 即空名册回原生（不拿欧氏混分，不可达跳过）。
 * 选完对 task 19 的蹲守者强制 Hunt 解蹲（否则 Hide 里永远捡不了包）。
 */
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

    // 蹲守者解蹲：名册里 task 19 的不踢出来就永远捡不了包
    for (int i = 0; i < LOOT_ROSTER_SIZE; i++)
    {
        if (g_iLootRoster[i] != -1)
        {
            Loot_ForceHuntIfCamping(g_iLootRoster[i]);
        }
    }
}

/**
 * 除名 + 按掉包点就近回补 + 全灭 fail-open（死亡与掉线共用）
 * 名单有效时 C4 必躺在掉包点（谁捡起名单即散），按掉包点招人即按 C4 位置招人
 */
static void Loot_RemoveAndBackfill(int client, const char[] reason)
{
    if (!g_bHasRoster || !Module9_IsInRoster(client))
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
        if (!IsTeamBot(i, 2, true) || Module9_IsInRoster(i))
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
        Loot_ForceHuntIfCamping(best);
    }

    // 全灭则解散回原生（fail-open）
    if (Module9_GetRosterCount() == 0)
    {
        Loot_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Roster wiped, fail-open to native");
    }
}
