#if defined _botroutefix_module9_loosefetch_included
    #endinput
#endif
#define _botroutefix_module9_loosefetch_included

//========================================================================================
// MODULE 9: LOOSE-BOMB TOP3 FETCH ROSTER
// (NoticeLooseBomb Pre Hook)
//
// 业务背景与引擎逆向分析：
//
// 1. 机制分析 (cs_bot.cpp:428-444):
//    NoticeLooseBomb() 仅两个调用点：HuntState::OnUpdate (cs_bot_hunt.cpp:58,74)
//    与 IdleState::OnUpdate (cs_bot_idle.cpp:116,228)，且原生调用方外层均有 TEAM_TERRORIST 包裹。
//    不在此重复做无益的 CT 拦截，非存活 T 一律 MRES_Ignored 直通原生。
// 2. 750 码距离门控 (与 M10 拆包名册对称)：
//    - 名册为空（无候选/测距未就绪）或当选成员：NoticeLooseBomb 一律 MRES_Ignored 直通原生。
//    - 远端非名册存活 T（距无主 C4 > 750.0 units）：放行 MRES_Ignored，使其能向掉包大方向靠近，
//      途中若未被交敌打断留在 FetchBombState 顺手捡起亦无妨。
//    - 近端非名册存活 T（距无主 C4 <= 750.0 units）：Intercept 为 false + MRES_Supercede；
//      若在 750 码内因接敌/避险/换弹退出 FetchBombState 落回 Idle/Hunt，阻止其无脑再入抢包，
//      继续正常 HuntState 搜索交战状态机。
// 3. 花名册与冻结期对称性 (全事件驱动，无 timer)：
//    - 测距路由统一采用 ROUTE_FASTEST：掉包点常伴随前一任包匪阵亡钉刺的危险度，避免避险罚分扭曲最近候选排序。
//    - bomb_dropped：冻结期内直接跳过（交由 round_freeze_end 收尾）；常规对局递延一帧（等 C4 归属落定）
//      后以无主 weapon_c4（g_iLooseBombRef 实时跟踪坐标，回退缓存 g_fDropPos）为圆心选举最近 3 存活可达 T。
//    - round_freeze_end：若冻结期结束时地面仍存在无主 weapon_c4，立即补选一次。
//    - item_pickup(c4)：只要当前有有效名册即散伙（无需冻结期早退守卫；!g_bFetchHasRoster 已O(1)短路开局发包）。
//    - player_death / client_disconnect：名册减员后按实时掉落坐标就近回补，全灭则清名册回原生。
// 4. 蹲守解蹲与 M15 Hide(-1)/Ambush 协同：
//    - 当选/回补瞬间：task 19（MOVE_TO_SNIPER_SPOT）或正处 HideState（经 Module15_IsBotHiding 查指纹）者，
//      直调 BotHunt() 踢出（HuntState::OnUpdate 首行即 NoticeLooseBomb()；IdleState 中 NoticeLooseBomb 排在狙击手 camp 后）。
//    - 在途 Hide(-1) 短蹲与 Ambush Hide 踢出统一由 M15（module15_hideduration.inc）集中处理，本模块不再单独借 Pre 钩。
//========================================================================================

//========================================================================================
// MODULE 9 DEFINES & GLOBALS
//========================================================================================

#define FETCH_ROSTER_SIZE       3
#define FETCH_GUARD_DISTANCE    750.0

// 蹲守任务枚举（BOT_TASK_MOVE_TO_SNIPER_SPOT）上收 global.inc 统一定义，本文件只消费不定义

// 本模块私有状态（static，名册与句柄不出本文件）
static bool g_bModule9_Active = false;      // 业务开启态
static bool g_bLooseHooked = false;         // per-hook 真实启用态（NoticeLooseBomb Pre）
static Handle g_hLooseDetour = null;
static int g_iFetchRoster[FETCH_ROSTER_SIZE] = {-1, -1, -1};
static bool g_bFetchHasRoster = false;
static int g_iLooseBombRef = INVALID_ENT_REFERENCE;
static float g_fDropPos[3];

// 遥测统计计数器
static int g_iTotalFetchGated = 0;
static int g_iTotalFetchElections = 0;

//========================================================================================
// MODULE 9 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：掉包抢包花名册（ITEM_M9_FETCH）
 *
 * 只创建 NoticeLooseBomb Detour、绑定签名，并核验测距、偏移与 Hunt SDKCall，
 * 不提前启用钩子（启用由 Apply 执行）；M15 HideState 指纹宿主属于可选增强，不进硬门禁。
 * 失败语义：核心依赖（Detour/测距/State&Task偏移/Hunt）缺失返回 false，Init 将事项标记 FAILED（全成或全败，不降级）；
 * HideState 宿主缺失不阻断本事项。
 *
 * @return              true = 抢包事项后端就绪
 */
static bool PrepModule9_FetchBackend()
{
    g_hLooseDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hLooseDetour == null)
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb detour creation failed; M9 fetch roster item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hLooseDetour, g_hGameConf, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
    {
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb signature missing; M9 fetch roster item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }
    // NoticeLooseBomb(void)：无参数，this 即 bot 本体

    if (!BotNavReady())
    {
        LogError("[BotRouteFix] [Module 9] NavMeasure unavailable; M9 fetch roster item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    if (g_iOffset_State <= 0 || g_iOffset_Task <= 0)
    {
        LogError("[BotRouteFix] [Module 9] CCSBot state/task offsets missing; M9 fetch roster item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    // HideState::OnEnter 指纹捕获属于 best-effort 增强（C 类可选能力）：
    // 宿主缺失时 Module15_IsBotHiding() 恒返 false，750u 门控、Top3 选举与 task == 19 解蹲仍完整工作，不连坐失败。

    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [Module 9] CCSBot::Hunt SDKCall unavailable; M9 fetch roster item FAILED.");
        delete g_hLooseDetour;
        g_hLooseDetour = null;
        return false;
    }

    LogMessage("[BotRouteFix] [Module 9] Loose-bomb fetch backend verified (gate=%.0f, size=%d).",
        FETCH_GUARD_DISTANCE, FETCH_ROSTER_SIZE);
    return true;
}

//========================================================================================
// MODULE 9 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 9 的唯一事项：T 掉包 Top3 抢包花名册（ITEM_M9_FETCH）
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 */
bool InitModule9_Fetch()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 9] GameConf missing; M9 fetch roster item FAILED.");
        Item_SetReady(ITEM_M9_FETCH, false);
        return false;
    }

    Item_SetReady(ITEM_M9_FETCH, PrepModule9_FetchBackend());
    EnableModule9_Fetch(true);
    if (!g_bModule9_Active)
    {
        LogModuleAllFailed(9);
        return false;
    }

    LogModuleArmed(9, "Fetch", "1");
    return true;
}

/**
 * 动态启用或停用模块 9（事项 ITEM_M9_FETCH 的调度入口；幂等；停用即清空名册 = 全放行回原生）。
 *
 * 只做调度：只武装 Ready 的事项；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 */
void EnableModule9_Fetch(bool enable)
{
    if (enable == g_bModule9_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule9_FetchBackend();
        g_bModule9_Active = false;
        return;
    }

    g_bModule9_Active = ApplyModule9_FetchBackend();
}

/**
 * 还原模块 9（事项 ITEM_M9_FETCH；插件卸载：摘钩并释放句柄）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule9_Fetch()
{
    EnableModule9_Fetch(false);

    if (g_hLooseDetour != null)
    {
        delete g_hLooseDetour;
        g_hLooseDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 9] Fetch hooks safely restored.");
}

//========================================================================================
// MODULE 9 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M9 抢包花名册的名单查询（模块 5 后撤档位裁决与 M12 camp 门禁复用）
 *
 * 无效索引（client <= 0）直接返回 false；纯内存判断。
 */
bool Module9_IsInRoster(int client)
{
    if (client <= 0)
    {
        return false;
    }

    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] == client)
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
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] > 0 && IsTeamBot(g_iFetchRoster[i], 2, true))
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
    return FETCH_ROSTER_SIZE;
}

/**
 * 获取累计选举次数
 */
int Module9_GetTotalElections()
{
    return g_iTotalFetchElections;
}

/**
 * 获取累计拦截近端非名册哄抢次数
 */
int Module9_GetTotalGated()
{
    return g_iTotalFetchGated;
}

/**
 * 获取当前抢包花名册 Bot 列表与掉落 C4 坐标 (供可视化子系统渲染牵引光束)
 *
 * @param roster        输出候选人 client 索引数组 (容量 FETCH_ROSTER_SIZE)
 * @param c4Pos         输出掉落 C4 的世界坐标
 * @return              实际有效候选人数量
 */
int Module9_GetFetchRoster(int roster[FETCH_ROSTER_SIZE], float c4Pos[3])
{
    if (!g_bFetchHasRoster)
    {
        return 0;
    }

    if (!Fetch_GetLiveDropPos(c4Pos))
    {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        int bot = g_iFetchRoster[i];
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
 * 掉包事件：冻结期内跳过（交由 round_freeze_end 补选）；常规回合递延一帧选举（等 C4 归属落定）
 */
void Module9_OnBombDropped()
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    RequestFrame(Module9_FrameElect);
}

/**
 * 冻结期结束：若冻结期结束时地上已有无主 weapon_c4，立即补选花名册
 */
void Module9_OnRoundFreezeEnd()
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    float dummy[3];
    if (Fetch_FindDropPos(dummy))
    {
        Fetch_ElectRoster();
    }
}

/**
 * 递延选举帧（RequestFrame 回调）
 */
public void Module9_FrameElect(any data)
{
    // 递延帧可能在停用或重开回合冻结后才到，必须二次校验业务态与冻结期
    if (!Item_IsArmed(ITEM_M9_FETCH) || GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    Fetch_ElectRoster();
}

/**
 * 死亡事件：成员阵亡递补下家（防死锁必选项）
 */
void Module9_OnPlayerDeath(int victim)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    Fetch_RemoveAndBackfill(victim, "fallen member");
}

/**
 * 客户端掉线 (OnClientDisconnect)：无死亡事件，槽位会烂掉——同递补逻辑
 */
void Module9_OnClientDisconnect(int client)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return;
    }

    Fetch_RemoveAndBackfill(client, "disconnect");
}

/**
 * 抢包事件：只要当前名册有效，有人捡起 C4 即解散（无冻结期过滤，纯幂等清理）
 */
void Module9_OnItemPickup(int client, const char[] item)
{
    if (!Item_IsArmed(ITEM_M9_FETCH) || !g_bFetchHasRoster)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Bomb picked up by %N, roster disbanded", client);
    }
}

/**
 * 回合开始：清名册
 */
void Module9_OnRoundStart()
{
    Fetch_ClearRoster();
}

/**
 * 地图加载 (OnMapStart)：名单索引随实体全废，必须清（round_start 也会清，显式正确）
 */
void Module9_OnMapStart()
{
    Fetch_ClearRoster();
}

//========================================================================================
// MODULE 9 ROSTER PRIMITIVES
//========================================================================================

/**
 * 事项 M9 抢包花名册的清空：no-op 放行（无名册 = 全体放行回原生）
 *
 * 停用 / 抢包 / 回合重置 / 换图共用；只清名单与掉包缓存，不动钩子。
 */
static void Fetch_ClearRoster()
{
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        g_iFetchRoster[i] = -1;
    }
    g_bFetchHasRoster = false;
    g_iLooseBombRef = INVALID_ENT_REFERENCE;
    g_fDropPos[0] = 0.0;
    g_fDropPos[1] = 0.0;
    g_fDropPos[2] = 0.0;
}

/**
 * 事项 M9 抢包花名册的掉包定位：扫描无主 weapon_c4（m_hOwnerEntity == -1）并记录实体引用
 *
 * 找不到返回 false，调用方按 no-op 回原生放行（不等、不猜）。
 */
static bool Fetch_FindDropPos(float dropPos[3])
{
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "weapon_c4")) != -1)
    {
        if (GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") == -1)
        {
            GetEntPropVector(ent, Prop_Send, "m_vecOrigin", dropPos);
            g_iLooseBombRef = EntIndexToEntRef(ent);
            return true;
        }
    }
    return false;
}

/**
 * 优先通过已缓存的无主 weapon_c4 实体引用读取实时坐标，失效时回退扫描全图 weapon_c4。
 */
static bool Fetch_GetLiveDropPos(float dropPos[3])
{
    if (g_iLooseBombRef != INVALID_ENT_REFERENCE)
    {
        int ent = EntRefToEntIndex(g_iLooseBombRef);
        if (ent != INVALID_ENT_REFERENCE && IsValidEntity(ent)
            && GetEntPropEnt(ent, Prop_Send, "m_hOwnerEntity") == -1)
        {
            GetEntPropVector(ent, Prop_Send, "m_vecOrigin", dropPos);
            g_fDropPos = dropPos;
            return true;
        }
    }

    if (Fetch_FindDropPos(dropPos))
    {
        g_fDropPos = dropPos;
        return true;
    }

    return false;
}

/**
 * 更新并返回掉包点实时坐标；若场上暂不可见则保留最近一次已知坐标 g_fDropPos 兜底。
 */
static void Fetch_UpdateDropPos(float dropPos[3])
{
    if (!Fetch_GetLiveDropPos(dropPos))
    {
        dropPos = g_fDropPos;
    }
}

/**
 * 事项 M9 抢包花名册的蹲守承诺判定：task == 19 即视为蹲坑中
 *
 * task 是任务不是姿态：狙击手进 Hide 前 task 先落 19，所以这里能抓到"还没进 Hide 的蹲守承诺"
 * （MOVE_TO_SNIPER_SPOT，M12 自己也认这个值）；已经进 Hide 的不限 task 者由 Module15_IsBotHiding 兜底。
 * 偏移缺失/实体无效一律按"非蹲守"处理（宁可漏踢，不错杀）。
 *
 * @param bot           客户端索引
 * @return              true = 正蹲守承诺中，抢包指望不上它
 */
static bool Fetch_IsCampCommitted(int bot)
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

    return LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32) == BOT_TASK_MOVE_TO_SNIPER_SPOT;
}

/**
 * 事项 M9 抢包花名册的蹲守者解蹲：对名册里 task 19 的，一人一次强制 BotHunt
 *
 * 必须用 BotHunt 而非 BotIdle：HuntState::OnUpdate 首行即检查 NoticeLooseBomb()，
 * 而 IdleState::OnUpdate 中 NoticeLooseBomb 优先级排在狙击手 camp 与随机掷骰之后；
 * Hunt 进去 task 落 SEEK_AND_DESTROY，M12 不认（只认 19），交接干净。
 * 调用点保证帧上下文（选举/回补都在 RequestFrame 或事件里，不在钩子里），直调安全。
 *
 * @param bot           名册成员客户端索引
 * @return              true = 已发起强制 Hunt（调用方以此跳过 Hide 复检，避免连踢两次）
 */
static bool Fetch_ForceHuntIfCamping(int bot)
{
    if (!Fetch_IsCampCommitted(bot))
    {
        return false;
    }

    if (BotHunt(bot))
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N was camping (task 19), forced Hunt to fetch", bot);
    }
    return true;
}

/**
 * 事项 M9 抢包花名册的 Hide 解蹲：对名册里一切正处 HideState 者强制 BotHunt
 *
 * HideState::OnUpdate 不检查 NoticeLooseBomb()，不踢出即锁死；
 * 未处 Hide 时 no-op（复用 Module15_IsBotHiding 虚表真值门禁）。
 *
 * @param bot           名册成员客户端索引
 */
static void Fetch_ForceHuntIfHiding(int bot)
{
    if (!Module15_IsBotHiding(bot))
    {
        return;
    }

    if (BotHunt(bot))
    {
        DebugLog("[BotRouteFix] [Module 9] Roster %N was hiding (any task), forced Hunt to fetch", bot);
    }
}

/**
 * 事项 M9 抢包花名册的选举：掉包点真实最快地面行走距离（ROUTE_FASTEST）最近 3 存活 T bot
 *
 * 测距未就绪 / 找不到掉包 / 无存活 T bot 即空名册回原生（不拿欧氏混分，不可达跳过）。
 * 选完强制解蹲：task 19 蹲守承诺先踢（快道，含尚未进 Hide 者），一切正处 Hide 者再踢（虚表真值）。
 */
static void Fetch_ElectRoster()
{
    Fetch_ClearRoster();

    float dropPos[3];
    if (!Fetch_FindDropPos(dropPos))
    {
        DebugLog("[BotRouteFix] [Module 9] Bomb dropped but loose C4 not found, no-op to native");
        return;
    }
    g_fDropPos = dropPos;

    // 测距未就绪：无名单，全员放行回原生（不拿欧氏混分）
    if (!BotNavReady())
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] NavMeasure standby, no-op to native");
        return;
    }

    float bestDist[FETCH_ROSTER_SIZE];
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        bestDist[i] = 9999999.0;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true))
        {
            continue;
        }

        // ROUTE_FASTEST 实测地面直达距离，避开掉包交火处刚钉刺的危险度扭曲排序；不可达跳过
        float dist = BotNavWalkDistance(i, dropPos, ROUTE_FASTEST);
        if (dist < 0.0)
        {
            continue;
        }

        for (int slot = 0; slot < FETCH_ROSTER_SIZE; slot++)
        {
            if (dist < bestDist[slot])
            {
                for (int shift = FETCH_ROSTER_SIZE - 1; shift > slot; shift--)
                {
                    g_iFetchRoster[shift] = g_iFetchRoster[shift - 1];
                    bestDist[shift] = bestDist[shift - 1];
                }
                g_iFetchRoster[slot] = i;
                bestDist[slot] = dist;
                break;
            }
        }
    }

    if (g_iFetchRoster[0] == -1)
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] No alive reachable T bots to elect, no-op to native");
        return;
    }

    g_bFetchHasRoster = true;
    g_iTotalFetchElections++;
    DebugLog("[BotRouteFix] [Module 9] Elected fetch roster: %N / %N / %N",
        g_iFetchRoster[0],
        (g_iFetchRoster[1] != -1) ? g_iFetchRoster[1] : g_iFetchRoster[0],
        (g_iFetchRoster[2] != -1) ? g_iFetchRoster[2] : g_iFetchRoster[0]);

    // 蹲守者 / 隐藏者解蹲（HuntState 首行即检查 NoticeLooseBomb）
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] != -1)
        {
            if (!Fetch_ForceHuntIfCamping(g_iFetchRoster[i]))
            {
                Fetch_ForceHuntIfHiding(g_iFetchRoster[i]);
            }
        }
    }
}

/**
 * 除名 + 按实时掉落坐标就近回补（ROUTE_FASTEST）+ 全灭解散放行（死亡与掉线共用）
 */
static void Fetch_RemoveAndBackfill(int client, const char[] reason)
{
    if (!g_bFetchHasRoster || !Module9_IsInRoster(client))
    {
        return;
    }

    // 腾出槽位
    for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
    {
        if (g_iFetchRoster[i] == client)
        {
            g_iFetchRoster[i] = -1;
        }
    }

    float dropPos[3];
    Fetch_UpdateDropPos(dropPos);

    int best = -1;
    float bestDist = 9999999.0;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 2, true) || Module9_IsInRoster(i))
        {
            continue;
        }

        float dist = BotNavWalkDistance(i, dropPos, ROUTE_FASTEST);
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
        for (int i = 0; i < FETCH_ROSTER_SIZE; i++)
        {
            if (g_iFetchRoster[i] == -1)
            {
                g_iFetchRoster[i] = best;
                break;
            }
        }
        g_iTotalFetchElections++;
        DebugLog("[BotRouteFix] [Module 9] Roster backfill (%s): %N joins", reason, best);
        if (!Fetch_ForceHuntIfCamping(best))
        {
            Fetch_ForceHuntIfHiding(best);
        }
    }

    // 全灭则解散回原生（no-op 放行）
    if (Module9_GetRosterCount() == 0)
    {
        Fetch_ClearRoster();
        DebugLog("[BotRouteFix] [Module 9] Roster wiped, no-op to native");
    }
}

//========================================================================================
// MODULE 9 DETOUR & HOOKS (NOTICELOOSEBOMB::PRE)
//========================================================================================

/**
 * 事项 M9 抢包花名册的入口门控（NoticeLooseBomb Pre）
 *
 * ITEM_M9_FETCH 武装且名册有效时：
 *   - 非存活 T（含 CT）/ 名册为空 / 在册成员：MRES_Ignored 直通原生；
 *   - 远端非名册 T（距包 > 750.0 units）：MRES_Ignored 放行靠近掉包大方向；
 *   - 近端非名册 T（距包 <= 750.0 units）：DHookSetReturn(hParams, false) + MRES_Supercede，
 *     若在包旁接敌后退回 Hunt/Idle，阻止再入 FetchBombState 扎堆，令其延续正常状态机。
 *
 * 契约注记：底层 DHooks 在 0 参数 non-void 钩子下槽 2 实推 rHndl（DHookSetReturn 内部双认
 * g_HookReturnHandle 与 g_HookParamsHandle），此处保留 hParams 惯用标签防误调 DHookGetReturn，勿再改名。
 */
public MRESReturn Hook_NoticeLooseBomb_Pre(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M9_FETCH))
    {
        return MRES_Ignored;
    }

    if (!IsTeamBot(client, 2, true))
    {
        return MRES_Ignored;
    }

    if (!g_bFetchHasRoster || Module9_IsInRoster(client))
    {
        return MRES_Ignored;
    }

    float dropPos[3];
    Fetch_UpdateDropPos(dropPos);

    float botPos[3];
    GetClientAbsOrigin(client, botPos);
    if (GetVectorDistance(botPos, dropPos) > FETCH_GUARD_DISTANCE)
    {
        return MRES_Ignored;
    }

    DHookSetReturn(hParams, false);
    g_iTotalFetchGated++;
    // DebugLog("[BotRouteFix] [Module 9] Bot %N gated loose-bomb fetch within %.0fu (off-roster)", client, FETCH_GUARD_DISTANCE);
    return MRES_Supercede;
}

//========================================================================================
// MODULE 9 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用抢包事项（ITEM_M9_FETCH）：挂载 NoticeLooseBomb Pre，并尽力同步可选 HideState::OnEnter 指纹宿主。
 *
 * 失败语义：NoticeLooseBomb 启用失败清 Armed 保留 Ready，供下次 Enable 重试；
 * HideState::OnEnter 宿主属于 C 类可选增强，缺失或挂钩失败时仅放弃 Hide 指纹捕获，不连坐回滚 M9。
 *
 * @return              true = 抢包门控已驻留
 */
static bool ApplyModule9_FetchBackend()
{
    if (!Item_IsReady(ITEM_M9_FETCH) || g_hLooseDetour == null)
    {
        Item_SetArmed(ITEM_M9_FETCH, false);
        LogError("[BotRouteFix] [Module 9] Fetch backend unavailable (ready=%d, loose=%d); fetch item unarmed.",
            Item_IsReady(ITEM_M9_FETCH), g_hLooseDetour != null);
        return false;
    }

    if (!SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked,
        false, Hook_NoticeLooseBomb_Pre, true, "Module 9"))
    {
        Item_SetArmed(ITEM_M9_FETCH, false);
        LogError("[BotRouteFix] [Module 9] NoticeLooseBomb detour enable failed; M9 fetch item unarmed.");
        return false;
    }

    // 先临时置位再尽力同步共享宿主：HideState::OnEnter 仅供首次目击捕获 g_pHideStateVTable；
    // SyncDetourEnabled 对 hook == null 静默安全，宿主缺失或挂钩失败时不回滚 M9 抢包门控。
    Item_SetArmed(ITEM_M9_FETCH, true);
    SyncHideDurationDetourState();

    // 仅在服务帧已启动后检查冻结期（规避冷启动 OnPluginStart 时 GameRules 尚未创建报错）；
    // 非冻结期内中途武装且场上已有无主 C4 时立即补选（冻结期对称延后至 round_freeze_end）
    if (IsServerProcessing() && GameRules_GetProp("m_bFreezePeriod") == 0)
    {
        Fetch_ElectRoster();
    }

    return true;
}

/**
 * 还原抢包事项（ITEM_M9_FETCH）：摘钩、清空名册（= 全体放行回原生）、解除武装并收敛宿主钩子。
 *
 * 失败语义：无（幂等）。
 */
static void RestoreModule9_FetchBackend()
{
    SyncDetourEnabled(g_hLooseDetour, g_bLooseHooked, false, Hook_NoticeLooseBomb_Pre, false, "Module 9");
    Fetch_ClearRoster();
    Item_SetArmed(ITEM_M9_FETCH, false);
    SyncHideDurationDetourState();
}
