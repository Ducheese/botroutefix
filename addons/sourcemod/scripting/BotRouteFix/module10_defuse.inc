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
// 5. 首席选举（全事件，无 timer）：bomb_planted 选距包最近存活 CT bot（行走距离 − 持钳）；player_death 首席倒地
//    立即按缓存包坐标重选（planted_c4 实体下包后一直在）；defused/exploded/round_start 清。
//    选举无视 bot_defer_to_human，出动尊重原生：首席在钩子里直通——defer 关照去，真人玩家抢拆则
//    原生让路；defer 开且有真人玩家，首席坐板凳，真人玩家死光原生翻 true 自动上，无需重选。
// 全程事件驱动，无 timer 轮询。
//========================================================================================

// 本模块私有状态（static，首席与句柄不出本文件）
static Handle g_hScenarioDetour = null;
static int g_iChief = -1;
static float g_fBombPos[3];

// 持钳加成距离
#define DEFUSE_KIT_BONUS 500.0

// 遥测统计计数器
static int g_iTotalChiefs = 0;
static int g_iTotalScenarioGated = 0;

// 读包坐标（下包后 planted_c4 实体一直在，零耦合不读别模块）
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

// 在存活 CT bot 里选首席：真实地面行走距离 − 持钳加成（选举与接力共用；无视真人玩家，见文件头 §5）
// 无损实测（备份/还原移动状态），跨楼层不可达跳过不参评，全员不可达则无首席全员放行；测距未就绪直接无首席回原生
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
        if (!IsBot(i, true) || GetClientTeam(i) != 3)
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

// 选首席；无候选/无包 → 首席 -1（钩子直通回原生）
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

public MRESReturn Hook_IsDoingScenario_Pre(int client, DHookParam hParams)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return MRES_Ignored;
    }

    if (!IsBot(client, true) || GetClientTeam(client) != 3)
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

/**
 * 查询场景门控 Detour 是否处于就绪/已挂载状态
 */
bool IsDefuseHooked()
{
    return (g_hScenarioDetour != null);
}

/**
 * 获取当前首席拆包员（-1 = 无）
 */
int GetChiefDefuser()
{
    if (g_iChief != -1 && !IsBot(g_iChief, true))
    {
        return -1;
    }
    return g_iChief;
}

/**
 * 获取累计首席选举次数
 */
int GetTotalChiefs()
{
    return g_iTotalChiefs;
}

/**
 * 获取累计拦截非首席拆包次数
 */
int GetTotalScenarioGated()
{
    return g_iTotalScenarioGated;
}

/**
 * 初始化模块 10（挂 IsDoingScenario Pre）
 * 签名缺失时优雅待机（不阻断全库启动），与模块 8 同策略
 */
bool InitModule10_Defuse()
{
    g_hScenarioDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
    if (g_hScenarioDetour == null)
    {
        LogError("[BotRouteFix] [Module 10] Failed to create DHook detour instance for IsDoingScenario!");
        return false;
    }

    if (!DHookSetFromConf(g_hScenarioDetour, g_hGameConf, SDKConf_Signature, "CCSBot_IsDoingScenario"))
    {
        LogMessage("[BotRouteFix] [Module 10] Signature 'CCSBot_IsDoingScenario' pending in gamedata (prototype mode). Module 10 is on standby.");
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
        return true;
    }

    // IsDoingScenario(void)：无参数，this 即 bot 本体
    if (!DHookEnableDetour(g_hScenarioDetour, false, Hook_IsDoingScenario_Pre))
    {
        LogError("[BotRouteFix] [Module 10] Failed to enable IsDoingScenario Pre detour!");
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
        return false;
    }

    LogMessage("[BotRouteFix] [Module 10] CT chief defuser online (IsDoingScenario Pre hook + event election)");
    return true;
}

/**
 * 还原模块 10（摘钩子 + 清首席）
 */
void RestoreModule10_Defuse()
{
    if (g_hScenarioDetour != null)
    {
        DHookDisableDetour(g_hScenarioDetour, false, Hook_IsDoingScenario_Pre);
        delete g_hScenarioDetour;
        g_hScenarioDetour = null;
    }
    g_iChief = -1;
}

/**
 * 动态启用或停用模块 10
 */
void EnableModule10_Defuse(bool enable)
{
    if (g_hScenarioDetour == null)
    {
        return;
    }

    if (enable)
    {
        DHookEnableDetour(g_hScenarioDetour, false, Hook_IsDoingScenario_Pre);
    }
    else
    {
        DHookDisableDetour(g_hScenarioDetour, false, Hook_IsDoingScenario_Pre);
    }
}

/**
 * 下包事件：选举首席
 */
void Module10_OnBombPlanted()
{
    Defuse_ElectChief();
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

void Module10_OnBombExploded()
{
    g_iChief = -1;
}

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
