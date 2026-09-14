#if defined _botroutefix_module11_sniper_included
    #endinput
#endif
#define _botroutefix_module11_sniper_included

//========================================================================================
// MODULE 11: SNIPER DETERRENCE & CORRIDOR DANGER (SpottedSniper Post hook)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生缺陷 (cs_bot_update.cpp:529-535 & cs_bot_attack.cpp:354-360):
//    Bot 观察到敌方持有狙击枪时调用 GetChatter()->SpottedSniper() 发出警告，但非狙击手步枪手
//    依然会盲目横穿狙击手直视走廊（如中门、中路、香蕉道）排队送命。原生交火撤退判定虽然有
//    (me->CanSeeSniper() && !me->IsSniper())，却被 m_retreatTimer 与交战冷却死死卡住。
// 2. 战术闭环设计：
//    - 触发源：挂载 BotChatterInterface::SpottedSniper Post 钩子，纯事件驱动，0 模拟视觉开销；
//    - 自身校验：若目击者自己持有狙击枪（AWP/Scout/连狙），放行对狙博弈；
//    - 战术豁免：下包后 CT 见狙禁撤（争分夺秒回防拆包），T 进包点后见狙禁撤（坚守包点与强行下包）；
//    - 贴脸不撤：距狙 < 265 units 只钉走廊不请求撤退（近距离跑路等于送背）；
//    - 敌方狙击手定位：双轨确定性解析（0 Raycast、0 全图盲搜）：
//      1) 优先读取 spotter 的 m_enemy 句柄（处于交火态时直接锁定目标）；
//      2) 路径 2：若 m_enemy 尚未锁定（如移动/非交火状态），直接从 CCSBot::UpdateReactionQueue
//         每帧推入的 m_enemyQueue 瞬时感知威胁队列逆序回溯提取当帧目击的狙击手实体；
//      3) 武器判断全面覆盖：同时检索 m_hActiveWeapon 与 Slot 0 主武器，杜绝狙击手切副手/刀具时漏判；
//    - 走廊加压：调用 CCSBot::ComputePath 生成通往狙击手的 A* 走廊，最多遍历前 20 步 NavArea 钉刺 +0.8f 危险度
//      （封顶 2.5f，严格遵循 1.0/120.0s 沉降契约），逼迫友军与自身后续 A* 绕开该走廊；
//    - 强制后撤：立即清空前进路径碎步，并联动模块 5 机制穿透冷却，强制底层立即执行 TryToRetreat() 躲入掩体。
//========================================================================================

#define SNIPER_NAIL_DANGER      0.8
#define SNIPER_NAIL_MAX         2.5
#define SNIPER_MAX_STEPS        20
#define DANGER_DECAY_RATE_SEC   120.0   // 引擎原生自然沉降速率 (1.0 每 120 秒)

// 贴脸不撤距离（units）：距狙小于此值只钉走廊不请求撤退（近距离跑路等于送背）
#define SNIPER_NO_RETREAT_DIST  265.0

// 本模块私有状态
static Handle g_hSpottedSniperDetour = null;
static int g_iTotalSniperDeterred = 0;

// 缓存偏移
static int g_iOffset_ChatterMe       = -1;
static int g_iOffset_Enemy           = -1;
static int g_iOffset_EnemyQueue      = -1;
static int g_iOffset_EnemyQueueIndex = -1;
static int g_iOffset_Path            = -1;
static int g_iPathStride             = 0;
static int g_iOffset_PathLength      = -1;
static int g_iOffset_PathIndex       = -1;
static int g_iOffset_PathLadder      = -1;
static int g_iOffset_IsStopping      = -1;
static int g_iOffset_Danger          = -1;
static int g_iOffset_DangerTimestamp = -1;

/**
 * 校验指定武器实体是否属于狙击枪类
 */
static bool Sniper_IsSniperWeapon(int weapon)
{
    if (weapon <= 0 || !IsValidEntity(weapon))
    {
        return false;
    }

    char classname[32];
    GetEntityClassname(weapon, classname, sizeof(classname));
    return (StrEqual(classname, "weapon_awp")
        || StrEqual(classname, "weapon_scout")
        || StrEqual(classname, "weapon_g3sg1")
        || StrEqual(classname, "weapon_sg550"));
}

/**
 * 判断指定客户端是否手持或装备狙击枪（涵盖切刀/手枪时的主武器槽位校验）
 */
static bool Sniper_IsSniper(int client)
{
    if (!IsValidClient(client, true))
    {
        return false;
    }

    // 1. 检查当前手持激活武器
    int activeWpn = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (Sniper_IsSniperWeapon(activeWpn))
    {
        return true;
    }

    // 2. 检查 Slot 0 (主武器槽位，覆盖切副手/切刀时的狙击手身份)
    int primaryWpn = GetPlayerWeaponSlot(client, 0);
    if (Sniper_IsSniperWeapon(primaryWpn))
    {
        return true;
    }

    return false;
}

/**
 * 从 m_me 指针解析出客户端索引
 */
static int Sniper_ResolveClient(Address pMe)
{
    if (pMe == Address_Null)
    {
        return -1;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsBot(i, true))
        {
            if (GetEntityAddress(i) == pMe)
            {
                return i;
            }
        }
    }
    return -1;
}

/**
 * 路径 2：从 CCSBot 的瞬时感知威胁队列 (m_enemyQueue) 解析出当前视野内的狙击手目标
 */
static int Sniper_ResolveQueueThreat(Address pBot, int spotterTeam)
{
    if (pBot == Address_Null || g_iOffset_EnemyQueue <= 0 || g_iOffset_EnemyQueueIndex <= 0)
    {
        return -1;
    }

    int queueIdx = LoadFromAddress(pBot + view_as<Address>(g_iOffset_EnemyQueueIndex), NumberType_Int8);
    if (queueIdx < 0 || queueIdx >= 20)
    {
        return -1;
    }

    // 从最新推入的槽位向历史回溯（最多 20 帧，遍历由 FindMostDangerousThreat 记录的瞬时威胁）
    for (int step = 1; step <= 20; step++)
    {
        int idx = (queueIdx - step + 20) % 20;
        Address pEntry = pBot + view_as<Address>(g_iOffset_EnemyQueue + idx * 8);
        int handle = LoadFromAddress(pEntry, NumberType_Int32);
        if (handle == -1 || handle == 0)
        {
            continue;
        }

        int client = handle & 0xFFF;
        if (IsValidClient(client, true))
        {
            if (GetClientTeam(client) != spotterTeam && Sniper_IsSniper(client))
            {
                return client;
            }
        }
    }

    return -1;
}

/**
 * 锁定目标敌方狙击手坐标（优先 m_enemy，若未锁定则走路径 2 m_enemyQueue 瞬时感知提取）
 */
static int Sniper_FindTargetSniper(int spotter, float sniperPos[3])
{
    int sniper = -1;
    int spotterTeam = GetClientTeam(spotter);
    Address pBot = GetEntityAddress(spotter);
    if (pBot == Address_Null)
    {
        return -1;
    }

    // 1. 优先读取 spotter 的 m_enemy 句柄
    if (g_iOffset_Enemy > 0)
    {
        int handle = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Enemy), NumberType_Int32);
        if (handle != -1 && handle != 0)
        {
            int enemy = handle & 0xFFF;
            if (IsValidClient(enemy, true))
            {
                if (GetClientTeam(enemy) != spotterTeam && Sniper_IsSniper(enemy))
                {
                    sniper = enemy;
                    DebugLog("[BotRouteFix] [Module 11] Spotter %N: resolved sniper %N via m_enemy handle (0x%X)",
                        spotter, enemy, handle);
                }
            }
        }
    }

    // 2. 路径 2：若 m_enemy 未锁定，从 m_enemyQueue (瞬时感知队列) 提取目标
    if (sniper == -1)
    {
        int queueThreat = Sniper_ResolveQueueThreat(pBot, spotterTeam);
        if (queueThreat != -1)
        {
            sniper = queueThreat;
            DebugLog("[BotRouteFix] [Module 11] Spotter %N: resolved sniper %N via m_enemyQueue (instantaneous threat)",
                spotter, sniper);
        }
    }

    if (sniper != -1)
    {
        GetClientAbsOrigin(sniper, sniperPos);
    }
    return sniper;
}

/**
 * 为单个 NavArea 钉刺危险度（严格遵守 1.0/120.0s 沉降契约，封顶 2.5f）
 */
static void Sniper_NailDanger(Address pArea, int teamIdx, float curtime)
{
    if (pArea == Address_Null || g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
    {
        return;
    }

    float rawDanger = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), NumberType_Int32));
    float dangerTimestamp = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), NumberType_Int32));

    float elapsed = curtime - dangerTimestamp;
    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }

    float effDanger = rawDanger - (elapsed / DANGER_DECAY_RATE_SEC);
    if (effDanger < 0.0)
    {
        effDanger = 0.0;
    }

    float newDanger = effDanger + SNIPER_NAIL_DANGER;
    if (newDanger > SNIPER_NAIL_MAX)
    {
        newDanger = SNIPER_NAIL_MAX;
    }

    StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4), view_as<int>(newDanger), NumberType_Int32);
    StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4), view_as<int>(curtime), NumberType_Int32);
}

/**
 * SpottedSniper Post 钩子回调
 */
public MRESReturn Hook_SpottedSniper_Post(Address pThis, DHookParam hParams)
{
    if (g_cvEnable != null && !g_cvEnable.BoolValue)
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null || g_iOffset_ChatterMe <= 0)
    {
        return MRES_Ignored;
    }

    Address pMe = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_ChatterMe));
    int spotter = Sniper_ResolveClient(pMe);
    if (!IsBot(spotter, true))
    {
        return MRES_Ignored;
    }

    // 1. 若自己也是狙击手，放行对狙博弈
    if (Sniper_IsSniper(spotter))
    {
        return MRES_Ignored;
    }

    int spotterTeam = GetClientTeam(spotter);
    if (spotterTeam != 2 && spotterTeam != 3)
    {
        return MRES_Ignored;
    }

    // 2. 下包后，CT 见狙击手不可以撤（争分夺秒回防拆包）
    if (g_bBombPlanted && spotterTeam == 3)
    {
        return MRES_Ignored;
    }

    // 3. T 进包点后，见狙击手不可以撤（坚守包点阵地与强行下包）
    if (spotterTeam == 2 && GetEntProp(spotter, Prop_Send, "m_bInBombZone") != 0)
    {
        return MRES_Ignored;
    }

    // 原生 20 秒节流在上游按人头卡着，per-bot 节流永远轮不到，直接走
    float curtime = GetGameTime();

    // 4. 锁定敌方狙击手坐标
    float sniperPos[3];
    int sniper = Sniper_FindTargetSniper(spotter, sniperPos);
    if (sniper == -1)
    {
        DebugLog("[BotRouteFix] [Module 11] Spotter %N: SpottedSniper triggered, but target sniper could not be resolved in m_enemy or m_enemyQueue", spotter);
        return MRES_Ignored;
    }

    int teamIdx = spotterTeam - 2;

    // 4b. 贴脸不撤：距狙 265 内跑路等于送背，只钉走廊（团队情报）不请求撤退
    float spotterPos[3];
    GetClientAbsOrigin(spotter, spotterPos);
    bool tooClose = (GetVectorDistance(spotterPos, sniperPos) < SNIPER_NO_RETREAT_DIST);

    Address pBot = GetEntityAddress(spotter);
    if (pBot == Address_Null)
    {
        return MRES_Ignored;
    }

    // 5. 计算到狙击手的走廊路径，并为沿途 NavArea 钉上 +0.8f 危险度
    if (BotComputePath(spotter, sniperPos, 0))
    {
        if (g_iOffset_PathLength > 0 && g_iOffset_Path > 0 && g_iPathStride > 0)
        {
            int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
            int count = (pathLength > SNIPER_MAX_STEPS) ? SNIPER_MAX_STEPS : pathLength;

            for (int i = 0; i < count; i++)
            {
                Address pConnect = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
                Address pArea = LoadAddressFromAddress(pConnect);
                if (pArea != Address_Null)
                {
                    Sniper_NailDanger(pArea, teamIdx, curtime);
                }
            }
        }
    }

    // 6. 立即打碎前进路径，迫使 Bot 停步并重新评估
    if (g_iOffset_IsStopping > 0)
    {
        StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
    }
    if (g_iOffset_PathLength > 0)
    {
        StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
    }
    if (g_iOffset_PathIndex > 0)
    {
        StoreToAddress(pBot + view_as<Address>(g_iOffset_PathIndex), 0, NumberType_Int32);
    }
    if (g_iOffset_PathLadder > 0)
    {
        StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
    }

    // 7. 联动模块 5 机制：穿透撤退冷却，迫使下一拍立即执行 TryToRetreat 进掩体（贴脸不撤）
    if (!tooClose)
    {
        Combat_RequestSniperRetreat(spotter);
    }

    g_iTotalSniperDeterred++;
    if (tooClose)
    {
        DebugLog("[BotRouteFix] [Module 11] Bot %N spotted sniper %N at close range, nailed corridor danger (no retreat)", spotter, sniper);
    }
    else
    {
        DebugLog("[BotRouteFix] [Module 11] Bot %N spotted sniper %N, nailed corridor danger and retreated", spotter, sniper);
    }
    return MRES_Ignored;
}

/**
 * 查询狙击手威慑 Detour 是否已就绪
 */
bool IsSniperDeterrenceHooked()
{
    return (g_hSpottedSniperDetour != null);
}

/**
 * 获取累计成功规避/钉刺狙击走廊次数
 */
int GetTotalSniperDeterred()
{
    return g_iTotalSniperDeterred;
}

/**
 * 初始化模块 11（挂载 SpottedSniper Post Detour）
 */
bool InitModule11_Sniper()
{
    // 读取关键结构体偏移
    g_iOffset_ChatterMe       = GameConfGetOffset(g_hGameConf, "BotChatter_Me_Offset");
    g_iOffset_Enemy           = GameConfGetOffset(g_hGameConf, "CCSBot_Enemy_Offset");
    g_iOffset_EnemyQueue      = GameConfGetOffset(g_hGameConf, "CCSBot_EnemyQueue_Offset");
    g_iOffset_EnemyQueueIndex = GameConfGetOffset(g_hGameConf, "CCSBot_EnemyQueueIndex_Offset");
    g_iOffset_Path            = GameConfGetOffset(g_hGameConf, "CCSBot_Path_Offset");
    g_iPathStride             = GameConfGetOffset(g_hGameConf, "CCSBot_PathStride");
    g_iOffset_PathLength      = GameConfGetOffset(g_hGameConf, "CCSBot_PathLength_Offset");
    g_iOffset_PathIndex       = g_iOffset_PathLength + 4;
    g_iOffset_PathLadder      = GameConfGetOffset(g_hGameConf, "CCSBot_PathLadder_Offset");
    g_iOffset_IsStopping      = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");
    g_iOffset_Danger          = GameConfGetOffset(g_hGameConf, "NavArea_Danger_Offset");
    g_iOffset_DangerTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_DangerTimestamp_Offset");

    if (g_iOffset_ChatterMe == -1 || g_iOffset_Enemy == -1 || g_iOffset_EnemyQueue == -1
        || g_iOffset_EnemyQueueIndex == -1 || g_iOffset_Path == -1 || g_iPathStride <= 0
        || g_iOffset_PathLength == -1 || g_iOffset_IsStopping == -1
        || g_iOffset_PathLadder == -1 || g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1)
    {
        LogError("[BotRouteFix] [Module 11] Failed to read one or more sniper/path offsets from gamedata!");
        return false;
    }

    g_hSpottedSniperDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hSpottedSniperDetour == null)
    {
        LogError("[BotRouteFix] [Module 11] Failed to create DHook detour instance for SpottedSniper!");
        return false;
    }

    if (!DHookSetFromConf(g_hSpottedSniperDetour, g_hGameConf, SDKConf_Signature, "BotChatterInterface_SpottedSniper"))
    {
        LogMessage("[BotRouteFix] [Module 11] Signature 'BotChatterInterface_SpottedSniper' pending in gamedata (prototype mode). Module 11 is on standby.");
        delete g_hSpottedSniperDetour;
        g_hSpottedSniperDetour = null;
        return true;
    }

    if (!DHookEnableDetour(g_hSpottedSniperDetour, true, Hook_SpottedSniper_Post))
    {
        LogError("[BotRouteFix] [Module 11] Failed to enable SpottedSniper Post detour!");
        delete g_hSpottedSniperDetour;
        g_hSpottedSniperDetour = null;
        return false;
    }

    LogMessage("[BotRouteFix] [Module 11] Sniper deterrence online (Corridor danger + forced retreat)");
    return true;
}

/**
 * 还原模块 11
 */
void RestoreModule11_Sniper()
{
    if (g_hSpottedSniperDetour != null)
    {
        DHookDisableDetour(g_hSpottedSniperDetour, true, Hook_SpottedSniper_Post);
        delete g_hSpottedSniperDetour;
        g_hSpottedSniperDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 11] Sniper deterrence hooks safely restored.");
}

/**
 * 动态启用或停用模块 11
 */
void EnableModule11_Sniper(bool enable)
{
    if (g_hSpottedSniperDetour == null)
    {
        return;
    }

    if (enable)
    {
        DHookEnableDetour(g_hSpottedSniperDetour, true, Hook_SpottedSniper_Post);
    }
    else
    {
        DHookDisableDetour(g_hSpottedSniperDetour, true, Hook_SpottedSniper_Post);
    }
}
