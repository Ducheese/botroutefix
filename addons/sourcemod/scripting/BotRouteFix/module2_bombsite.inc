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
//    - 补丁方案: 将 GetClosestZone 虚函数调用替换为直接赋值汇编:
//               - 64 位: `mov rax, <目标Zone指针>` + 9x NOP (共 19 字节)
//               - 32 位: `mov eax, <目标Zone指针>` + 14x NOP (共 19 字节)
//               在 `round_freeze_end` 随机选取全图可用包点之一并锁定；
//               ★ 换人自愈机制: 一旦开局包主掉包 (`bomb_dropped`)、包主死亡 (`player_death`)
//               或新队友捡包 (`item_pickup`)，立即解除内存锁定，无缝恢复原生动态 `GetClosestZone`，
//               使残局新持包者能够根据当前战场位置就近选择最合理的包点下包。
//========================================================================================

// 模块状态与补丁
MemoryPatch g_PatchBombsiteLock;
bool g_bBombsiteLocked = false;
int g_iLockedZoneIndex = -1;
int g_iOpeningCarrier = 0;
float g_fCurrentPlantDelay = 0.0;

// 模块内部专用偏移缓存
static int g_iOffset_EarliestBombPlant = -1;
static int g_iOffset_ZoneCount = -1;
static int g_iOffset_ZoneBase = -1;
static int g_iZoneStride = -1;

/**
 * 初始化模块 2 所需的偏移与内存补丁准备
 *
 * @return              是否初始化成功
 */
bool InitModule2_Bombsite()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 2] Error: Gamedata is not loaded, cannot initialize bombsite module!");
        return false;
    }

    // 1. 读取所需内部偏移
    g_iOffset_EarliestBombPlant = GameConfGetOffset(g_hGameConf, "TheCSBots_EarliestBombPlant_Offset");
    g_iOffset_ZoneCount         = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneCount_Offset");
    g_iOffset_ZoneBase          = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneBase_Offset");
    g_iZoneStride               = GameConfGetOffset(g_hGameConf, "TheCSBots_ZoneStride");

    if (g_iOffset_EarliestBombPlant == -1 || g_iOffset_ZoneCount == -1 ||
        g_iOffset_ZoneBase == -1 || g_iZoneStride == -1)
    {
        LogError("[BotRouteFix] [Module 2] Failed to read one or more zone/plant offsets from gamedata!");
        return false;
    }

    // 2. 解析 TheCSBots 单例指针
    if (!ResolveTheCSBots())
    {
        return false;
    }

    // 3. 准备 PlantBombClosestZone 补丁
    Address sigAddr = GameConfGetAddress(g_hGameConf, "IdleState_PlantBombClosestZone");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 2] Failed to locate signature for IdleState_PlantBombClosestZone!");
        return false;
    }

    int patchOffset = GameConfGetOffset(g_hGameConf, "PlantBombClosestZone_PatchOffset");
    if (patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 2] Failed to read PlantBombClosestZone_PatchOffset!");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);
    int patchSize = 19;
    g_PatchBombsiteLock.Init("PlantBombClosestZone_Lock", targetAddr, patchSize);

    int firstByte  = LoadFromAddress(targetAddr, NumberType_Int8);
    int secondByte = LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8);

    // 幂等性检测：检查是否已处于锁定补丁状态 (64位: 48 B8, 32位: B8)
    bool isAlreadyPatched = (g_bIsWin64 && firstByte == 0x48 && secondByte == 0xB8) || (!g_bIsWin64 && firstByte == 0xB8);
    if (isAlreadyPatched)
    {
        // 核心安全防线：已打分支绝不能标 isPatched=true（因缺失未打补丁时的原始基线备份），
        // 否则后续 round_start/掉包/死亡/换人/安包/卸载触发 Restore 时会向代码段写回全零导致崩服！
        g_PatchBombsiteLock.isPatched = false;
        g_bBombsiteLocked = false;
        LogMessage("[BotRouteFix] [Module 2] Warning: Bombsite lock patch already present @ %X on startup (skipping backup; restore will be no-op to prevent code corruption)", targetAddr);
        return true;
    }

    // 预检验证：严格比对 19 字节调用头部的特征操作码
    if (g_bIsWin64)
    {
        int chkOff64[5] = {0, 1, 2, 3, 4};
        int chkVal64[5] = {0x45, 0x33, 0xC9, 0x4C, 0x8D}; // xor r9d, r9d; lea r8, [rsp+..]
        for (int i = 0; i < 5; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff64[i]), NumberType_Int8);
            if (got != chkVal64[i])
            {
                LogError("[BotRouteFix] [Module 2] 64-bit PlantBombClosestZone template mismatch at +%d (expected %02X, got %02X)!",
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
                LogError("[BotRouteFix] [Module 2] 32-bit PlantBombClosestZone template mismatch at +%d (expected %02X, got %02X)!",
                    chkOff32[i], chkVal32[i], got);
                return false;
            }
        }
    }

    // 备份原始指令现场 (均为 19 字节)
    g_PatchBombsiteLock.BackupOriginal();
    LogMessage("[BotRouteFix] [Module 2] PlantBombClosestZone block verified @ %X [%s]",
        targetAddr, g_bIsWin64 ? "64-bit" : "32-bit");

    return true;
}

/**
 * 还原模块 2 的所有内存补丁并复位状态
 */
void RestoreModule2_Bombsite()
{
    RestoreBombsiteLockPatch();
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
}

/**
 * 动态启用或停用模块 2
 */
void EnableModule2_Bombsite(bool enable)
{
    if (!enable)
    {
        if (g_bBombsiteLocked)
        {
            RestoreBombsiteLockPatch();
        }
        g_iOpeningCarrier = 0;
        g_fCurrentPlantDelay = 0.0;
    }
}

/**
 * 查询包点是否正处于开局锁定状态
 */
bool IsBombsiteLocked()
{
    return g_bBombsiteLocked;
}

/**
 * 获取当前锁定的包点索引
 */
int GetLockedBombsiteIndex()
{
    return g_iLockedZoneIndex;
}

/**
 * 获取当前回合计算出的运包延时
 */
float GetCurrentPlantDelay()
{
    return g_fCurrentPlantDelay;
}

/**
 * 根据士气计算运包延时 (公式: 30 + 10 * Morale，区间 0.0s ~ 60.0s)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              运包延时秒数
 */
float CalculatePlantDelay(int morale)
{
    float delay = 30.0 + 10.0 * float(morale);
    if (delay < 0.0)
    {
        delay = 0.0;
    }
    else if (delay > 60.0)
    {
        delay = 60.0;
    }
    return delay;
}

//========================================================================================
// LIFECYCLE & TACTICAL EVENT HOOKS
//========================================================================================

/**
 * 回合重置 (round_start)
 */
void Module2_OnRoundStart()
{
    RestoreBombsiteLockPatch();
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
}

/**
 * 地图加载 (OnMapStart)： stale 锁定不能漏进新图（补丁字节跨换图常驻内存）
 */
void Module2_OnMapStart()
{
    RestoreBombsiteLockPatch();
    g_iOpeningCarrier = 0;
    g_fCurrentPlantDelay = 0.0;
}

/**
 * 冻结时间结束 (round_freeze_end)
 * 核心决策点：确定开局包匪、计算士气运包时钟、随机锁定目标包点
 */
void Module2_OnRoundFreezeEnd()
{
    int carrier = GetC4Carrier();
    if (!IsValidClient(carrier, true))
    {
        return;
    }

    g_iOpeningCarrier = carrier;

    // 1. 读取持包者士气（调用 helpers 标准封装：Bot 读自身，人类读存活 T 均值）
    int carrierMorale = GetEffectiveCarrierMorale(carrier);

    // 2. 士气线性映射: 30.0 + 10.0 * Morale (0.0s ~ 60.0s)
    g_fCurrentPlantDelay = CalculatePlantDelay(carrierMorale);

    // 3. 动态写入 TheCSBots()->m_earliestBombPlantTimestamp
    if (g_pTheCSBotsPtr != Address_Null && g_iOffset_EarliestBombPlant > 0)
    {
        Address pTheCSBots = LoadAddressFromAddress(g_pTheCSBotsPtr);
        if (pTheCSBots != Address_Null)
        {
            float targetTimestamp = GetGameTime() + g_fCurrentPlantDelay;
            StoreToAddress(pTheCSBots + view_as<Address>(g_iOffset_EarliestBombPlant),
                view_as<int>(targetTimestamp), NumberType_Int32);

            char moraleStr[16];
            GetMoraleName(carrierMorale, moraleStr, sizeof(moraleStr));

            DebugLog("[BotRouteFix] [Module 2] C4 Carrier %N (Morale: %d %s) -> Plant Delay: %.1fs (Earliest: %.1fs)",
                carrier, carrierMorale, moraleStr, g_fCurrentPlantDelay, targetTimestamp);
        }
    }

    // 4. 开局随机选择包点并打上内存锁定补丁
    SelectRandomBombsite();
}

/**
 * C4 掉落事件 (bomb_dropped)
 * 包匪掉包 -> 立即解除开局锁定，恢复原生动态 GetClosestZone
 */
void Module2_OnBombDropped()
{
    if (g_bBombsiteLocked)
    {
        RestoreBombsiteLockPatch();
        DebugLog("[BotRouteFix] [Module 2] C4 dropped! Restored native GetClosestZone for tactical adaptation.");
    }
    g_iOpeningCarrier = 0;
}

/**
 * 道具拾取事件 (item_pickup)
 * 检查是否有新包主产生
 */
void Module2_OnItemPickup(int client, const char[] item)
{
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
                RestoreBombsiteLockPatch();
                DebugLog("[BotRouteFix] [Module 2] C4 picked up by new carrier %N (previous: %d)! Restored native GetClosestZone.",
                    client, g_iOpeningCarrier);
            }
            g_iOpeningCarrier = client;
        }
    }
}

/**
 * 玩家死亡事件 (player_death)
 * 若开局包匪阵亡，则解除开局锁定
 */
void Module2_OnPlayerDeath(int victim)
{
    if (victim > 0 && victim == g_iOpeningCarrier)
    {
        if (g_bBombsiteLocked)
        {
            RestoreBombsiteLockPatch();
            DebugLog("[BotRouteFix] [Module 2] Opening C4 carrier %N died! Restored native GetClosestZone.", victim);
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
        RestoreBombsiteLockPatch();
    }
    g_iOpeningCarrier = 0;
}

//========================================================================================
// BOMBSITE LOCKING PRIMITIVES
//========================================================================================

/**
 * 解析 TheCSBots 单例指针
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
    ApplyLockedBombsitePatch(randomIdx);
}

/**
 * 注入包点锁定补丁，将目标包点指针硬编码写入调用现场
 *
 * @param zoneIndex     目标包点索引 (0 ~ zoneCount-1)
 */
static void ApplyLockedBombsitePatch(int zoneIndex)
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
 * 还原 GetClosestZone 原始指令现场
 */
static void RestoreBombsiteLockPatch()
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
