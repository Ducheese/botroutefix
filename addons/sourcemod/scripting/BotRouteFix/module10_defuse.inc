#if defined _botroutefix_module10_defuse_included
    #endinput
#endif
#define _botroutefix_module10_defuse_included

//========================================================================================
// MODULE 10: CT TOP-3 DEFUSE ROSTER (IsDoingScenario pre-hook + event election)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生全员涌向雷包 (cs_bot_idle.cpp:250-281):
//    下包后在首人按 E 读条（GetBombDefuser() 置位）前，IsDoingScenario() == true 且已感知包点的 CT
//    全员拿 DEFUSE_BOMB 沿 FASTEST_ROUTE 走向同一包点坐标，人多回防时不预先分兵架掩体。
//    （首人开始拆包后，其余视野内队友会在 MoveToState:170 转入 GUARD_BOMB_DEFUSER 掩护。）
// 2. 与 M9 掉包 Top-3 花名册对称的节流门控：
//    本模块 Pre 钩 CCSBot::IsDoingScenario()（cs_bot.cpp:411-421，64 位 sub_18032B2D0：
//    bot_defer_to_human + GetTeamNumber + UTIL_HumansOnTeam）：
//    - 名册内 Top-3 CT（nav 真实行走距离 − 持钳 500 评分最低的前 3 人）放行直通原生（true）：
//      第 1 人到达按 E 读条后，另 2 人按 MoveTo:170 / Idle:257 自动转为 GUARD_BOMB_DEFUSER 掩护；
//      前锋遇敌受阻时 2/3 号无需等待即可直接接力拆包，彻底消除单首席被拖住卡死包旁队友的问题；
//    - 远端非名册 CT（距雷包 > 750 码）放行直通原生（true）：
//      原因 ① HideState:147 只在 IsDoingScenario()==true 时把原处蹲守的远端 CT 踢回 Idle() 冲包；
//      原因 ② HideState:284 在 GUARD_BOMB_ZONE 计时到期时按 GetClosestZone(myOrigin) 重抽最近包点
//      （极易锁在错误半区原处无限续蹲），且无掩体点时会原地蹲下。故远端必须放行推向包点；
//    - 近端非名册 CT（距雷包 <= 750 码）掐成 false 掉进 Idle:276 else——GUARD_BOMB_ZONE
//      （Hide 包点 750 码物理半径内真掩体 + OPPORTUNITY_FIRE），并回落 M5 软豁免。
// 3. 名册自愈与空名册放行（全事件驱动，无 timer）：
//    - bomb_planted：planted_c4 在事件触发前已生成落位（weapon_c4.cpp:1070-1114），同步以 C4 坐标
//      无损实测（BotNavWalkDistance, ROUTE_FASTEST）取评分最低的前 3 名存活可达 CT；
//      选举失败 / 无候选 / 测距未就绪 → 空名册 = 全员放行回原生；
//    - player_death / client_disconnect：名册减员立即按缓存包坐标就近递补下一顺位；
//      刚递补进名册的原 GUARD_BOMB_ZONE 队友会在下一帧由 HideState:147 原生唤醒切入 Idle()->DEFUSE_BOMB
//      （无需额外 SDKCall 踢蹲）；名册打空则解散回原生，杜绝死锁；
//    - bomb_defused / bomb_exploded / round_start / OnMapStart 清册。
// 4. 与真人玩家的关系：
//    选举只计存活 CT bot，出动尊重原生 bot_defer_to_human——defer 开且有活人时名册成员原生同样返回 false，
//    真人全部阵亡后原生自动翻 true，名册成员无需重选立即接管；defer 关或有真人抢拆时原生让路不冲突。
//========================================================================================

//========================================================================================
// MODULE 10 DEFINES & GLOBALS
//========================================================================================

#define DEFUSE_ROSTER_SIZE 3

// 持钳加成距离（等效缩短 5 秒拆包时间让分）
#define DEFUSE_KIT_BONUS 500.0

// 包点掩护与警戒判决半径（物理对齐引擎 cs_bot.h:462 / bot_hide.cpp:89 的 750.0 码掩体搜索阈值）
#define DEFUSE_GUARD_DISTANCE 750.0

// 本模块私有状态（static，名册与句柄不出本文件）
static bool g_bModule10_Active = false;     // 业务开启态
static bool g_bScenarioHooked = false;      // per-hook 真实启用态
static Handle g_hScenarioDetour = null;
static int g_iDefuseRoster[DEFUSE_ROSTER_SIZE] = {-1, -1, -1};
static bool g_bDefuseHasRoster = false;
static float g_fBombPos[3];

// 遥测统计计数器
static int g_iTotalDefuseElections = 0;
static int g_iTotalScenarioGated = 0;

//========================================================================================
// MODULE 10 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：Top-3 拆包花名册（ITEM_M10_DEFUSE）
 *
 * 只创建 Detour、绑定签名、校验测距子系统，不启用钩子（启用由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把拆包事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 拆包事项后端就绪
 */
static bool PrepModule10_DefuseBackend()
{
    g_hScenarioDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hScenarioDetour == null)
    {
        LogError("[BotRouteFix] [Module 10] IsDoingScenario detour creation failed; defuse item FAILED.");
        return false;
    }

    if (!DHookSetFromConf(g_hScenarioDetour, g_hGameConf, SDKConf_Signature, "CCSBot_IsDoingScenario"))
    {
        LogError("[BotRouteFix] [Module 10] IsDoingScenario signature missing; defuse item FAILED.");
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
        return false;
    }

    if (!BotNavReady())
    {
        LogError("[BotRouteFix] [Module 10] NavMeasure unavailable; defuse item FAILED (no empty-roster fail-open).");
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
        return false;
    }

    // IsDoingScenario(void)：无参数，this 即 bot 本体
    return true;
}


//========================================================================================
// MODULE 10 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 10 的唯一事项：Top-3 拆包花名册（ITEM_M10_DEFUSE），挂 IsDoingScenario Pre。
 * 签名缺失时优雅待机（不阻断全库启动），与模块 8/9 同策略。
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 */
bool InitModule10_Defuse()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 10] GameConf missing; defuse item FAILED.");
        Item_SetReady(ITEM_M10_DEFUSE, false);
        return false;
    }

    Item_SetReady(ITEM_M10_DEFUSE, PrepModule10_DefuseBackend());
    EnableModule10_Defuse(true);
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        LogModuleAllFailed(10);
        return false;
    }

    LogModuleArmed(10, "Defuse", "1");
    return true;
}

/**
 * 动态启用或停用模块 10（事项 ITEM_M10_DEFUSE 的调度入口；幂等；停用即清名册 = 全员放行回原生）。
 *
 * 只做调度：只武装 Ready 的事项；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 */
void EnableModule10_Defuse(bool enable)
{
    if (enable == g_bModule10_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule10_DefuseBackend();
        g_bModule10_Active = false;
        return;
    }

    g_bModule10_Active = ApplyModule10_DefuseBackend();
}

/**
 * 还原模块 10（事项 ITEM_M10_DEFUSE；插件卸载：摘钩并释放句柄）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule10_Defuse()
{
    EnableModule10_Defuse(false);

    if (g_hScenarioDetour != null)
    {
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 10] Defuse hooks safely restored.");
}


//========================================================================================
// MODULE 10 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M10 拆包花名册（ITEM_M10_DEFUSE）的名单查询（O(DEFUSE_ROSTER_SIZE) 内存判定）
 *
 * 失败语义：无（只读查询，输入无效返回 false，不判定事项成败）。
 */
bool Module10_IsInRoster(int client)
{
    if (client <= 0)
    {
        return false;
    }

    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        if (g_iDefuseRoster[i] == client)
        {
            return true;
        }
    }
    return false;
}

/**
 * 事项 ITEM_M10_DEFUSE 只读遥测：获取当前拆包名册有效人数
 *
 * 失败语义：无（只读计数，名册为空返回 0，不判定事项成败）。
 */
int Module10_GetRosterCount()
{
    int count = 0;
    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        if (g_iDefuseRoster[i] > 0 && IsTeamBot(g_iDefuseRoster[i], 3, true))
        {
            count++;
        }
    }
    return count;
}

/**
 * 事项 ITEM_M10_DEFUSE 只读遥测：获取拆包花名册容量上限
 *
 * 失败语义：无（纯常量返回，不判定事项成败）。
 */
int Module10_GetRosterSize()
{
    return DEFUSE_ROSTER_SIZE;
}

/**
 * 事项 ITEM_M10_DEFUSE 只读遥测：获取累计选举/递补次数
 *
 * 失败语义：无（只读计数，不判定事项成败）。
 */
int Module10_GetTotalElections()
{
    return g_iTotalDefuseElections;
}

/**
 * 事项 ITEM_M10_DEFUSE 只读遥测：获取累计拦截非名册近点拆包次数
 *
 * 失败语义：无（只读计数，不判定事项成败）。
 */
int Module10_GetTotalGated()
{
    return g_iTotalScenarioGated;
}

/**
 * 事项 ITEM_M10_DEFUSE 跨模块可视化读取：获取当前拆包花名册 CT 列表与已安放 C4 坐标
 *
 * 失败语义：名册无效、未下包或找不到 planted_c4 返回 0，调用方不绘制，不判定事项成败。
 *
 * @param roster        输出候选人 client 索引数组 (容量 DEFUSE_ROSTER_SIZE)
 * @param c4Pos         输出已安放 C4 的世界坐标
 * @return              实际有效候选人数量
 */
int Module10_GetDefuseRoster(int roster[DEFUSE_ROSTER_SIZE], float c4Pos[3])
{
    if (!g_bDefuseHasRoster || !g_bBombPlanted)
    {
        return 0;
    }

    if (!Defuse_ReadBombPos())
    {
        return 0;
    }

    c4Pos[0] = g_fBombPos[0];
    c4Pos[1] = g_fBombPos[1];
    c4Pos[2] = g_fBombPos[2];

    int count = 0;
    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        int bot = g_iDefuseRoster[i];
        if (IsTeamBot(bot, 3, true))
        {
            roster[count++] = bot;
        }
    }
    return count;
}


//========================================================================================
// MODULE 10 EVENT HANDLERS
//========================================================================================

/**
 * 事项 ITEM_M10_DEFUSE 下包事件入口：选举 Top-3 拆包名册
 *
 * 未武装时首行自守直接返回；找不到 planted_c4 / 测距未就绪 / 全员不可达保持空名册（全员放行回原生）。
 */
void Module10_OnBombPlanted()
{
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        return;
    }

    Defuse_ElectRoster();
}

/**
 * 事项 ITEM_M10_DEFUSE 死亡事件入口：名册减员立即按缓存包坐标递补
 *
 * 失败语义：未武装或非名册成员时直接返回；无可用递补或剩余均失效则清册放行回原生，不阻断拆包。
 */
void Module10_OnPlayerDeath(int victim)
{
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        return;
    }

    Defuse_RemoveAndBackfill(victim, "fallen member");
}

/**
 * 事项 ITEM_M10_DEFUSE 掉线事件入口（OnClientDisconnect）：名册减员立即递补
 *
 * 失败语义：未武装或非名册成员时直接返回；无可用递补或剩余均失效则清册放行回原生，不阻断拆包。
 */
void Module10_OnClientDisconnect(int client)
{
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        return;
    }

    Defuse_RemoveAndBackfill(client, "disconnect");
}

/**
 * 事项 ITEM_M10_DEFUSE 拆包结束事件入口：清空名册（纯幂等清理，无业务副作用，天然豁免武装门卫）
 *
 * 失败语义：无（幂等置空）。
 */
void Module10_OnBombDefused()
{
    Defuse_ClearRoster();
}

/**
 * 事项 ITEM_M10_DEFUSE C4 爆炸事件入口：清空名册（纯幂等清理，天然豁免武装门卫）
 *
 * 失败语义：无（幂等置空）。
 */
void Module10_OnBombExploded()
{
    Defuse_ClearRoster();
}

/**
 * 事项 ITEM_M10_DEFUSE 回合开始事件入口：清空名册（纯幂等清理，天然豁免武装门卫）
 *
 * 失败语义：无（幂等置空）。
 */
void Module10_OnRoundStart()
{
    Defuse_ClearRoster();
}

/**
 * 事项 ITEM_M10_DEFUSE 地图加载事件入口（OnMapStart）：实体索引全废，清空名册（纯幂等清理，天然豁免武装门卫）
 *
 * 失败语义：无（幂等置空）。
 */
void Module10_OnMapStart()
{
    Defuse_ClearRoster();
}


//========================================================================================
// MODULE 10 ROSTER PRIMITIVES
//========================================================================================

/**
 * 事项 ITEM_M10_DEFUSE 内部原语：清空拆包花名册（无名册 = 全员放行回原生）
 *
 * 失败语义：无（幂等重置名册数组与缓存坐标）。
 */
static void Defuse_ClearRoster()
{
    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        g_iDefuseRoster[i] = -1;
    }
    g_bDefuseHasRoster = false;
    g_fBombPos[0] = 0.0;
    g_fBombPos[1] = 0.0;
    g_fBombPos[2] = 0.0;
}

/**
 * 事项 ITEM_M10_DEFUSE 内部原语：读取已安放 C4 坐标（下包后 planted_c4 实体一直在，零耦合不读别模块）
 *
 * 失败语义：找不到 planted_c4 返回 false，调用方按空名册/无包放行，不判定事项成败。
 */
static bool Defuse_ReadBombPos()
{
    int ent = -1;
    while ((ent = FindEntityByClassname(ent, "planted_c4")) != -1)
    {
        GetEntPropVector(ent, Prop_Send, "m_vecOrigin", g_fBombPos);
        return true;
    }
    return false;
}

/**
 * 事项 ITEM_M10_DEFUSE 内部原语：评估候选 CT 拆包到达评分（真实地面最快行走距离 − 持钳 500 码加成）
 *
 * 注意：持有拆包钳且距包 < 500 码时评分为负，不可用 < 0.0 判无效，故显式返回 bool。
 * 失败语义：非存活 CT bot 或寻路不可达返回 false，跳过该候选人不参评，不判定事项成败。
 *
 * @param client        候选客户端索引
 * @param score         输出评分（越小越优先）
 * @return              true = 存活 CT bot 且寻路可达
 */
static bool Defuse_TryScoreCandidate(int client, float &score)
{
    if (!IsTeamBot(client, 3, true))
    {
        return false;
    }

    // 真实地面行走距离选人（ROUTE_FASTEST 纯拼最快直达路线；持钳加分，避免到不了的人靠钳当选）
    float walk = BotNavWalkDistance(client, g_fBombPos, ROUTE_FASTEST);
    if (walk < 0.0)
    {
        return false;
    }

    score = walk;
    if (GetEntProp(client, Prop_Send, "m_bHasDefuser") == 1)
    {
        score -= DEFUSE_KIT_BONUS;
    }
    return true;
}

/**
 * 事项 ITEM_M10_DEFUSE 内部原语：选举 Top-3 拆包花名册（按评分取前 3 名存活可达 CT bot）
 *
 * 失败语义：找不到 planted_c4 / 测距未就绪 / 全员不可达时保持空名册放行回原生，不判定事项成败。
 */
static void Defuse_ElectRoster()
{
    Defuse_ClearRoster();

    if (!Defuse_ReadBombPos())
    {
        DebugLog("[BotRouteFix] [Module 10] Bomb planted but planted_c4 not found, no-op to native");
        return;
    }

    if (!BotNavReady())
    {
        Defuse_ClearRoster();
        DebugLog("[BotRouteFix] [Module 10] NavMeasure standby, no-op to native");
        return;
    }

    float bestScore[DEFUSE_ROSTER_SIZE];
    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        bestScore[i] = 9999999.0;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        float score;
        if (!Defuse_TryScoreCandidate(i, score))
        {
            continue;
        }

        for (int slot = 0; slot < DEFUSE_ROSTER_SIZE; slot++)
        {
            if (score < bestScore[slot])
            {
                for (int shift = DEFUSE_ROSTER_SIZE - 1; shift > slot; shift--)
                {
                    g_iDefuseRoster[shift] = g_iDefuseRoster[shift - 1];
                    bestScore[shift] = bestScore[shift - 1];
                }
                g_iDefuseRoster[slot] = i;
                bestScore[slot] = score;
                break;
            }
        }
    }

    if (g_iDefuseRoster[0] == -1)
    {
        DebugLog("[BotRouteFix] [Module 10] No reachable CT bots to elect, no-op to native");
        return;
    }

    g_bDefuseHasRoster = true;
    g_iTotalDefuseElections++;
    DebugLog("[BotRouteFix] [Module 10] Elected defuse roster: %N / %N / %N",
        g_iDefuseRoster[0],
        (g_iDefuseRoster[1] != -1) ? g_iDefuseRoster[1] : g_iDefuseRoster[0],
        (g_iDefuseRoster[2] != -1) ? g_iDefuseRoster[2] : g_iDefuseRoster[0]);
}

/**
 * 事项 ITEM_M10_DEFUSE 内部原语：除名 + 按缓存包坐标就近递补 + 全灭解散放行（死亡与掉线共用）
 *
 * 名单有效时 C4 坐标固定于 g_fBombPos；递补进名册者若此前正处 GUARD_BOMB_ZONE HideState，
 * 下一帧 HideState::OnUpdate:147 会因 IsDoingScenario() 翻回 true 而原生踢回 Idle()->DEFUSE_BOMB。
 * 失败语义：测距未就绪时跳过回补；无可用后补且名册减空时解散回原生（no-op），不卡死拆包。
 */
static void Defuse_RemoveAndBackfill(int client, const char[] reason)
{
    if (!g_bDefuseHasRoster || !g_bBombPlanted || !Module10_IsInRoster(client))
    {
        return;
    }

    // 腾出槽位
    for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
    {
        if (g_iDefuseRoster[i] == client)
        {
            g_iDefuseRoster[i] = -1;
        }
    }

    if (BotNavReady())
    {
        int best = -1;
        bool hasBest = false;
        float bestScore = 0.0;

        for (int i = 1; i <= MaxClients; i++)
        {
            if (Module10_IsInRoster(i))
            {
                continue;
            }

            float score;
            if (!Defuse_TryScoreCandidate(i, score))
            {
                continue;
            }

            if (!hasBest || score < bestScore)
            {
                hasBest = true;
                bestScore = score;
                best = i;
            }
        }

        if (best != -1)
        {
            for (int i = 0; i < DEFUSE_ROSTER_SIZE; i++)
            {
                if (g_iDefuseRoster[i] == -1)
                {
                    g_iDefuseRoster[i] = best;
                    break;
                }
            }
            g_iTotalDefuseElections++;
            DebugLog("[BotRouteFix] [Module 10] Roster backfill (%s): %N joins defuse", reason, best);
        }
    }

    // 全灭或剩余成员均失效 → 解散回原生（no-op 放行）
    if (Module10_GetRosterCount() == 0)
    {
        Defuse_ClearRoster();
        DebugLog("[BotRouteFix] [Module 10] Defuse roster wiped, no-op to native");
    }
}


//========================================================================================
// MODULE 10 DETOUR & HOOKS (ISDOINGSCENARIO::PRE)
//========================================================================================

/**
 * 事项 M10 Top-3 拆包花名册的唯一入口钩子（IsDoingScenario Pre）
 *
 * ITEM_M10_DEFUSE 武装、包已下、名册有效、来者非名册成员且已在包点 750 码半径内时才掐成 false
 * （近端非名册掉进 Idle:276 Hide 分支守包点）；名册成员与远端（> 750 码）一律 MRES_Ignored 直通原生。
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * 契约注记：底层 DHooks 在 0 参数 non-void 钩子下槽 2 实推 rHndl（DHookSetReturn 内部双认
 * g_HookReturnHandle 与 g_HookParamsHandle），此处保留 hParams 惯用标签防误调 DHookGetReturn，勿再改名。
 */
public MRESReturn Hook_IsDoingScenario_Pre(int client, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        return MRES_Ignored;
    }

    if (!IsTeamBot(client, 3, true))
    {
        return MRES_Ignored;
    }

    // 包没下 / 名册未建立（空名册）→ 直通原生
    if (!g_bBombPlanted || !g_bDefuseHasRoster)
    {
        return MRES_Ignored;
    }

    // 名册成员直通原生：defer 关照去；defer 开且有真人玩家，原生 false 按板凳；
    // 真人玩家死光，原生翻 true，名册成员自动上——无需重选
    if (Module10_IsInRoster(client))
    {
        return MRES_Ignored;
    }

    // 远端 CT（距雷包 > 750 码）放行直通原生：确保 HideState:147 唤醒他处蹲守 CT 并朝下包点推进，
    // 规避 GUARD_BOMB_ZONE 在 HideState:284 按 GetClosestZone(myOrigin) 锁在原半区续蹲的问题
    float botPos[3];
    GetClientAbsOrigin(client, botPos);
    if (GetVectorDistance(botPos, g_fBombPos) > DEFUSE_GUARD_DISTANCE)
    {
        return MRES_Ignored;
    }

    // 近端（<= 750 码）非名册掩护组拦截：就地转入包点真掩体警戒掩护
    DHookSetReturn(hParams, false);
    g_iTotalScenarioGated++;
    // DebugLog("[BotRouteFix] [Module 10] Bot %N held at GUARD_BOMB_ZONE (off-roster)", client);
    return MRES_Supercede;
}


//========================================================================================
// MODULE 10 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用拆包事项：只有 Prep 已就绪才启用 Pre 钩子
 *
 * 事项 ITEM_M10_DEFUSE 的应用入口；不做 Detour 创建与签名绑定（那是 Prep 的职责）。
 * 失败时该事项 FAILED。
 * 失败语义：钩子启用失败只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = 拆包钩子已驻留
 */
static bool ApplyModule10_DefuseBackend()
{
    if (!Item_IsReady(ITEM_M10_DEFUSE) || g_hScenarioDetour == null)
    {
        Item_SetArmed(ITEM_M10_DEFUSE, false);
        return false;
    }

    if (!SyncDetourEnabled(g_hScenarioDetour, g_bScenarioHooked,
        false, Hook_IsDoingScenario_Pre, true, "Module 10"))
    {
        // 挂钩失败属动态环境抖动，不证伪探测结论：只清 Armed 保留 Ready，下次 Enable 重试
        Item_SetArmed(ITEM_M10_DEFUSE, false);
        LogError("[BotRouteFix] [Module 10] Defuse detour enable failed; defuse item unarmed.");
        return false;
    }

    Item_SetArmed(ITEM_M10_DEFUSE, true);
    return true;
}

/**
 * 还原拆包事项（ITEM_M10_DEFUSE）：摘钩、清名册（= 全员放行回原生）并解除武装。
 *
 * 失败语义：无（幂等；摘钩 + 清名册 + Armed(false)）。
 */
static void RestoreModule10_DefuseBackend()
{
    SyncDetourEnabled(g_hScenarioDetour, g_bScenarioHooked, false, Hook_IsDoingScenario_Pre, false, "Module 10");
    Defuse_ClearRoster();
    Item_SetArmed(ITEM_M10_DEFUSE, false);
}
