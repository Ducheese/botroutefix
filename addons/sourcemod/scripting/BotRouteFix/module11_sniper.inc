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
//      A* 走廊，最多遍历前 20 步 NavArea 钉刺 +0.8f 危险度（封顶 2.5f，遵循 1.0/120.0s 沉降契约）；
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
#define SNIPER_MAX_STEPS        20
// 沉降速率 DANGER_DECAY_RATE_SEC 统一定义于 global.inc

// 贴脸无效化距离（units）：距狙小于此值视为步枪，不钉 danger、不碎路径、不请求撤退
#define SNIPER_NO_RETREAT_DIST  265.0

// 模块启用缓存（= Item_IsArmed(ITEM_M11_CORRIDOR)，Enable 幂等短路）
static bool g_bModule11_Active = false;
// 规则意图（载体开关与关闭路径的唯一判据；事项 FAILED 时 Active=false，不能拿它判规则）
static bool g_bModule11_RuleOn = false;
static bool g_bSpottedSniperHooked = false;
static Handle g_hSpottedSniperDetour = null;
static int g_iTotalSniperRequests = 0;

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
 * 任一后端缺失返回 false，Init 会把走廊事项标记为 FAILED。
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
 * 初始化模块 11
 *
 * 先挂通知载体，再单独探测走廊事项；任一失败不连坐另一方。
 *
 * @return              true = 载体已挂载（事项状态以 items.inc 为准）
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

    char armedDetail[64];
    Format(armedDetail, sizeof(armedDetail), "corridor=%d, transport=%d",
        Item_IsArmed(ITEM_M11_CORRIDOR), g_bSpottedSniperHooked);
    LogModuleArmed(11, "Corridor", armedDetail);
    return g_bModule11_Active;
}

/**
 * 动态启用或停用模块 11（幂等）
 *
 * 载体随模块规则开关；走廊事项按后端就绪独立武装。
 *
 * @param enable        true = 挂载体并按 Ready 武装；false = 摘钩并解除武装
 */
void EnableModule11_Sniper(bool enable)
{
    if (enable)
    {
        if (g_bModule11_Active)
        {
            return;
        }

        g_bModule11_RuleOn = true;

        // 载体是模块级资源：规则开启且签名可用即挂载，不依赖走廊事项 Ready
        ApplyModule11_Transport();
        ApplyModule11_CorridorBackend();

        g_bModule11_Active = Item_IsArmed(ITEM_M11_CORRIDOR);
        return;
    }

    if (!g_bModule11_RuleOn)
    {
        return;
    }

    g_bModule11_RuleOn = false;
    RestoreModule11_CorridorBackend();
    g_bModule11_Active = false;
}

/**
 * 还原模块 11（插件卸载：摘钩并释放句柄）
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

    LogMessage("[BotRouteFix] [Module 11] Sniper deterrence hooks safely restored.");
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

//========================================================================================
// MODULE 11 DETOUR & HOOKS (BOTCHATTERINTERFACE::SPOTTEDSNIPER, POST)
//========================================================================================

/**
 * SpottedSniper Post：走廊加压 + 撤退请求
 *
 * 载体只在模块启用时挂载；走廊钉刺按事项武装门控，
 * 撤退请求是跨模块通知（归 M5），载体在就照发。
 *
 * @param pThis         BotChatterInterface 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_SpottedSniper_Post(Address pThis, DHookParam hParams)
{
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
 */
static void RestoreModule11_CorridorBackend()
{
    Item_SetArmed(ITEM_M11_CORRIDOR, false);
    SyncDetourEnabled(g_hSpottedSniperDetour, g_bSpottedSniperHooked,
        true, Hook_SpottedSniper_Post, false, "Module 11");
}

//========================================================================================
// MODULE 11 INTERNAL PRIMITIVES
//========================================================================================

/**
 * 狙击手判定统一走 sdkcalls.inc 的 `IsSniper` 总入口（引擎真值优先 + 名册回退，
 * 人机通用）。本文件不再自带判定实现，三个调用点直接调总入口。
 */

/**
 * 事项 M11 corridor danger 的实体指针反解：从 m_me 解析出客户端索引
 *
 * 空指针或找不到对应 Bot 时返回 -1。
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
 * 严格遵守 1.0/120.0s 沉降契约并封顶 2.5f；空指针/偏移不可用时返回 false。
 *
 * @return              true = 本次真实写入危险度与时间戳
 */
static bool Sniper_NailDanger(Address pArea, int teamIdx, float curtime)
{
    if (pArea == Address_Null || g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
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
