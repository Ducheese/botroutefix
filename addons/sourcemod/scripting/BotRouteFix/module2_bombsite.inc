#if defined _botroutefix_module2_bombsite_included
    #endinput
#endif
#define _botroutefix_module2_bombsite_included

//========================================================================================
// [模块 2] T 运包时钟与开局包点随机锁定
//
// 业务背景与引擎逆向分析：
//
// 1. IsTimeToPlantBomb 士气线性映射 (m_earliestBombPlantTimestamp):
//    - 源码位置: `bot/cs_bot_manager.cpp:138` 与 `bot/states/cs_bot_idle.cpp:163`
//    - 机制分析: 引擎原生每回合开局掷骰 `m_earliestBombPlantTimestamp = curtime + RandomFloat(10.0f, 30.0f)`。
//               这是一种生硬的随机延迟，与战局态势和 Bot 士气完全脱节。
//    - 补丁方案: 接入士气线性映射公式: `plantDelay = 30.0 + 10.0 * Morale`（区间限制在 0.0s ~ 60.0s）。
//               - 士气 -3 (大劣): 0.0s (争分夺秒即刻运包下包)
//               - 士气  0 (均势): 30.0s (常规节奏)
//               - 士气 +3 (大优): 60.0s (大幅延后下包，拉长架枪与抓人窗口)
//               在开局冻结结束时 (`round_freeze_end`)，读取持包者士气计算延时，
//               写入 `TheCSBots()->m_earliestBombPlantTimestamp` 引擎内存变量中。
//
// 2. GetClosestZone 开局包点锁定与换人动态自愈:
//    - 源码位置: `bot/states/cs_bot_idle.cpp:163-196`
//    - 机制分析: 当达到运包时钟且包匪不在包点内时，引擎硬编码调用 `TheCSBots()->GetClosestZone()`
//               获取几何距离最近的包点，导致 100% 的局数 Bot 只冲固定最近包点（如 de_dust2 永远只冲 A）。
//    - 补丁方案: 将 GetClosestZone 成员函数调用序列替换为直接赋值汇编:
//               - 64 位: `mov rax, <目标Zone指针>` + 9x NOP (共 19 字节)
//               - 32 位: `mov eax, <目标Zone指针>` + 14x NOP (共 19 字节)
//               在 `round_freeze_end` 随机选取全图可用包点之一并锁定；
//               ★ 换人自愈机制: 一旦开局包主掉包 (`bomb_dropped`)、包主死亡 (`player_death`)
//               或新队友捡包 (`item_pickup`)，立即解除内存锁定，无缝恢复原生动态 `GetClosestZone`，
//               使残局新持包者能够根据当前战场位置就近选择最合理的包点下包。
//========================================================================================

//========================================================================================
// MODULE 2 DEFINES & GLOBALS
//========================================================================================

// 运包时钟士气映射公式宏定义 (Delay = BASE + STEP * Morale)
#define PLANT_DELAY_BASE        30.0    // 基础运包时钟 (士气 0 时 30 秒)
#define PLANT_DELAY_STEP        10.0    // 每级士气增减步长 (秒)
#define PLANT_DELAY_MIN         0.0     // 运包延时下限 (大劣即刻运包)
#define PLANT_DELAY_MAX         60.0    // 运包延时上限 (大优卡点控图)

// 模块状态与补丁
static MemoryPatch g_PatchBombsiteLock;
static bool g_bModule2_Active = false;   // 业务开启态
static bool g_bBombsiteLocked = false;
static int g_iLockedZoneIndex = -1;
static int g_iOpeningCarrier = 0;
static float g_fCurrentPlantDelay = 0.0;

// 模块内部专用偏移缓存
static int g_iOffset_EarliestBombPlant = -1;
static int g_iOffset_ZoneCount = -1;
static int g_iOffset_ZoneBase = -1;
static int g_iZoneStride = -1;

//========================================================================================
// MODULE 2 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：运包时钟写入
 *
 * 条件 = earliest timestamp 偏移 + morale 偏移 + TheCSBots 单例（单例由 Init 预先解析）。
 * 任一缺失 → clockReady=false，只让运包时钟事项 FAILED，不影响包点锁。
 * 失败语义：任一依赖缺失返回 false（clockReady=false），Init 将事项标记 FAILED。
 *
 * @return              运包时钟事项的后端是否就绪
 */
static bool PrepModule2_ClockBackend()
{
    g_iOffset_EarliestBombPlant = GameConfGetOffset(g_hGameConf, "TheCSBots_EarliestBombPlant_Offset");

    bool ready = (g_iOffset_EarliestBombPlant > 0 && g_iOffset_Morale > 0);
    if (!ready)
    {
        LogError("[BotRouteFix] [Module 2] Clock backend missing (earliest=%d, morale=%d); clock item FAILED.",
            g_iOffset_EarliestBombPlant, g_iOffset_Morale);
    }
    else
    {
        LogMessage("[BotRouteFix] [Module 2] Clock offsets verified (earliest=%d, morale=%d).",
            g_iOffset_EarliestBombPlant, g_iOffset_Morale);
    }
    return ready;
}

/**
 * 事项后端：包点锁补丁
 *
 * 条件 = zone 三个偏移 + 签名 + patch 偏移 + 19 字节模板校验 + 原始基线备份。
 * 任一缺失 → lockReady=false，只让包点锁事项 FAILED，不影响运包时钟。
 * 失败语义：任一依赖缺失返回 false（lockReady=false），Init 将事项标记 FAILED。
 *
 * @return              包点锁事项的后端是否就绪
 */
static bool PrepModule2_LockBackend()
{
    g_iOffset_ZoneCount = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneCount_Offset");
    g_iOffset_ZoneBase  = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneBase_Offset");
    g_iZoneStride       = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneStride");

    if (g_iOffset_ZoneCount <= 0 || g_iOffset_ZoneBase <= 0 || g_iZoneStride <= 0)
    {
        LogError("[BotRouteFix] [Module 2] Zone offsets missing; lock item FAILED.");
        return false;
    }

    Address sigAddr = GameConfGetAddress(g_hGameConf, "IdleState_PlantBombClosestZone");
    int patchOffset = GameConfGetOffset(g_hGameConf, "PlantBombClosestZone_PatchOffset");
    if (sigAddr == Address_Null || patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 2] Lock signature/offset missing; lock item FAILED.");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);
    g_PatchBombsiteLock.Init("PlantBombClosestZone_Lock", targetAddr, 19);

    int firstByte  = LoadFromAddress(targetAddr, NumberType_Int8);
    int secondByte = LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8);

    // 幂等性检测：已处于锁定补丁状态 (64位: 48 B8, 32位: B8) 时无原始基线可还原
    bool isAlreadyPatched = (g_bIsWin64 && firstByte == 0x48 && secondByte == 0xB8) || (!g_bIsWin64 && firstByte == 0xB8);
    if (isAlreadyPatched)
    {
        g_PatchBombsiteLock.isPatched = false;
        g_bBombsiteLocked = false;
        LogError("[BotRouteFix] [Module 2] Lock patch already present @ %X; lock item FAILED.", targetAddr);
        return false;
    }

    if (g_bIsWin64)
    {
        int chkOff64[5] = {0, 1, 2, 3, 4};
        int chkVal64[5] = {0x45, 0x33, 0xC9, 0x4C, 0x8D}; // xor r9d, r9d; lea r8, [rsp+..]
        for (int i = 0; i < 5; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff64[i]), NumberType_Int8);
            if (got != chkVal64[i])
            {
                LogError("[BotRouteFix] [Module 2] 64-bit lock template mismatch at +%d (expected %02X, got %02X)!",
                    chkOff64[i], chkVal64[i], got);
                return false;
            }
        }
    }
    else
    {
        int chkOff32[10] = {0, 1, 2, 3, 4, 5, 6, 11, 12, 14};
        int chkVal32[10] = {0x6A, 0x00, 0x6A, 0x02, 0x57, 0xFF, 0x90, 0x50, 0x8B, 0xE8}; // push 0; push 2; push edi; call [eax+..]; push eax; mov ecx, esi; call GetClosestZone
        for (int i = 0; i < 10; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff32[i]), NumberType_Int8);
            if (got != chkVal32[i])
            {
                LogError("[BotRouteFix] [Module 2] 32-bit lock template mismatch at +%d (expected %02X, got %02X)!",
                    chkOff32[i], chkVal32[i], got);
                return false;
            }
        }
    }

    if (!g_PatchBombsiteLock.BackupOriginal())
    {
        LogError("[BotRouteFix] [Module 2] Lock patch baseline backup failed; lock item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 2] PlantBombClosestZone block verified @ %X [%s]",
        targetAddr, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

//========================================================================================
// MODULE 2 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 2 的两个独立事项：运包时钟写入与包点锁补丁
 *
 * 先解析两个事项共用的 TheCSBots 单例，再分别检查各自后端；
 * 任一事项失败只让对应事项 FAILED，另一件仍可 ACTIVE。
 *
 * 分别准备两个事项并立即武装；任一失败不拖累另一个。
 * 失败语义：全灭时 LogModuleAllFailed + return false；部分成功按事项各自 FAILED，不连带。
 *
 * @return              是否至少有一个事项武装成功
 */
bool InitModule2_Bombsite()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 2] Gamedata missing; both items FAILED.");
        Item_SetReady(ITEM_M2_CLOCK, false);
        Item_SetReady(ITEM_M2_LOCK, false);
        return false;
    }

    // 两个事项共用 TheCSBots 单例
    if (!ResolveTheCSBots())
    {
        Item_SetReady(ITEM_M2_CLOCK, false);
        Item_SetReady(ITEM_M2_LOCK, false);
        return false;
    }

    bool clockReady = PrepModule2_ClockBackend();
    bool lockReady  = PrepModule2_LockBackend();

    Item_SetReady(ITEM_M2_CLOCK, clockReady);
    Item_SetReady(ITEM_M2_LOCK, lockReady);

    EnableModule2_Bombsite(true);
    if (!g_bModule2_Active)
    {
        LogModuleAllFailed(2);
        return false;
    }

    char armedDetail[64];
    Format(armedDetail, sizeof(armedDetail), "clock=%d, lock=%d",
        Item_IsArmed(ITEM_M2_CLOCK), Item_IsArmed(ITEM_M2_LOCK));
    LogModuleArmed(2, "Bombsite", armedDetail);
    return true;
}

/**
 * 动态启用或停用模块 2（事项 ITEM_M2_LOCK / ITEM_M2_CLOCK 的调度入口；幂等；两个事项分别武装/还原，互不连带）
 *
 * 只做调度：只武装 Ready 的事项；Active = OR(Armed)。
 * 失败语义：无返回值；按事项各自 Armed 落定（事件驱动注入在 freeze 回合事件里，不在 Enable 内）。
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule2_Bombsite(bool enable)
{
    if (enable == g_bModule2_Active)
    {
        return;
    }

    if (enable)
    {
        // 两个事项独立武装：锁定补丁按回合即时注入，运包时钟按 freeze_end 写内存。
        Item_SetArmed(ITEM_M2_LOCK, Item_IsReady(ITEM_M2_LOCK));
        Item_SetArmed(ITEM_M2_CLOCK, Item_IsReady(ITEM_M2_CLOCK));
        g_bModule2_Active = (Item_IsArmed(ITEM_M2_LOCK) || Item_IsArmed(ITEM_M2_CLOCK));

        if (g_bModule2_Active && (!Item_IsArmed(ITEM_M2_LOCK) || !Item_IsArmed(ITEM_M2_CLOCK)))
        {
            char partialDetail[64];
            Format(partialDetail, sizeof(partialDetail), "lock=%d, clock=%d",
                Item_IsArmed(ITEM_M2_LOCK), Item_IsArmed(ITEM_M2_CLOCK));
            LogModulePartial(2, partialDetail);
        }
        return;
    }

    if (g_bBombsiteLocked)
    {
        RestoreModule2_LockBackend();
    }
    Item_SetArmed(ITEM_M2_LOCK, false);
    Item_SetArmed(ITEM_M2_CLOCK, false);
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
    g_bModule2_Active = false;
}

/**
 * 还原模块 2（事项 ITEM_M2_LOCK / ITEM_M2_CLOCK；插件卸载：解除锁定并复位业务态）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule2_Bombsite()
{
    EnableModule2_Bombsite(false);
    LogMessage("[BotRouteFix] [Module 2] Bombsite patches safely restored to original bytes.");
}

//========================================================================================
// MODULE 2 PUBLIC GETTERS & FORMULA
//========================================================================================

/**
 * 查询包点是否正处于开局锁定状态
 *
 * @return              true = 当前补丁已驻留并锁定包点
 */
bool Module2_IsBombsiteLocked()
{
    return g_bBombsiteLocked;
}

/**
 * 获取当前锁定的包点索引
 *
 * @return              包点在 TheCSBots zone 表中的下标；-1 = 未锁定
 */
int Module2_GetLockedBombsiteIndex()
{
    return g_iLockedZoneIndex;
}

/**
 * 获取当前回合计算出的运包延时
 *
 * @return              运包延时秒数；0.0 = 本回合时钟事项未武装或尚未计算
 */
float Module2_GetCurrentPlantDelay()
{
    return g_fCurrentPlantDelay;
}

// --- internal formula (only used by the clock item) ---

/**
 * 根据士气计算运包延时 (公式: 30 + 10 * Morale，区间 0.0s ~ 60.0s)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              运包延时秒数
 */
static float CalculatePlantDelay(int morale)
{
    float delay = PLANT_DELAY_BASE + PLANT_DELAY_STEP * float(morale);
    if (delay < PLANT_DELAY_MIN)
    {
        delay = PLANT_DELAY_MIN;
    }
    else if (delay > PLANT_DELAY_MAX)
    {
        delay = PLANT_DELAY_MAX;
    }
    return delay;
}

/**
 * 获取目标包点世界中心坐标（供可视化子系统渲染航标）
 *
 * 跨模块只读 getter，不参与本模块事项状态。
 *
 * @param pos           输出包点中心三维坐标
 * @return              是否成功解析出包点坐标
 */
bool Module2_GetTargetBombsitePosition(float pos[3])
{
    if (!g_bBombsiteLocked || g_iLockedZoneIndex < 0)
    {
        return false;
    }

    if (g_pTheCSBotsPtr == Address_Null || g_iOffset_ZoneCount <= 0 || g_iOffset_ZoneBase <= 0 || g_iZoneStride <= 0)
    {
        return false;
    }

    Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
    if (pTheCSBots == Address_Null)
    {
        return false;
    }

    int zoneCount = LoadFromAddress(pTheCSBots + view_as<Address>(g_iOffset_ZoneCount), NumberType_Int32);
    if (zoneCount <= 0 || g_iLockedZoneIndex >= zoneCount)
    {
        return false;
    }

    int targetZone = g_iLockedZoneIndex;
    Address pZone = pTheCSBots + view_as<Address>(g_iOffset_ZoneBase + targetZone * g_iZoneStride);
    int offsetCenter = g_bIsWin64 ? 140 : 72;

    pos[0] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter),     NumberType_Int32));
    pos[1] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 4), NumberType_Int32));
    pos[2] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 8), NumberType_Int32));
    return true;
}

/**
 * 获取距离给定 Bot 基于 NavMesh 路网行走成本最近的包点世界中心（贴近原生包匪选包点算法）
 *
 * 跨模块只读 getter，不参与本模块事项状态。
 * 复刻引擎原生包匪下包逻辑（cs_bot_idle.cpp:180 TheCSBots()->GetClosestZone(me->GetLastKnownArea(), PathCost(me))）：
 * 1. 遍历所有未阻挡包点（Zone）；
 * 2. 借助 NavMeasure 调用 CCSBot::ComputePath(center, ROUTE_SAFEST) 实测路网行走距离；
 *    该路线受 SAFEST_ROUTE 避险权重加权（综合各 NavArea 动态危险度与侵略度惩罚）；
 * 3. 选取可达且有效路径距离最短的包点作为最佳目标；
 * 4. 容灾兜底：若 client 非有效 Bot、NavMeasure 待机或路网临时不可达，优雅回退至 3D 几何直线测距。
 *
 * @param client        参考 Bot（通常为包匪；用于代入其队伍、侵略度与最后已知 NavArea 进行 SAFEST 算路）
 * @param pos           输出最近包点中心三维坐标
 * @param fallbackPos   可选几何兜底坐标（若未提供或为 0 向量，则取 client 当前原点）
 * @return              true = 成功解析；false = 容器/偏移不可用
 */
bool Module2_GetNativeClosestBombsitePosition(int client, float pos[3], const float fallbackPos[3] = NULL_VECTOR)
{
    if (g_pTheCSBotsPtr == Address_Null || g_iOffset_ZoneCount <= 0 || g_iOffset_ZoneBase <= 0 || g_iZoneStride <= 0)
    {
        return false;
    }

    Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
    if (pTheCSBots == Address_Null)
    {
        return false;
    }

    int zoneCount = LoadFromAddress(pTheCSBots + view_as<Address>(g_iOffset_ZoneCount), NumberType_Int32);
    if (zoneCount <= 0)
    {
        return false;
    }

    int offsetCenter = g_bIsWin64 ? 140 : 72;
    int bestZone = -1;
    float bestDist = -1.0;

    // 1. 首选：若 client 为有效存活 Bot 且无损测距就绪，严格走 SAFEST_ROUTE 实测（贴近原生包匪选点）
    if (IsBot(client, true) && BotNavReady())
    {
        for (int i = 0; i < zoneCount; i++)
        {
            Address pZone = pTheCSBots + view_as<Address>(g_iOffset_ZoneBase + i * g_iZoneStride);
            float center[3];
            center[0] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter),     NumberType_Int32));
            center[1] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 4), NumberType_Int32));
            center[2] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 8), NumberType_Int32));

            // 实测 SAFEST_ROUTE 路线步长（带 NavArea 危险度避险惩罚）
            float dist = BotNavWalkDistance(client, center, ROUTE_SAFEST);
            if (dist >= 0.0)
            {
                if (bestZone < 0 || dist < bestDist)
                {
                    bestZone = i;
                    bestDist = dist;
                }
            }
        }
    }

    // 2. 容灾兜底：Nav 寻路不可达/非 Bot 时代入几何欧氏测距（绝不失败空转）
    if (bestZone < 0)
    {
        float from[3];
        if (!IsNullVector(fallbackPos))
        {
            from = fallbackPos;
        }
        else if (IsValidClient(client, true))
        {
            GetClientAbsOrigin(client, from);
        }
        else
        {
            return false;
        }

        float bestDistSq = 0.0;
        for (int i = 0; i < zoneCount; i++)
        {
            Address pZone = pTheCSBots + view_as<Address>(g_iOffset_ZoneBase + i * g_iZoneStride);
            float center[3];
            center[0] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter),     NumberType_Int32));
            center[1] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 4), NumberType_Int32));
            center[2] = view_as<float>(LoadFromAddress(pZone + view_as<Address>(offsetCenter + 8), NumberType_Int32));

            float dx = center[0] - from[0];
            float dy = center[1] - from[1];
            float dz = center[2] - from[2];
            float distSq = dx * dx + dy * dy + dz * dz;
            if (bestZone < 0 || distSq < bestDistSq)
            {
                bestZone = i;
                bestDistSq = distSq;
            }
        }
    }

    if (bestZone < 0)
    {
        return false;
    }

    Address pBest = pTheCSBots + view_as<Address>(g_iOffset_ZoneBase + bestZone * g_iZoneStride);
    pos[0] = view_as<float>(LoadFromAddress(pBest + view_as<Address>(offsetCenter),     NumberType_Int32));
    pos[1] = view_as<float>(LoadFromAddress(pBest + view_as<Address>(offsetCenter + 4), NumberType_Int32));
    pos[2] = view_as<float>(LoadFromAddress(pBest + view_as<Address>(offsetCenter + 8), NumberType_Int32));
    return true;
}

/**
 * 获取当前对局最早允许下包的时间戳（引擎原生字段，供可视化读取）
 *
 * @return              m_earliestBombPlantTimestamp；不可读时返回 0.0
 */
float Module2_GetEarliestPlantTime()
{
    if (g_pTheCSBotsPtr != Address_Null && g_iOffset_EarliestBombPlant > 0)
    {
        Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
        if (pTheCSBots != Address_Null)
        {
            return view_as<float>(LoadFromAddress(pTheCSBots + view_as<Address>(g_iOffset_EarliestBombPlant), NumberType_Int32));
        }
    }
    return 0.0;
}

//========================================================================================
// MODULE 2 EVENT HANDLERS
//========================================================================================

/**
 * 回合重置 (round_start)
 */
void Module2_OnRoundStart()
{
    RestoreModule2_LockBackend();
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
}

/**
 * 地图加载 (OnMapStart)： stale 锁定不能漏进新图（补丁字节跨换图常驻内存）
 */
void Module2_OnMapStart()
{
    RestoreModule2_LockBackend();
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
}

/**
 * 冻结时间结束 (round_freeze_end)
 * 核心决策点：确定开局包匪、计算士气运包时钟、随机锁定目标包点
 */
void Module2_OnRoundFreezeEnd()
{
    if (!Item_IsArmed(ITEM_M2_CLOCK) && !Item_IsArmed(ITEM_M2_LOCK))
    {
        return;
    }

    int carrier = GetC4Carrier();
    if (!IsValidClient(carrier, true))
    {
        return;
    }

    g_iOpeningCarrier = carrier;

    // 1. 运包时钟事项：士气映射并写 m_earliestBombPlantTimestamp
    if (Item_IsArmed(ITEM_M2_CLOCK))
    {
        int carrierMorale = GetEffectiveCarrierMorale(carrier);
        g_fCurrentPlantDelay = CalculatePlantDelay(carrierMorale);

        Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
        if (pTheCSBots != Address_Null)
        {
            float targetTimestamp = GetGameTime() + g_fCurrentPlantDelay;
            StoreToAddress(pTheCSBots + view_as<Address>(g_iOffset_EarliestBombPlant),
                view_as<int>(targetTimestamp), NumberType_Int32);

            char moraleStr[16];
            GetMoraleName(carrierMorale, moraleStr, sizeof(moraleStr));

            DebugLog("[BotRouteFix] [Module 2] C4 Carrier %N (Morale: %s%d %s) -> Plant Delay: %.1fs (Earliest: %.1fs)",
                carrier, (carrierMorale >= 0) ? "+" : "", carrierMorale, moraleStr, g_fCurrentPlantDelay, targetTimestamp);
        }
    }

    // 2. 包点锁事项：随机选点并打补丁
    if (Item_IsArmed(ITEM_M2_LOCK))
    {
        SelectRandomBombsite();
    }
}

/**
 * C4 掉落事件 (bomb_dropped)
 * 包匪掉包 -> 立即解除开局锁定，恢复原生动态 GetClosestZone
 */
void Module2_OnBombDropped()
{
    if (!Item_IsArmed(ITEM_M2_LOCK))
    {
        return;
    }

    if (g_bBombsiteLocked)
    {
        RestoreModule2_LockBackend();
        DebugLog("[BotRouteFix] [Module 2] C4 dropped! Restored native GetClosestZone for tactical adaptation.");
    }
    g_iOpeningCarrier = 0;
}

/**
 * 道具拾取事件 (item_pickup)：新包主出现时解除开局锁定
 *
 * @param client        拾取者客户端索引
 * @param item          被拾取的道具名
 */
void Module2_OnItemPickup(int client, const char[] item)
{
    if (!Item_IsArmed(ITEM_M2_LOCK))
    {
        return;
    }

    // 忽略冻结买枪阶段的系统发包
    if (GameRules_GetProp("m_bFreezePeriod") != 0)
    {
        return;
    }

    if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
    {
        // 若包主换人，则解除开局锁定
        if (client != g_iOpeningCarrier)
        {
            if (g_bBombsiteLocked)
            {
                RestoreModule2_LockBackend();
                DebugLog("[BotRouteFix] [Module 2] C4 picked up by new carrier %N (previous: %d)! Restored native GetClosestZone.",
                    client, g_iOpeningCarrier);
            }
            g_iOpeningCarrier = client;
        }
    }
}

/**
 * 玩家死亡事件 (player_death)：开局包匪阵亡时解除锁定
 *
 * @param victim        阵亡客户端索引
 */
void Module2_OnPlayerDeath(int victim)
{
    if (!Item_IsArmed(ITEM_M2_LOCK))
    {
        return;
    }

    if (victim > 0 && victim == g_iOpeningCarrier)
    {
        if (g_bBombsiteLocked)
        {
            RestoreModule2_LockBackend();
            DebugLog("[BotRouteFix] [Module 2] Opening C4 carrier %N died! Restored native GetClosestZone.", victim);
        }
        g_iOpeningCarrier = 0;
    }
}

/**
 * 客户端掉线事件 (OnClientDisconnect)：开局持包者掉线时解除锁定
 *
 * @param client        掉线客户端索引
 */
void Module2_OnClientDisconnect(int client)
{
    if (!Item_IsArmed(ITEM_M2_LOCK))
    {
        return;
    }

    if (client > 0 && client == g_iOpeningCarrier)
    {
        if (g_bBombsiteLocked)
        {
            RestoreModule2_LockBackend();
            DebugLog("[BotRouteFix] [Module 2] Opening C4 carrier %d disconnected! Restored native GetClosestZone.", client);
        }
        g_iOpeningCarrier = 0;
    }
}

/**
 * C4 已安放 (bomb_planted)
 * 下包完成，清理包点锁定状态
 */
void Module2_OnBombPlanted()
{
    if (g_bBombsiteLocked)
    {
        RestoreModule2_LockBackend();
    }
    g_iOpeningCarrier = 0;
}

//========================================================================================
// MODULE 2 BACKEND RESOLUTION PRIMITIVES
//========================================================================================

/**
 * 解析 TheCSBots 单例指针（两个事项共用后端）
 *
 * @return              是否成功解析到非空单例
 */
static bool ResolveTheCSBots()
{
    if (g_pTheCSBotsPtr != Address_Null)
    {
        return true;
    }

    Address sigRush = GameConfGetAddress(g_hGameConf, "IdleState_DefenseRush");
    if (sigRush == Address_Null)
    {
        LogError("[BotRouteFix] [Module 2] Failed to locate IdleState_DefenseRush to resolve TheCSBots!");
        return false;
    }

    if (g_bIsWin64)
    {
        int relOffset = LoadFromAddress(sigRush + view_as<Address>(3), NumberType_Int32);
        g_pTheCSBotsPtr = sigRush + view_as<Address>(7 + relOffset);
    }
    else
    {
        g_pTheCSBotsPtr = view_as<Address>(LoadFromAddress(sigRush + view_as<Address>(1), NumberType_Int32));
    }

    LogMessage("[BotRouteFix] [Module 2] Resolved TheCSBots singleton pointer @ %X", g_pTheCSBotsPtr);
    return true;
}

/**
 * 随机选择一个可用包点并执行锁定
 */
static void SelectRandomBombsite()
{
    if (g_pTheCSBotsPtr == Address_Null || g_iOffset_ZoneCount <= 0)
    {
        return;
    }

    Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
    if (pTheCSBots == Address_Null)
    {
        return;
    }

    int zoneCount = LoadFromAddress(pTheCSBots + view_as<Address>(g_iOffset_ZoneCount), NumberType_Int32);
    if (zoneCount <= 0)
    {
        return;
    }

    int randomIdx = GetRandomInt(0, zoneCount - 1);
    ApplyModule2_LockBackend(randomIdx);
}

//========================================================================================
// MODULE 2 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 事项 M2 包点锁的后端：注入补丁，将目标包点指针硬编码写入调用现场
 *
 * 事件驱动写入口（round_freeze_end 选点后调用），只写不探测；
 * 地址/单例未就绪时直接放行原逻辑，不标记补丁驻留。
 * 失败语义：条件不齐直接放行原逻辑（no-op），不写补丁、不标记驻留，Ready 结论不变。
 *
 * @param zoneIndex     目标包点索引 (0 ~ zoneCount-1)
 */
static void ApplyModule2_LockBackend(int zoneIndex)
{
    if (g_bBombsiteLocked)
    {
        return;
    }

    if (g_PatchBombsiteLock.address == Address_Null || g_pTheCSBotsPtr == Address_Null)
    {
        return;
    }

    Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
    if (pTheCSBots == Address_Null)
    {
        return;
    }

    int offset = g_iOffset_ZoneBase + zoneIndex * g_iZoneStride;
    Address targetZone = pTheCSBots + view_as<Address>(offset);
    Address patchAddr = g_PatchBombsiteLock.address;

    if (g_bIsWin64)
    {
        // 64-bit: mov rax, [imm64] (48 B8 [8-byte imm64]) + 9x NOP (total 19 bytes)
        // 采用零循环平铺写入（Zero-Loop Direct Write），物理根除循环超时风险
        // 首字节通过 updateMemAccess=true 获得整页读写权限，后续同一页内写入无需重复系统调用
        StoreToAddress(patchAddr, 0x48, NumberType_Int8, true);
        StoreToAddress(patchAddr + view_as<Address>(1), 0xB8, NumberType_Int8, false);
        StoreAddressToAddress(patchAddr + view_as<Address>(2), targetZone, false);

        // 填充 9 字节 NOP (0x90): 4 字节 + 4 字节 + 1 字节
        StoreToAddress(patchAddr + view_as<Address>(10), 0x90909090, NumberType_Int32, false);
        StoreToAddress(patchAddr + view_as<Address>(14), 0x90909090, NumberType_Int32, false);
        StoreToAddress(patchAddr + view_as<Address>(18), 0x90, NumberType_Int8, false);
    }
    else
    {
        // 32-bit: mov eax, [imm32] (B8 [4-byte imm32]) + 14x NOP (total 19 bytes)
        StoreToAddress(patchAddr, 0xB8, NumberType_Int8, true);
        StoreAddressToAddress(patchAddr + view_as<Address>(1), targetZone, false);

        // 填充 14 字节 NOP (0x90): 4 字节 + 4 字节 + 4 字节 + 2 字节 = 14 字节
        StoreToAddress(patchAddr + view_as<Address>(5), 0x90909090, NumberType_Int32, false);
        StoreToAddress(patchAddr + view_as<Address>(9), 0x90909090, NumberType_Int32, false);
        StoreToAddress(patchAddr + view_as<Address>(13), 0x90909090, NumberType_Int32, false);
        StoreToAddress(patchAddr + view_as<Address>(17), 0x9090, NumberType_Int16, false);
    }

    g_PatchBombsiteLock.isPatched = true;
    g_bBombsiteLocked = true;
    g_iLockedZoneIndex = zoneIndex;

    DebugLog("[BotRouteFix] [Module 2] Locked target bombsite to Zone #%d (Offset +%d) @ %X",
        zoneIndex, offset, patchAddr);
}

/**
 * 事项 M2 包点锁的后端还原：写回 GetClosestZone 原始指令现场
 *
 * 未驻留或地址无效时 no-op；还原后清锁定态与目标索引。
 * 失败语义：无（未驻留即 no-op；尽力还原）。
 */
static void RestoreModule2_LockBackend()
{
    if (g_bBombsiteLocked && g_PatchBombsiteLock.address != Address_Null)
    {
        g_PatchBombsiteLock.Restore();
        g_bBombsiteLocked = false;
        g_iLockedZoneIndex = -1;
        DebugLog("[BotRouteFix] [Module 2] Restored original GetClosestZone call @ %X",
            g_PatchBombsiteLock.address);
    }
}
