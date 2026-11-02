#if defined _botroutefix_module10_defuse_included
    #endinput
#endif
#define _botroutefix_module10_defuse_included

//========================================================================================
// MODULE 10: CT CHIEF DEFUSER (IsDoingScenario pre-hook + event election)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生缺陷 (cs_bot_idle.cpp:250-281):
//    下包后无 defuser 时，IsDoingScenario() == true 的 CT 全员拿 DEFUSE_BOMB 往雷包上撞，
//    直到第一个人读上条（m_bombDefuser 置位）其余才翻成 GUARD_BOMB_DEFUSER。撞墙窗口 = 全员旅行时间。
// 2. 卡门：IsDoingScenario() 本体就是"defer 检查 + return true"（cs_bot.cpp:411-421，
//    64 位 sub_18032B290：convar 内联读 + GetTeamNumber + UTIL_HumansOnTeam，n=1 验证）。
//    非首席掐成 false 即掉进 276 行 else——"不让拆就守包点"（Hide 包点 + OPPORTUNITY_FIRE），
//    掩护组引擎自建。首席读上条后 257 行原生把众人翻成 GUARD_BOMB_DEFUSER。
// 3. 字面"提前转 GUARD_BOMB_DEFUSER"不可行：HideState:125 在 defuser==NULL 时每拍 Idle() 弹回，
//    自旋抖动。故掩护组拿 ZONE task（行为等价），名字晚到几秒。
// 4. 调用点审计（掐 false 无行为副作用）：
//    - Hide:337 → 纯 Encourage 喊话（3-15 秒节流），进就进了；
//    - Idle:775 → 人质图分支，de_ 图 planted 为真时到不了；
//    - 其余（Hunt:102/MoveTo:170/Hide:147）只读 true 分支做退出，false 即"继续蹲"，正是要的。
// 5. 首席选举（全事件，无 timer）：bomb_planted 选距包最近存活 CT bot（行走距离 − 持钳）；
//    player_death / client_disconnect 首席倒地或掉线立即按缓存包坐标重选（planted_c4 实体下包后一直在）；
//    defused / exploded / round_start / OnMapStart 清。
//    选举无视 bot_defer_to_human，出动尊重原生：首席在钩子里直通——defer 关照去，真人玩家抢拆则
//    原生让路；defer 开且有真人玩家，首席坐板凳，真人玩家死光原生翻 true 自动上，无需重选。
// 全程事件驱动，无 timer 轮询。
//========================================================================================

//========================================================================================
// MODULE 10 DEFINES & GLOBALS
//========================================================================================

// 本模块私有状态（static，首席与句柄不出本文件）
static bool g_bModule10_Active = false;     // 业务开启态
static bool g_bScenarioHooked = false;      // per-hook 真实启用态
static Handle g_hScenarioDetour = null;
static int g_iChief = -1;
static float g_fBombPos[3];

// 持钳加成距离
#define DEFUSE_KIT_BONUS 500.0

// 遥测统计计数器
static int g_iTotalChiefs = 0;
static int g_iTotalScenarioGated = 0;

//========================================================================================
// MODULE 10 ITEM BACKEND: CHIEF DEFUSER (IsDoingScenario pre-hook + NavMeasure)
//========================================================================================

/**
 * 事项后端：首席拆包员（ITEM_M10_DEFUSE）
 *
 * 只创建 Detour、绑定签名、校验测距子系统，不启用钩子（启用由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把拆包事项标记为 FAILED。
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
 * 初始化模块 10（挂 IsDoingScenario Pre）
 * 签名缺失时优雅待机（不阻断全库启动），与模块 8 同策略
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

    LogMessage("[BotRouteFix] [Module 10] CT chief defuser item armed=%d (IsDoingScenario Pre hook + event election)",
        Item_IsArmed(ITEM_M10_DEFUSE));
    return g_bModule10_Active;
}

/**
 * 动态启用或停用模块 10（幂等；停用即清首席 = 全员放行回原生）
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
 * 还原模块 10（插件卸载：摘钩并释放句柄）
 */
void RestoreModule10_Defuse()
{
    EnableModule10_Defuse(false);

    if (g_hScenarioDetour != null)
    {
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
    }
}


//========================================================================================
// MODULE 10 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 获取累计首席选举次数
 */
int Module10_GetTotalChiefs()
{
    return g_iTotalChiefs;
}

/**
 * 获取累计拦截非首席拆包次数
 */
int Module10_GetTotalGated()
{
    return g_iTotalScenarioGated;
}

/**
 * 获取当前首席拆包 CT 与已安放 C4 坐标 (供可视化子系统渲染拆包指引光束)
 *
 * @param c4Pos         输出已安放 C4 的世界坐标
 * @return              首席 CT client 索引 (若无首席或 C4 未安放则返回 -1)
 */
int Module10_GetChiefDefuser(float c4Pos[3])
{
    if (g_iChief <= 0 || g_iChief > MaxClients || !IsClientInGame(g_iChief) || !IsPlayerAlive(g_iChief))
    {
        return -1;
    }

    if (!Defuse_ReadBombPos())
    {
        return -1;
    }

    c4Pos[0] = g_fBombPos[0];
    c4Pos[1] = g_fBombPos[1];
    c4Pos[2] = g_fBombPos[2];
    return g_iChief;
}


//========================================================================================
// MODULE 10 EVENT HANDLERS
//========================================================================================

/**
 * 下包事件：选举首席
 */
void Module10_OnBombPlanted()
{
    if (!Item_IsArmed(ITEM_M10_DEFUSE))
    {
        return;
    }

    Defuse_ElectChief();
}

/**
 * 死亡事件：首席倒地立即按缓存包坐标接力
 */
void Module10_OnPlayerDeath(int victim)
{
    if (victim != g_iChief)
    {
        return;
    }

    Defuse_RelayChief();
}

/**
 * 客户端掉线 (OnClientDisconnect)：无死亡事件，首席掉线不断档
 */
void Module10_OnClientDisconnect(int client)
{
    if (client != g_iChief)
    {
        return;
    }

    Defuse_RelayChief();
}

/**
 * 拆包/爆炸/开局：清首席
 */
void Module10_OnBombDefused()
{
    g_iChief = -1;
}

/**
 * 事项 M10 首席拆包的 C4 爆炸处理：清首席，确保换回合不拿旧索引当新人
 */
void Module10_OnBombExploded()
{
    g_iChief = -1;
}

/**
 * 事项 M10 首席拆包的回合开始处理：清首席（与 Defused / Exploded / MapStart 同策）
 */
void Module10_OnRoundStart()
{
    g_iChief = -1;
}

/**
 * 地图加载 (OnMapStart)：首席索引随实体全废，必须清
 */
void Module10_OnMapStart()
{
    g_iChief = -1;
}


//========================================================================================
// MODULE 10 DETOUR & HOOKS (ISDOINGSCENARIO::PRE)
//========================================================================================

/**
 * 事项 M10 首席拆包的唯一入口钩子（IsDoingScenario Pre）
 *
 * ITEM_M10_DEFUSE 武装、包已下、首席在位且来者非首席时才掐成 false
 * （掩护组掉进 Hide 分支自建掩护）；其余一律 MRES_Ignored 直通原生。
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

    // 包没下/首席缺位（无候选）→ 直通原生
    if (!g_bBombPlanted || g_iChief == -1)
    {
        return MRES_Ignored;
    }

    // 首席直通原生：defer 关照去；defer 开且有真人玩家，原生 false 按板凳；
    // 真人玩家死光，原生翻 true，首席自动上——无需重选
    if (client == g_iChief)
    {
        return MRES_Ignored;
    }

    DHookSetReturn(hParams, false);
    g_iTotalScenarioGated++;
    // DebugLog("[BotRouteFix] [Module 10] Bot %N held at GUARD_BOMB_ZONE (chief %N goes)", client, g_iChief);
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
 * 还原拆包事项：摘钩、清首席（= 全员放行回原生）并解除武装
 */
static void RestoreModule10_DefuseBackend()
{
    SyncDetourEnabled(g_hScenarioDetour, g_bScenarioHooked, false, Hook_IsDoingScenario_Pre, false, "Module 10");
    g_iChief = -1;
    Item_SetArmed(ITEM_M10_DEFUSE, false);
}


//========================================================================================
// MODULE 10 ELECTION PRIMITIVES
//========================================================================================

/**
 * 事项 M10 首席拆包的包坐标读取：下包后 planted_c4 实体一直在，零耦合不读别模块
 *
 * 找不到返回 false，调用方按无包处理（首席 -1，钩子直通）。
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
 * 事项 M10 首席拆包的选举：在存活 CT bot 里选真实地面行走距离最近者（持钳减分）
 *
 * 寻路不可达直接跳过不参评；测距未就绪 / 全员不可达返回 -1（无首席全员放行）。
 */
static int Defuse_SelectBest()
{
    if (!BotNavReady())
    {
        return -1;
    }

    int best = -1;
    float bestScore = 9999999.0;
    bool anyReachable = false;
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsTeamBot(i, 3, true))
        {
            continue;
        }

        // 寻路不可达直接跳过不参评（持钳也不加分，避免到不了的人靠钳当选）
        float walk = BotNavWalkDistance(i, g_fBombPos);
        if (walk < 0.0)
        {
            continue;
        }
        anyReachable = true;

        float score = walk;
        if (GetEntProp(i, Prop_Send, "m_bHasDefuser") == 1)
        {
            score -= DEFUSE_KIT_BONUS;
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }

    // 全员不可达：无首席，全员放行回原生（不断谁的 scenario 逻辑）
    if (!anyReachable)
    {
        return -1;
    }
    return best;
}

/**
 * 事项 M10 首席拆包的选举入口：选首席；无候选/无包则首席 -1（钩子直通回原生）
 */
static void Defuse_ElectChief()
{
    g_iChief = -1;

    if (!Defuse_ReadBombPos())
    {
        return;
    }

    int best = Defuse_SelectBest();
    if (best != -1)
    {
        g_iChief = best;
        g_iTotalChiefs++;
        DebugLog("[BotRouteFix] [Module 10] Elected chief defuser: %N", best);
    }
}

/**
 * 首席接力（死亡与掉线共用）：按缓存包坐标选下一顺位
 */
static void Defuse_RelayChief()
{
    g_iChief = -1;

    if (!g_bBombPlanted)
    {
        return;
    }

    int best = Defuse_SelectBest();
    if (best != -1)
    {
        g_iChief = best;
        g_iTotalChiefs++;
        DebugLog("[BotRouteFix] [Module 10] Chief relay: %N takes over defuse", best);
    }
}
