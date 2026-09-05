#if defined _botroutefix_module7_escort_included
    #endinput
#endif
#define _botroutefix_module7_escort_included

//========================================================================================
// MODULE 7: T C4 CARRIER ESCORT & BODYGUARD ROSTER
//
// 花名册制贴身保镖：按士气配 3-Morale 名保镖 Follow 包匪，绝境抱团、顺风放飞。
//   - 距离一律欧氏 2D 平方（只比 X/Y，不看 Z）
//   - 全量重算只做 diff；减员按调用时实时士气重算编制，多退少补
//   - 纯事件驱动，无 timer；运包时钟归 module2，本模块永不写时钟
//========================================================================================

// 编制公式（单点定义）：保镖数 = ESCORT_BASE_COUNT - Morale，钳制 [0, ESCORT_MAX_COUNT]
//   Morale -3 → 6 保镖抱团推点；Morale +3 → 0 保镖放飞控图；默认 +1 → 2 保镖
#define ESCORT_BASE_COUNT 3
#define ESCORT_MAX_COUNT 6

// 本模块私有状态：花名册（不出本文件，对外只暴露 Escort_* 与 Module7_*）
int g_iEscortCarrier = 0;
int g_iBodyguardList[MAXPLAYERS + 1];
int g_iBodyguardCount = 0;
int g_iEscortTarget = 0;

enum struct EscortCandidate
{
    int client;
    float dist;
}

// 士气→编制
static int Escort_DesiredCount(int morale)
{
    int n = ESCORT_BASE_COUNT - morale;
    if (n < 0) n = 0;
    if (n > ESCORT_MAX_COUNT) n = ESCORT_MAX_COUNT;
    return n;
}

// 名单查找：返回下标，不在名单返回 -1
static int Escort_Find(int bot)
{
    if (!IsValidClient(bot, false))
    {
        return -1;
    }

    for (int i = 0; i < g_iBodyguardCount; i++)
    {
        if (g_iBodyguardList[i] == bot)
            return i;
    }
    return -1;
}

// 花名册防腐：正常流程 count 远小于 MAXPLAYERS；一旦离谱直接清零并报错，绝不让循环失控
static void Escort_ClampRoster()
{
    if (g_iBodyguardCount < 0 || g_iBodyguardCount > MAXPLAYERS)
    {
        LogError("[BotRouteFix] [Module 7] insane roster count (%d), resetting to 0", g_iBodyguardCount);
        g_iBodyguardCount = 0;
    }
}

// 按指定下标在名单内删除（swap-remove，不发 SDKCall，调用方决定要不要 Stop）
static void Escort_RemoveAt(int idx)
{
    if (idx < 0 || idx >= g_iBodyguardCount)
    {
        return;
    }
    g_iBodyguardList[idx] = g_iBodyguardList[g_iBodyguardCount - 1];
    g_iBodyguardCount--;
}

// 名单内删除（根据 client 查找后 swap-remove，不发 SDKCall）
static void Escort_RemoveFromRoster(int bot)
{
    int idx = Escort_Find(bot);
    if (idx != -1)
    {
        Escort_RemoveAt(idx);
    }
}

// 欧氏 2D 平方距离（只比 X/Y；排序用途，与 GetVectorDistance(..., true) 的相对顺序一致）
static float Escort_FlatDistSq(float a[3], float b[3])
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

// 在名单之外按 anchor（排序基准，一般取包匪坐标）挑最近的 need 个写入 result[]，
// carrier 本人恒排除；result 容量上限 maxOut；返回实际挑到数
static int Escort_PickClosest(float anchor[3], int carrier, int need, int result[MAXPLAYERS + 1], int maxOut)
{
    EscortCandidate pool[MAXPLAYERS + 1];
    int poolCount = 0;

    for (int i = 1; i <= MaxClients; i++)
    {
        if (i == carrier || !IsTeamBot(i, 2, true) || Escort_Find(i) != -1)
        {
            continue;
        }

        float pos[3];
        GetClientAbsOrigin(i, pos);

        pool[poolCount].client = i;
        pool[poolCount].dist = Escort_FlatDistSq(pos, anchor);
        poolCount++;
    }

    for (int i = 0; i < poolCount - 1; i++)
    {
        for (int j = 0; j < poolCount - i - 1; j++)
        {
            if (pool[j].dist > pool[j + 1].dist)
            {
                EscortCandidate temp;
                temp.client = pool[j].client;
                temp.dist = pool[j].dist;
                pool[j].client = pool[j + 1].client;
                pool[j].dist = pool[j + 1].dist;
                pool[j + 1].client = temp.client;
                pool[j + 1].dist = temp.dist;
            }
        }
    }

    int n = (need < poolCount) ? need : poolCount;
    if (n > maxOut) n = maxOut;
    for (int k = 0; k < n; k++)
        result[k] = pool[k].client;
    return n;
}

// 人多了：从远到近停掉多余的，只留最近的 desired 个
static void Escort_TrimToTarget(float anchor[3], int desired)
{
    while (g_iBodyguardCount > desired)
    {
        int farIdx = -1;
        float farD = -1.0;
        for (int i = 0; i < g_iBodyguardCount; i++)
        {
            int bot = g_iBodyguardList[i];
            if (!IsTeamBot(bot, 2, true))
            {
                // 无效、非 T 或阵亡成员优先剔除
                farIdx = i;
                break;
            }

            float pos[3];
            GetClientAbsOrigin(bot, pos);
            float d = Escort_FlatDistSq(pos, anchor);
            if (d > farD)
            {
                farD = d;
                farIdx = i;
            }
        }

        if (farIdx == -1)
        {
            break;
        }

        int targetBot = g_iBodyguardList[farIdx];
        BotStopFollowing(targetBot);
        Escort_RemoveAt(farIdx);
    }
}

// 把名单补到 desired（只补缺口，不碰现有人）
static void Escort_FillToTarget(int carrier, float anchor[3], int desired)
{
    if (g_iBodyguardCount >= desired)
        return;

    int need = desired - g_iBodyguardCount;
    int picked[MAXPLAYERS + 1];
    int n = Escort_PickClosest(anchor, carrier, need, picked, MAXPLAYERS + 1);
    for (int k = 0; k < n; k++)
    {
        BotFollow(picked[k], carrier);
        g_iBodyguardList[g_iBodyguardCount++] = picked[k];
    }
}

// 一次配平：超编从远到近停，缺编从近到远补（Refresh 与 OnRosterLost 共用）
static void Escort_Rebalance(int carrier, float anchor[3], int desired)
{
    Escort_TrimToTarget(anchor, desired);
    Escort_FillToTarget(carrier, anchor, desired);
}

//========================================================================================
// PUBLIC ROSTER API (events.inc 与主文件调用)
//========================================================================================

// 全量重算：开局 / 换包匪 / 中途捡包 / 热重载 / 重启用。只做 diff，不扫全队；永不写运包时钟
void Escort_Refresh()
{
    Escort_ClampRoster();
    int carrier = GetC4Carrier();
    if (!IsValidClient(carrier, true))
    {
        Escort_ReleaseAll();
        return;
    }

    // 换了包匪：先解散旧名单（只停名单内的人；空名单时 ReleaseAll 本就是 no-op）
    if (g_iEscortCarrier != carrier)
        Escort_ReleaseAll();
    g_iEscortCarrier = carrier;

    int morale = GetEffectiveCarrierMorale(carrier);
    int desired = Escort_DesiredCount(morale);
    g_iEscortTarget = desired;

    float anchor[3];
    GetClientAbsOrigin(carrier, anchor);

    // 名单 pruning：死/飞/转队/变成包匪本人的一律除名（死人不再发 Stop）
    for (int i = g_iBodyguardCount - 1; i >= 0; i--)
    {
        int bot = g_iBodyguardList[i];
        if (bot == carrier || !IsTeamBot(bot, 2, true))
        {
            Escort_RemoveFromRoster(bot);
        }
    }

    // 按 desired 一次配平
    Escort_Rebalance(carrier, anchor, desired);

    char moraleStr[16];
    GetMoraleName(morale, moraleStr, sizeof(moraleStr));
    DebugLog("[BotRouteFix] [Module 7] Carrier %N (Morale: %d %s) roster %d/%d",
        carrier, morale, moraleStr, g_iBodyguardCount, desired);
}

// 释放全部保镖（bomb_dropped / 包匪丢失）：快照隔离 + 只停存活名单内的人 + 清空花名册
void Escort_ReleaseAll()
{
    int count = g_iBodyguardCount;
    if (count <= 0)
    {
        g_iBodyguardCount = 0;
        g_iEscortCarrier = 0;
        g_iEscortTarget = 0;
        return;
    }

    int tempList[MAXPLAYERS + 1];
    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        tempList[i] = g_iBodyguardList[i];
    }

    // 立即清零全局计数与包匪记录，阻断重入
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;

    for (int i = 0; i < count && i <= MAXPLAYERS; i++)
    {
        int bot = tempList[i];
        BotStopFollowing(bot);
    }
}

// 静默清空（bomb_planted/defused/exploded/round_start：引擎或换图已接管，不发 SDKCall）
void Escort_ClearSilent()
{
    g_iBodyguardCount = 0;
    g_iEscortCarrier = 0;
    g_iEscortTarget = 0;
}

// 回合开始重置：Escort_ClearSilent 的对外别名
void Escort_Reset()
{
    Escort_ClearSilent();
}

// 包匪身份查询（供 player_death 处理用）
bool Escort_IsCarrier(int client)
{
    return client > 0 && client == g_iEscortCarrier;
}

// 名单成员丢失（阵亡/掉线）：除名 + 按当前士气重算编制，多退少补
void Escort_OnRosterLost(int bot, bool logIt = true)
{
    Escort_ClampRoster();
    if (Escort_Find(bot) == -1)
        return;

    Escort_RemoveFromRoster(bot);

    int carrier = g_iEscortCarrier;
    if (!IsValidClient(carrier, true))
        return;

    // 人类包匪同样按调用时的 T 均值士气重算编制
    int morale = GetEffectiveCarrierMorale(carrier);
    g_iEscortTarget = Escort_DesiredCount(morale);

    float anchor[3];
    GetClientAbsOrigin(carrier, anchor);

    int before = g_iBodyguardCount;
    Escort_Rebalance(carrier, anchor, g_iEscortTarget);

    if (logIt && g_iBodyguardCount > before)
    {
        DebugLog("[BotRouteFix] [Module 7] Replenished bodyguard %N for carrier %N (%d/%d)",
            g_iBodyguardList[g_iBodyguardCount - 1], carrier, g_iBodyguardCount, g_iEscortTarget);
    }
}

// 掉线处理：包匪掉线释放名册（掉包事件随后接管残局）；保镖掉线除名+按当前士气多退少补
void Escort_OnClientDisconnect(int client)
{
    if (client <= 0 || client > MaxClients)
    {
        return;
    }

    if (client == g_iEscortCarrier)
    {
        Escort_ReleaseAll();
        return;
    }
    Escort_OnRosterLost(client, false);
}

//========================================================================================
// MODULE LIFECYCLE & TACTICAL EVENT HOOKS
//========================================================================================

/**
 * 初始化模块 7（校验 Follow 调用就绪）
 */
bool InitModule7_Escort()
{
    if (g_hSDKCall_Follow == null || g_hSDKCall_StopFollowing == null)
    {
        LogError("[BotRouteFix] [Module 7] Follow SDKCalls not ready, escort disabled!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 7] Carrier escort roster online (Euclidean bodyguards)");
    return true;
}

/**
 * 还原模块 7（卸载不断跟随链）
 */
void RestoreModule7_Escort()
{
    Escort_ReleaseAll();
}

/**
 * 动态启用或停用模块 7
 */
void EnableModule7_Escort(bool enable)
{
    if (!enable)
    {
        Escort_ReleaseAll();
    }
    else
    {
        Escort_Refresh();
    }
}

/**
 * 回合重置 (round_start)
 */
void Module7_OnRoundStart()
{
    Escort_Reset();
}

/**
 * 地图加载 (OnMapStart)：名单索引随实体全废，新 bot 复用索引会被误认，必须清
 */
void Module7_OnMapStart()
{
    Escort_Reset();
}

/**
 * 冻结时间结束 (round_freeze_end)：开局配一次保镖（时钟归 module2，本模块不碰）
 */
void Module7_OnRoundFreezeEnd()
{
    Escort_Refresh();
}

/**
 * 道具拾取事件 (item_pickup)：中途换包匪递延一帧重配（等槽位归属落定）
 */
void Module7_OnItemPickup(int client, const char[] item)
{
    // 冻结期内开局发包不触发（买枪期跟随会打断 BuyState）
    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        RequestFrame(Module7_FrameRefresh);
    }
}

/**
 * 中途捡包递延帧（RequestFrame 回调）
 */
public void Module7_FrameRefresh(any data)
{
    Escort_Refresh();
}

/**
 * C4 掉落事件 (bomb_dropped)：释放名单，残局交给 module2 的原生选点与 module8 的捡包
 */
void Module7_OnBombDropped()
{
    DebugLog("[BotRouteFix] [Module 7] C4 dropped! Disbanding bodyguard roster (%d guards released)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 安放事件 (bomb_planted)：释放全体保镖跟随，立即转入模块 6 守点与掩体架枪任务 (GUARD_TICKING_BOMB / HideState)
 */
void Module7_OnBombPlanted()
{
    DebugLog("[BotRouteFix] [Module 7] C4 planted! Disbanding bodyguard roster (%d guards released to site defense)", g_iBodyguardCount);
    Escort_ReleaseAll();
}

/**
 * C4 拆除/爆炸事件：释放名单，确保无残留跟随
 */
void Module7_OnBombDefused()
{
    Escort_ReleaseAll();
}

void Module7_OnBombExploded()
{
    Escort_ReleaseAll();
}

/**
 * 玩家死亡事件 (player_death)
 */
void Module7_OnPlayerDeath(int victim)
{
    // 包匪阵亡：当场彻底释放保镖名单，保镖立即恢复自由身可就近捡包、自由迎敌或防守
    if (Escort_IsCarrier(victim))
    {
        Escort_ReleaseAll();
        return;
    }

    // 保镖阵亡：除名 + 按当前士气重算编制，多退少补
    Escort_OnRosterLost(victim, true);
}

/**
 * 客户端掉线处理（主文件 OnClientDisconnect 直调，game event 到不了这里）
 */
void Module7_OnClientDisconnect(int client)
{
    Escort_OnClientDisconnect(client);
}

//========================================================================================
// TELEMETRY GETTERS
//========================================================================================

int Escort_GetBodyguardCount()
{
    return g_iBodyguardCount;
}

int Escort_GetTargetCount()
{
    return g_iEscortTarget;
}

int Escort_GetCarrier()
{
    return g_iEscortCarrier;
}
