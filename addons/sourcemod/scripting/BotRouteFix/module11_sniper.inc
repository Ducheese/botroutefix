#if defined _botroutefix_module11_sniper_included
    #endinput
#endif
#define _botroutefix_module11_sniper_included

//========================================================================================
// MODULE 11: SNIPER DETERRENCE & CORRIDOR DANGER (SpottedSniper Post hook)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生缺陷 (cs_bot_update.cpp:606-611 & cs_bot_attack.cpp:334-360):
//    Bot 观察到敌方持有狙击枪时调用 GetChatter()->SpottedSniper() 发出警告，但非狙击手步枪手
//    依然会盲目横穿狙击手直视走廊（如中门、中路、香蕉道）排队送命。原生交火撤退判定虽然有
//    (me->CanSeeSniper() && !me->IsSniper())，却被 m_retreatTimer 与交战冷却死死卡住。
// 2. 战术闭环设计：
//    - 触发源：挂载 BotChatterInterface::SpottedSniper Post 钩子，纯事件驱动，0 模拟视觉开销；
//    - 自身校验：若目击者自己持有狙击枪（AWP/Scout/连狙），放行对狙博弈；
//    - 职责边界 (2026-10-31)：本模块只负责"走廊 danger + 撤退请求"，撤退档位（硬豁免/短蹲/放行）
//      由模块 5 统一裁决，本模块不判任务与包点豁免；
//    - 贴脸无效化：距狙 < 265 units 时狙击枪退化为步枪，不构成走廊威胁——不钉 danger、不探测走廊
//      （因此不改写当前路径）、不发撤退请求，直接放行（两点重叠没有走廊可钉，钉了只会污染遭遇区）；
//    - 敌方狙击手定位：双轨确定性解析（0 Raycast、0 全图盲搜）：
//      1) 优先读取 spotter 的 m_enemy 句柄（处于交火态时直接锁定目标）；
//      2) 路径 2：若 m_enemy 尚未锁定（如移动/非交火状态），直接从 CCSBot::UpdateReactionQueue
//         每次感知更新推入的 m_enemyQueue 瞬时感知威胁队列逆序回溯提取当帧目击的狙击手实体；
//      3) 武器判断全面覆盖：同时检索 m_hActiveWeapon 与 Slot 0 主武器，杜绝狙击手切副手/刀具时漏判；
//    - 走廊加压：经 NavMeasure 的临时探测（内部调用 CCSBot::ComputePath，快照-还原）生成通往狙击手的
//      A* 走廊，最多遍历前 64 步 NavArea 钉刺 +0.8f 危险度（封顶 2.5f，遵循 1.0/120.0s 沉降契约；
//      步数上限与可视化名单容量 MAX_PRESSURIZED_AREAS 对齐，单次事件输出恒能完整落盘）；
//      爬梯中跳过探测（梯子子状态不在快照范围）；
//    - 撤退请求：仅发 Module5_RequestSniperRetreat（带 2 秒 TTL），不碎路径——
//      是否真撤、撤多久由 M5 按身份/阶段裁决，避免硬豁免目标被本模块打断使命动作。
//
// 事项契约:
//   - 唯一事项 = corridor danger（走廊钉刺）。撤退由 M5 执行 → 撤退归 M5；
//     本模块只发跨模块通知，不计事项（items.inc:21 归属原则）。
//   - SpottedSniper Detour 是通知载体（模块级资源），不是事项后端：
//     规则开启且签名可用即挂载，保证 nav 缺失、走廊事项 FAILED 时请求仍能转发
//     （README M11 特例 / items.inc 归属原则）。
//   - 走廊钉刺只在 ITEM_M11_CORRIDOR 武装时写入；只有载体挂载时本钩子才可能被调用，
//     因此 g_bModule11_Active 同时充当"通知是否放行"的规则开关。
//========================================================================================

//========================================================================================
// MODULE 11 DEFINES & GLOBALS
//========================================================================================

#define SNIPER_NAIL_DANGER      0.8
#define SNIPER_NAIL_MAX         2.5
#define SNIPER_MAX_STEPS        64
// 沉降速率 DANGER_DECAY_RATE_SEC 统一定义于 global.inc

// 贴脸无效化距离（units）：距狙小于此值视为步枪，不钉 danger、不碎路径、不请求撤退
#define SNIPER_NO_RETREAT_DIST  265.0

// 模块启用缓存（= Item_IsArmed(ITEM_M11_CORRIDOR)，Enable 幂等短路）
static bool g_bModule11_Active = false;
static bool g_bSpottedSniperHooked = false;
static Handle g_hSpottedSniperDetour = null;
static int g_iTotalSniperRequests = 0;

// 目击连线追踪：发现者 -> 狙击手（SpottedSniper 事件驱动，高亮 10 秒，与 M9/M10 同画法）
#define SNIPER_PAIR_HIGHLIGHT_SEC   10.0
#define MAX_SNIPER_PAIRS            8
static int g_SniperPairSpotter[MAX_SNIPER_PAIRS];
static int g_SniperPairSniper[MAX_SNIPER_PAIRS];
static float g_flSniperPairExpire[MAX_SNIPER_PAIRS];
static int g_iSniperPairCount = 0;

// 走廊高亮追踪：被加压过的走廊 NavArea（钉刺后高亮 60 秒，只画圈）
// 【编译器缺陷补偿】spcomp64 把全局 Address 数组按 4B/元分配、读写却按 8B 步进
//（实证 F:\Git\scratch\stride_test\），故声明双倍长度，实际只用前 N 槽。
#define SNIPER_CORRIDOR_HIGHLIGHT_SEC   60.0
#define MAX_PRESSURIZED_AREAS           64
static Address g_PressurizedAreas[MAX_PRESSURIZED_AREAS * 2];
static float g_flPressurizedExpire[MAX_PRESSURIZED_AREAS];
static int g_iPressurizedAreaCount = 0;

// 缓存偏移（Danger/DangerTimestamp 上收 nav_measure.inc 共享，本文件只用不存）
static int g_iOffset_ChatterMe       = -1;
static int g_iOffset_Enemy           = -1;
static int g_iOffset_EnemyQueue      = -1;
static int g_iOffset_EnemyQueueIndex = -1;

//========================================================================================
// MODULE 11 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：走廊 danger（ITEM_M11_CORRIDOR）
 *
 * 只定位/校验走廊钉刺所需偏移与测距子系统，不挂载钩子、不写内存。
 * 任一后端缺失都返回 false，Init 会把走廊事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 * nav 缺失只让走廊事项 FAILED；Detour 仍在时撤退请求继续转发（不属于本事项）。
 *
 * @return              true = 走廊事项后端就绪
 */
static bool PrepModule11_CorridorBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 11] Gamedata missing; corridor item FAILED (no retreat forwarding).");
        return false;
    }

    g_iOffset_ChatterMe       = GameConfGetOffset(g_hGameConf, "BotChatter_Me_Offset");
    g_iOffset_Enemy           = GameConfGetOffset(g_hGameConf, "CCSBot_Enemy_Offset");
    g_iOffset_EnemyQueue      = GameConfGetOffset(g_hGameConf, "CCSBot_EnemyQueue_Offset");
    g_iOffset_EnemyQueueIndex = GameConfGetOffset(g_hGameConf, "CCSBot_EnemyQueueIndex_Offset");
    // Danger 由 InitNavMeasure 统一解析，此处只做门禁

    bool offsetsOk = (g_iOffset_ChatterMe > 0 && g_iOffset_Enemy > 0
        && g_iOffset_EnemyQueue > 0 && g_iOffset_EnemyQueueIndex > 0
        && g_iOffset_Danger > 0 && g_iOffset_DangerTimestamp > 0);
    if (!offsetsOk)
    {
        LogError("[BotRouteFix] [Module 11] Sniper/danger offsets missing; corridor item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 11] Corridor offsets verified (me=%d, enemy=%d, queue=%d, queueidx=%d, danger=%d, dangerts=%d).",
        g_iOffset_ChatterMe, g_iOffset_Enemy, g_iOffset_EnemyQueue, g_iOffset_EnemyQueueIndex,
        g_iOffset_Danger, g_iOffset_DangerTimestamp);

    // 没有通知载体就没有唯一入口，走廊钉刺无从执行
    if (g_hSpottedSniperDetour == null)
    {
        LogError("[BotRouteFix] [Module 11] SpottedSniper transport missing; corridor item FAILED.");
        return false;
    }

    // 走廊钉刺依赖 NavMeasure 的临时探测（快照-计算-还原），测距待机时事项不可用
    if (!BotNavReady())
    {
        LogError("[BotRouteFix] [Module 11] NavMeasure unavailable; corridor item FAILED (retreat requests still forwarded).");
        return false;
    }

    return true;
}

/**
 * 通知载体：挂载 BotChatterInterface::SpottedSniper Post Detour
 *
 * 载体不是事项后端：它只负责把"看见狙击手"这个事件送进回调，
 * 走廊钉刺与撤退请求都在回调里按各自门禁执行。
 * 失败语义：创建/绑定失败返回 false（载体挂载失败不连带走廊事项，Init 直接丢弃其返回值）。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool PrepModule11_NotificationTransport()
{
    if (g_hSpottedSniperDetour != null)
    {
        return true;
    }

    g_hSpottedSniperDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hSpottedSniperDetour == null)
    {
        LogError("[BotRouteFix] [Module 11] Failed to create DHook detour instance for SpottedSniper!");
        return false;
    }

    if (!DHookSetFromConf(g_hSpottedSniperDetour, g_hGameConf, SDKConf_Signature, "BotChatterInterface_SpottedSniper"))
    {
        LogError("[BotRouteFix] [Module 11] SpottedSniper signature missing; corridor and retreat requests unavailable.");
        delete g_hSpottedSniperDetour;
        g_hSpottedSniperDetour = null;
        return false;
    }

    return true;
}

//========================================================================================
// MODULE 11 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 11（走廊事项 ITEM_M11_CORRIDOR；通知载体非事项，只负责事件投递）
 *
 * 先挂通知载体，再单独探测走廊事项；任一失败不连坐另一方。
 * 失败语义：gamedata 缺失直接 Ready(false)+return false；载体挂载结果丢弃、不阻断；
 * 走廊事项未武装 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 *
 * @return              true = 走廊事项已武装（载体状态不影响返回值）
 */
bool InitModule11_Sniper()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 11] Gamedata missing; corridor item FAILED.");
        Item_SetReady(ITEM_M11_CORRIDOR, false);
        return false;
    }

    PrepModule11_NotificationTransport();
    Item_SetReady(ITEM_M11_CORRIDOR, PrepModule11_CorridorBackend());

    EnableModule11_Sniper(true);
    if (!Item_IsArmed(ITEM_M11_CORRIDOR))
    {
        LogModuleAllFailed(11);
        return false;
    }

    LogModuleArmed(11, "Corridor", "1");
    return true;
}

/**
 * 动态启用或停用模块 11（幂等）
 *
 * 载体随模块规则开关；走廊事项按后端就绪独立武装。
 * 失败语义：只做调度，无返回值；载体与事项各自按 Apply 结果落定，互不连带。
 *
 * @param enable        true = 挂载体并按 Ready 武装；false = 摘钩并解除武装
 */
void EnableModule11_Sniper(bool enable)
{
    if (enable == g_bModule11_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule11_CorridorBackend();
        g_bModule11_Active = false;
        return;
    }

    // 载体是模块级资源：规则开启且签名可用即挂载，不依赖走廊事项 Ready
    ApplyModule11_Transport();
    ApplyModule11_CorridorBackend();

    g_bModule11_Active = Item_IsArmed(ITEM_M11_CORRIDOR);
}

/**
 * 还原模块 11（事项 ITEM_M11_CORRIDOR；插件卸载：摘钩并释放句柄）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule11_Sniper()
{
    EnableModule11_Sniper(false);

    if (g_hSpottedSniperDetour != null)
    {
        // 卸载路径不看模块缓存，强制确保钩子已摘除后再释放句柄
        SyncDetourEnabled(g_hSpottedSniperDetour, g_bSpottedSniperHooked,
            true, Hook_SpottedSniper_Post, false, "Module 11");
        delete g_hSpottedSniperDetour;
        g_hSpottedSniperDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 11] Corridor hooks safely restored.");
}

//========================================================================================
// MODULE 11 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 查询累计发出的狙击撤退请求数（每命中一次威慑事件计一次）
 *
 * @return              自插件加载以来的累计请求数
 */
int Module11_GetTotalSniperRequests()
{
    return g_iTotalSniperRequests;
}

/**
 * 获取当前高亮期内的目击连线二元组（发现者 -> 狙击手）。
 *
 * 事项 M11 目击连线可视化的读取出口（跨模块只读，供 visuals 快定时绘制）。
 * 失败语义：无记录/全部过期时返回 0（调用方画零线）。
 * 门禁：无（读侧解耦；写侧由钩子载体门禁，模块关闭时名单恒空）。
 *
 * @param spotters      输出发现者客户端索引数组
 * @param snipers       输出狙击手客户端索引数组
 * @return              实际有效对数
 */
int Module11_GetSniperPairs(int spotters[MAX_SNIPER_PAIRS], int snipers[MAX_SNIPER_PAIRS])
{
    float curtime = GetGameTime();
    int count = 0;
    for (int i = 0; i < g_iSniperPairCount; i++)
    {
        if (g_SniperPairSpotter[i] > 0 && g_SniperPairSniper[i] > 0 && curtime < g_flSniperPairExpire[i])
        {
            spotters[count] = g_SniperPairSpotter[i];
            snipers[count] = g_SniperPairSniper[i];
            count++;
        }
    }
    return count;
}

/**
 * 获取当前高亮期内的加压走廊 NavArea（只画圈用）。
 *
 * 事项 M11 走廊可视化的读取出口（跨模块只读，供 visuals 慢定时绘制）。
 * 失败语义：名单为空/全部过期时返回 0（调用方画零环）。
 * 门禁：无（读侧解耦；写侧由钉刺事项武装门禁，关闭时名单恒空）。
 *
 * @param areas         输出 NavArea 指针数组
 * @param expires       输出各 Area 的过期时间戳
 * @return              实际有效数量
 */
int Module11_GetPressurizedCorridors(Address areas[MAX_PRESSURIZED_AREAS], float expires[MAX_PRESSURIZED_AREAS])
{
    float curtime = GetGameTime();
    int count = 0;
    for (int i = 0; i < g_iPressurizedAreaCount; i++)
    {
        if (g_PressurizedAreas[i] != Address_Null && curtime < g_flPressurizedExpire[i])
        {
            areas[count] = g_PressurizedAreas[i];
            expires[count] = g_flPressurizedExpire[i];
            count++;
        }
    }
    return count;
}

//========================================================================================
// MODULE 11 EVENT HANDLERS
//========================================================================================

/**
 * 回合重置：清空目击连线名单（与 M8 同步，避免跨回合残影）。
 *
 * 纯幂等清理，依底层契约天然豁免守卫。
 */
void Module11_OnRoundStart()
{
    g_iSniperPairCount = 0;
    g_iPressurizedAreaCount = 0;
}

/**
 * 地图加载：换图清空名单（客户端槽位会被复用，旧索引野掉）。
 *
 * 纯幂等清理，依底层契约天然豁免守卫。
 */
void Module11_OnMapStart()
{
    Module11_OnRoundStart();
}

//========================================================================================
// MODULE 11 INTERNAL PRIMITIVES
//========================================================================================

/**
 * 狙击手判定统一走 sdkcalls.inc 的 `IsSniper` 总入口（引擎真值优先 + 名册回退，
 * 人机通用）。本文件不再自带判定实现，三个调用点直接调总入口。
 */

/**
 * 事项 M11 corridor danger 的路径 2：从瞬时感知威胁队列回溯狙击手目标
 *
 * 队列不可用、越界或 20 帧内无狙击手时返回 -1。
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
            if (GetClientTeam(client) != spotterTeam && IsSniper(client))
            {
                return client;
            }
        }
    }

    return -1;
}

/**
 * 事项 M11 corridor danger 的目标锁定（优先 m_enemy，否则走 m_enemyQueue）
 *
 * 两条路径都失败时返回 -1，本拍不钉 danger、不发撤退请求。
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
                if (GetClientTeam(enemy) != spotterTeam && IsSniper(enemy))
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
 * 事项 M11 corridor danger 的唯一写入口：为单个 NavArea 钉刺危险度
 *
 * 严格遵守 1.0/120.0s 沉降契约并封顶 2.5f；空指针/偏移不可用/
 * 未知区域（不在 TheNavAreas 内）时返回 false（宁可漏钉）。
 *
 * @return              true = 本次真实写入危险度与时间戳
 */
static bool Sniper_NailDanger(Address pArea, int teamIdx, float curtime)
{
    // 写前门禁：哨兵快拒 + 容器 exact-match，野指针到不了 StoreToAddress。
    if (!IsValidAddress(pArea)
        || g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0
        || !Module3_IsKnownNavArea(pArea))
    {
        return false;
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
    return true;
}

/**
 * 目击连线追踪登记：记下 (发现者, 狙击手) 二元组，高亮 SNIPER_PAIR_HIGHLIGHT_SEC 秒。
 *
 * 事项 M11 目击连线可视化的追踪登记（写侧；SpottedSniper 钩子内调用，不分贴脸/走廊）。
 * 同一二元组重复登记只刷新过期时间；满 8 组挤掉最早过期的。任一槽位无效直接返回。
 * 与 M8 RecordBlockedArea 同构（定长名单 + 刷新续期 + 挤最旧）。
 */
static void RecordSniperPair(int spotter, int sniper, float expireTime)
{
    if (spotter <= 0 || sniper <= 0)
    {
        return;
    }

    for (int i = 0; i < g_iSniperPairCount; i++)
    {
        if (g_SniperPairSpotter[i] == spotter && g_SniperPairSniper[i] == sniper)
        {
            if (expireTime > g_flSniperPairExpire[i])
            {
                g_flSniperPairExpire[i] = expireTime;
            }
            return;
        }
    }

    if (g_iSniperPairCount < MAX_SNIPER_PAIRS)
    {
        g_SniperPairSpotter[g_iSniperPairCount] = spotter;
        g_SniperPairSniper[g_iSniperPairCount] = sniper;
        g_flSniperPairExpire[g_iSniperPairCount] = expireTime;
        g_iSniperPairCount++;
        return;
    }

    int oldestIdx = 0;
    float oldestTime = g_flSniperPairExpire[0];
    for (int i = 1; i < MAX_SNIPER_PAIRS; i++)
    {
        if (g_flSniperPairExpire[i] < oldestTime)
        {
            oldestTime = g_flSniperPairExpire[i];
            oldestIdx = i;
        }
    }
    g_SniperPairSpotter[oldestIdx] = spotter;
    g_SniperPairSniper[oldestIdx] = sniper;
    g_flSniperPairExpire[oldestIdx] = expireTime;
}

/**
 * 走廊加压可视化追踪登记：记下钉过 danger 的走廊 NavArea，高亮 SNIPER_CORRIDOR_HIGHLIGHT_SEC 秒。
 *
 * 事项 M11 走廊可视化的追踪登记（写侧；钉刺路径调用，调用点已按事项武装门禁）。
 * 同一 Area 重复登记只刷新过期时间；满上限挤掉最早过期的。空地址直接返回。
 */
static void RecordPressurizedCorridor(Address pArea, float expireTime)
{
    // 调用方保证：入参刚过 Sniper_NailDanger 的容器校验；此处哨兵快拒即可。
    if (!IsValidAddress(pArea))
    {
        return;
    }

    for (int i = 0; i < g_iPressurizedAreaCount; i++)
    {
        if (g_PressurizedAreas[i] == pArea)
        {
            if (expireTime > g_flPressurizedExpire[i])
            {
                g_flPressurizedExpire[i] = expireTime;
            }
            return;
        }
    }

    if (g_iPressurizedAreaCount < MAX_PRESSURIZED_AREAS)
    {
        g_PressurizedAreas[g_iPressurizedAreaCount] = pArea;
        g_flPressurizedExpire[g_iPressurizedAreaCount] = expireTime;
        g_iPressurizedAreaCount++;
        return;
    }

    int oldestIdx = 0;
    float oldestTime = g_flPressurizedExpire[0];
    for (int i = 1; i < MAX_PRESSURIZED_AREAS; i++)
    {
        if (g_flPressurizedExpire[i] < oldestTime)
        {
            oldestTime = g_flPressurizedExpire[i];
            oldestIdx = i;
        }
    }
    g_PressurizedAreas[oldestIdx] = pArea;
    g_flPressurizedExpire[oldestIdx] = expireTime;
}

//========================================================================================
// MODULE 11 DETOUR & HOOKS (BOTCHATTERINTERFACE::SPOTTEDSNIPER, POST)
//========================================================================================

/**
 * SpottedSniper Post：走廊加压 + 撤退请求
 *
 * 载体只在模块启用时挂载；走廊钉刺按事项武装门控，
 * 撤退请求是跨模块通知（归 M5），载体在就照发。
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * @param pThis         BotChatterInterface 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_SpottedSniper_Post(Address pThis)
{
    if (pThis == Address_Null || g_iOffset_ChatterMe <= 0)
    {
        return MRES_Ignored;
    }

    Address pMe = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_ChatterMe));
    int spotter = ResolveBotClient(pMe);
    if (!IsBot(spotter, true))
    {
        return MRES_Ignored;
    }

    // 1. 若自己也是狙击手，放行对狙博弈
    if (IsSniper(spotter))
    {
        return MRES_Ignored;
    }

    int spotterTeam = GetClientTeam(spotter);
    if (spotterTeam != 2 && spotterTeam != 3)
    {
        return MRES_Ignored;
    }

    // 原生 20 秒节流在上游按人头卡着，per-bot 节流永远轮不到，直接走
    float curtime = GetGameTime();

    // 2. 锁定敌方狙击手坐标
    float sniperPos[3];
    int sniper = Sniper_FindTargetSniper(spotter, sniperPos);
    if (sniper == -1)
    {
        DebugLog("[BotRouteFix] [Module 11] Spotter %N: SpottedSniper triggered, but target sniper could not be resolved in m_enemy or m_enemyQueue", spotter);
        return MRES_Ignored;
    }

    // 目击连线追踪登记（与走廊钉刺解耦：贴脸放行也画，画的是目击关系不是走廊威胁）
    RecordSniperPair(spotter, sniper, curtime + SNIPER_PAIR_HIGHLIGHT_SEC);

    int teamIdx = spotterTeam - 2;

    // 3. 贴脸无效化：distance < 265 时狙击枪退化为步枪，只构成点威胁、不构成走廊威胁。
    //    不钉 danger（两点重叠没有走廊可钉，钉了污染遭遇区）、不碎路径、不发撤退请求，
    //    威胁模型交回常规交火（M5 残血/一打多）处理。
    float spotterPos[3];
    GetClientAbsOrigin(spotter, spotterPos);
    if (GetVectorDistance(spotterPos, sniperPos) < SNIPER_NO_RETREAT_DIST)
    {
        // 贴脸属于"放行交火"，不计入请求数（否则 status 的 requests 会混入大量无效样本）
        DebugLog("[BotRouteFix] [Module 11] Bot %N spotted sniper %N at close range, treated as rifle (no danger, no retreat)", spotter, sniper);
        return MRES_Ignored;
    }

    // 4. 走廊事项：临时探测到狙击手的走廊路径，并为沿途 NavArea 钉上 +0.8f 危险度
    //    ★ 必须走 BotNavProbePathAreas（快照-计算-还原）：CCSBot::ComputePath 会先 DestroyPath()
    //      再写入新路径（cs_bot_pathfind.cpp:1877-1891），直接调用会篡改 spotter 当前行进路径，
    //      硬豁免目标（包匪下包临界、CT 拆包任务）不会被 M5 后撤，也就没人帮它重建路径。
    int corridorSteps = 0;
    int nailedAreas = 0;
    if (Item_IsArmed(ITEM_M11_CORRIDOR))
    {
        int corridorCount = BotNavProbePathAreas(spotter, sniperPos);
        corridorSteps = (corridorCount > SNIPER_MAX_STEPS) ? SNIPER_MAX_STEPS : corridorCount;
        for (int i = 0; i < corridorSteps; i++)
        {
            Address pArea = BotNavProbeAreaAt(i);
            if (Sniper_NailDanger(pArea, teamIdx, curtime))
            {
                nailedAreas++;
                RecordPressurizedCorridor(pArea, curtime + SNIPER_CORRIDOR_HIGHLIGHT_SEC);
            }
        }
    }

    // 5. 联动模块 5：只发撤退请求（带 TTL），不碎路径。撤退归 M5，本模块不判档位。
    //    是否真撤、撤多久由 M5 按身份/阶段裁决——硬豁免目标（包匪下包临界、CT 拆包临界）
    //    不能被本模块打断使命动作；真正撤退时 Hide 内部会自行 DestroyPath/ComputePath。
    Module5_RequestSniperRetreat(spotter);
    g_iTotalSniperRequests++;

    DebugLog("[BotRouteFix] [Module 11] Bot %N spotted sniper %N, nailed %d/%d corridor areas, requested retreat",
        spotter, sniper, nailedAreas, corridorSteps);
    return MRES_Ignored;
}

//========================================================================================
// MODULE 11 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用通知载体：规则开启且签名可用即挂载（模块级资源，不参与事项状态）
 *
 * 走廊事项 FAILED 时载体仍需保持挂载，撤退请求才能继续转发（M11 特例）。
 * 失败语义：载体挂不上时走廊事项必然 FAILED，请求也无法转发。
 *
 * @return              true = 载体处于启用态
 */
static bool ApplyModule11_Transport()
{
    return SyncDetourEnabled(g_hSpottedSniperDetour, g_bSpottedSniperHooked,
        true, Hook_SpottedSniper_Post, true, "Module 11");
}

/**
 * 应用走廊事项：只有 Ready 且载体已在运行才武装
 *
 * 走廊钉刺的执行点在 Detour 回调内，本入口只做武装门控（无独立注入点）。
 * 失败语义：纯门控无注入；条件不齐仅置 Armed(false)，不改写 Ready 结论。
 *
 * @return              true = 事项已武装
 */
static bool ApplyModule11_CorridorBackend()
{
    bool ok = (Item_IsReady(ITEM_M11_CORRIDOR)
        && g_hSpottedSniperDetour != null
        && g_bSpottedSniperHooked);
    Item_SetArmed(ITEM_M11_CORRIDOR, ok);
    return ok;
}

/**
 * 还原走廊事项：解除武装并摘除通知载体
 *
 * 载体归模块所有，停用即一并关闭；下次启用重新 Sync。
 * 失败语义：无（尽力还原；幂等）。
 */
static void RestoreModule11_CorridorBackend()
{
    Item_SetArmed(ITEM_M11_CORRIDOR, false);
    SyncDetourEnabled(g_hSpottedSniperDetour, g_bSpottedSniperHooked,
        true, Hook_SpottedSniper_Post, false, "Module 11");
}
