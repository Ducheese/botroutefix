#if defined _botroutefix_module3_danger_included
    #endinput
#endif
#define _botroutefix_module3_danger_included

//========================================================================================
// [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
//
// 业务背景与引擎逆向分析：
//
// 1. CNavArea 危险度存储结构与访问下界 (nav_area.h:683-684):
//    - 数据成员:
//        float m_danger[MAX_NAV_TEAMS];          // 偏移相邻 +8 (32位 188, 64位 256)
//        float m_dangerTimestamp[MAX_NAV_TEAMS]; // (32位 196, 64位 264)
//    - 队伍下标映射: teamID % MAX_NAV_TEAMS (nav_area.cpp:4164/4178)
//        * T (Team 2)  -> 下标 0 (m_danger + 0)
//        * CT (Team 3) -> 下标 1 (m_danger + 4)
//
// 2. 引擎原生衰减机制与"沉降契约" (nav_area.cpp:4139-4180):
//    - 原生衰减速率: rate = 1.0f / 120.0f (即 1 次击杀增加 1.0 危险度，2分钟/120秒后彻底遗忘)。
//    - 原生读写模型: "读即衰减 (Lazy Decay)"。内存中存储的裸值是陈旧的，只有在执行
//      GetDanger() 或 IncreaseDanger() 时才会根据 (curtime - timestamp) 动态扣减。
//    - ★ 沉降契约 (Settlement Contract):
//      任何插件若直接操作底层 danger 裸值，必须首先根据流逝时间结算沉降:
//        effDanger = rawDanger - ((curtime - timestamp) / 120.0f)
//      否则读取到的是陈旧虚高数值；修改后必须将 timestamp 同步更新为 curtime，
//      否则下一次引擎原生 GetDanger() 结算时会将写入值直接当作历史旧值扣减归零。
//
// 3. 回合初 80% 软衰减 (保留 20% 残余记忆) 与底噪截断:
//    - 每回合开始 (round_start) 时，全图遍历 TheNavAreas，对所有区域危险度执行 80% 衰减:
//        newDanger = effDanger * 0.2f
//      衰减后若 newDanger < 0.05 则截断归零 (0.0)，消除微弱浮点底噪，确保数回合后旧战区彻底清零沉寂。
//    - 战术价值: 既清除了大部分上一回合交火留下的过度偏见，又保留了 20% 的战场残存热点记忆，
//      为 Bot 提供自然且富于变化的动态寻路权衡，避免死板走固定路线。
//
// 事项契约:
//   - 唯一事项 = danger soft-decay；后端 = danger/timestamp 偏移 + TheNavAreas 容器解析。
//   - 本模块没有常驻补丁；换图只需重新解析 nav 容器，不需要恢复代码段。
//   - 规则关闭时仍刷新 Ready，避免下次启用时写入已释放的旧网格。
//========================================================================================

//========================================================================================
// MODULE 3 DEFINES & GLOBALS
//========================================================================================

// 危险度软衰减与网格安全宏定义（沉降速率 DANGER_DECAY_RATE_SEC 统一定义于 global.inc）
#define DANGER_ROUND_RETENTION      0.2     // 开局保留 20% 危险度（即衰减 80%）
#define DANGER_NOISE_THRESHOLD      0.05    // 低于该值直接归零，消除浮点底噪
#define NAV_AREA_MAX_SANITY         100000  // 导航网格数量合理性上限
#define NAV_AREA_PAIR_SCAN_BYTES    0x700   // 32 位锚点后的候选指令扫描窗口
#define NAV_AREA_PAIR_MAX_DELTA     0x800000 // 候选 base 与锚点的最大距离

static int g_iOffset_Danger = -1;
static int g_iOffset_DangerTimestamp = -1;
static int g_iPtrSize = 4;

// 模块内部启用缓存（仅用于 Enable 幂等短路；业务判据统一用 Item_IsArmed(ITEM_M3_DECAY)）
static bool g_bModule3_Active = false;

//========================================================================================
// MODULE 3 ITEM BACKENDS (called by Init / MapStart)
//========================================================================================

/**
 * 事项后端：危险度软衰减
 *
 * 只做校验、偏移缓存与 TheNavAreas 解析，不武装事项。
 * 任一后端缺失都返回 false，对应事项由 Init 标记为 FAILED。
 *
 * @return              true = 衰减事项后端全部就绪
 */
static bool PrepModule3_DecayBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 3] Gamedata missing; decay item FAILED.");
        return false;
    }

    g_iOffset_Danger          = GameConfGetOffset(g_hGameConf, "NavArea_Danger_Offset");
    g_iOffset_DangerTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_DangerTimestamp_Offset");
    g_iPtrSize                = GameConfGetOffset(g_hGameConf, "Pointer_Size");

    if (g_iPtrSize <= 0)
    {
        g_iPtrSize = g_bIsWin64 ? 8 : 4;
    }

    if (g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
    {
        LogError("[BotRouteFix] [Module 3] Danger offsets missing (danger=%d, timestamp=%d); decay item FAILED.",
            g_iOffset_Danger, g_iOffset_DangerTimestamp);
        return false;
    }

    LogMessage("[BotRouteFix] [Module 3] Danger offsets verified (danger=%d, timestamp=%d).",
        g_iOffset_Danger, g_iOffset_DangerTimestamp);

    if (!ResolveTheNavAreas())
    {
        return false;
    }

    return true;
}

/**
 * 事项 M3 危险度衰减的 NavArea 容器解析
 *
 * 64 位：从 CCSBotManager::GetRandomArea 的 mov ecx, cs:TheNavAreas.m_Size 定位。
 * 32 位：从 HuntState::OnUpdate 的 NavArray 对特征扫描定位。
 * 失败语义：签名缺失或容器不 sane 返回 false，Init 将衰减事项标记 FAILED。
 *
 * @return              true = 容器 base/count 均有效
 */
static bool ResolveTheNavAreas()
{
    if (g_pTheNavAreasCount != Address_Null && g_pTheNavAreasBase != Address_Null)
    {
        return true;
    }

    if (g_bIsWin64)
    {
        // -------------------------------------------------------------------------------
        // 64-bit 实现 (Steam x64)
        // 通过 CCSBotManager::GetRandomArea 的 64 位特征指令定位 TheNavAreas
        // 指令序列: mov ecx, cs:TheNavAreas.m_Size (8B 0D <rel32> @ sigAddr + 14)
        // -------------------------------------------------------------------------------
        Address sigAddr = GameConfGetAddress(g_hGameConf, "CCSBotManager_GetRandomArea");
        if (sigAddr == Address_Null)
        {
            LogError("[BotRouteFix] [Module 3] Failed to locate signature for CCSBotManager_GetRandomArea!");
            return false;
        }

        int relOffset = LoadFromAddress(sigAddr + view_as<Address>(16), NumberType_Int32);
        g_pTheNavAreasCount = sigAddr + view_as<Address>(20 + relOffset);
        // CUtlVector 内存布局: m_pMemory 位于 m_Size 前面 16 字节
        g_pTheNavAreasBase = g_pTheNavAreasCount - view_as<Address>(16);

        int areaCount = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
        if (areaCount < 0 || areaCount > NAV_AREA_MAX_SANITY)
        {
            LogError("[BotRouteFix] [Module 3] 64-bit areaCount (%d) out of reasonable range [0, %d]!",
                areaCount, NAV_AREA_MAX_SANITY);
            return false;
        }

        LogMessage("[BotRouteFix] [Module 3] Resolved 64-bit TheNavAreas (Base: @%X, Count: @%X, Areas: %d)",
            g_pTheNavAreasBase, g_pTheNavAreasCount, areaCount);
        return true;
    }

    // -------------------------------------------------------------------------------
    // 32-bit 实现 (v91/v92)
    // 通过 HuntState::OnUpdate 内部稳定的 NavArray 对特征扫描定位:
    //   mov ecx, [TheNavAreasBase] / mov ecx, [ecx + edx*4] (8B 0D <Base> 8B 0C 91)
    // 该特征在 CS:S 32 位二进制中全局唯一，Count = Base + 12
    // -------------------------------------------------------------------------------
    Address huntAddr = GameConfGetAddress(g_hGameConf, "HuntState_NavArrayAnchor");
    if (huntAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 3] Failed to locate signature for HuntState_NavArrayAnchor!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 3] Hunt anchor @ %X, scanning nav-array pairs...", huntAddr);

    Address pairAddr = Address_Null;
    int seenPairs = 0;

    // 逐个验候选 pair：base 先过邻近性检查（相对 anchor ±8MB，防野地址读崩服；
    // 不写死 0x10000000 基址——某些加载器会把 server.dll 重定基到 0x65 等他址），
    // 再验 count 是否 sane；首个全过即用；全灭则报错供诊断。
    for (int off = 0; off < NAV_AREA_PAIR_SCAN_BYTES; off++)
    {
        if (LoadFromAddress(huntAddr + view_as<Address>(off), NumberType_Int8) == 0x8B
            && LoadFromAddress(huntAddr + view_as<Address>(off + 1), NumberType_Int8) == 0x0D
            && LoadFromAddress(huntAddr + view_as<Address>(off + 6), NumberType_Int8) == 0x8B
            && LoadFromAddress(huntAddr + view_as<Address>(off + 7), NumberType_Int8) == 0x0C
            && LoadFromAddress(huntAddr + view_as<Address>(off + 8), NumberType_Int8) == 0x91)
        {
            seenPairs++;
            Address candAddr = huntAddr + view_as<Address>(off);
            int candBase = LoadFromAddress(candAddr + view_as<Address>(2), NumberType_Int32);

            int baseDelta = candBase - view_as<int>(huntAddr);
            if (baseDelta < 0)
            {
                baseDelta = -baseDelta;
            }
            if (baseDelta > NAV_AREA_PAIR_MAX_DELTA)
            {
                LogMessage("[BotRouteFix] [Module 3] Candidate pair @ %X skipped (base %X too far from anchor)",
                    candAddr, candBase);
                continue;
            }

            int candCount = LoadFromAddress(view_as<Address>(candBase + 12), NumberType_Int32);
            if (candCount < 0 || candCount > NAV_AREA_MAX_SANITY)
            {
                LogMessage("[BotRouteFix] [Module 3] Candidate pair @ %X skipped (count %d insane)",
                    candAddr, candCount);
                continue;
            }

            pairAddr = candAddr;
            break;
        }
    }

    if (pairAddr == Address_Null)
    {
        if (seenPairs == 0)
        {
            LogError("[BotRouteFix] [Module 3] Nav-array pair pattern not found in HuntState::OnUpdate!");
        }
        else
        {
            LogError("[BotRouteFix] [Module 3] %d nav-array pair(s) seen from anchor @ %X but none validated!",
                seenPairs, huntAddr);
        }
        return false;
    }

    int baseAddr = LoadFromAddress(pairAddr + view_as<Address>(2), NumberType_Int32);
    g_pTheNavAreasBase  = view_as<Address>(baseAddr);
    g_pTheNavAreasCount = view_as<Address>(baseAddr + 12);

    int areaCount = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
    if (areaCount < 0 || areaCount > NAV_AREA_MAX_SANITY)
    {
        LogError("[BotRouteFix] [Module 3] Nav-area count is unreasonable (%d)!", areaCount);
        return false;
    }

    LogMessage("[BotRouteFix] [Module 3] Resolved 32-bit TheNavAreas (Base: @%X, Count: @%X, Areas: %d)",
        g_pTheNavAreasBase, g_pTheNavAreasCount, areaCount);
    return true;
}

//========================================================================================
// MODULE 3 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 3 的唯一事项：全图危险度软衰减
 *
 * @return              true = 事项已武装
 */
bool InitModule3_Danger()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M3_DECAY, false);
        LogError("[BotRouteFix] [Module 3] Gamedata missing; decay item FAILED.");
        return false;
    }

    bool decayReady = PrepModule3_DecayBackend();
    Item_SetReady(ITEM_M3_DECAY, decayReady);

    EnableModule3_Danger(true);
    if (!Item_IsArmed(ITEM_M3_DECAY))
    {
        LogModuleAllFailed(3);
        return false;
    }

    LogModuleArmed(3, "Nav danger decay", Item_IsArmed(ITEM_M3_DECAY) ? "1" : "0");
    return true;
}

/**
 * 动态启用或停用模块 3
 *
 * 事项没有常驻补丁，Enable(false) 只撤销武装标记；round_start 不再执行衰减。
 *
 * @param enable        true = 武装；false = 解除武装
 */
void EnableModule3_Danger(bool enable)
{
    if (enable == g_bModule3_Active)
    {
        return;
    }

    Item_SetArmed(ITEM_M3_DECAY, enable && Item_IsReady(ITEM_M3_DECAY));
    g_bModule3_Active = Item_IsArmed(ITEM_M3_DECAY);
}

/**
 * 还原模块 3（插件卸载）
 *
 * 本模块没有持久代码补丁，只复位业务态。
 */
void RestoreModule3_Danger()
{
    EnableModule3_Danger(false);
    LogMessage("[BotRouteFix] [Module 3] Danger decay state reset (no persistent patch).");
}

//========================================================================================
// MODULE 3 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 查询当前 nav 网格区域总数
 *
 * @return              TheNavAreas 中的区域数量；0 = 容器未解析
 */
int Module3_GetTotalNavAreaCount()
{
    if (g_pTheNavAreasCount == Address_Null)
    {
        return 0;
    }
    return LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
}

/**
 * 查询指定 CNavArea 的当前有效危险度
 *
 * 读取时先按时间结算，避免把陈旧裸值当成真实危险度。
 *
 * @param pArea         CNavArea 指针
 * @param dangerT       输出 T 阵营有效危险度
 * @param dangerCT      输出 CT 阵营有效危险度
 * @return              true = 查询成功；false = 指针或偏移不可用
 */
bool Module3_GetNavAreaDanger(Address pArea, float &dangerT, float &dangerCT)
{
    dangerT = 0.0;
    dangerCT = 0.0;

    if (pArea == Address_Null || g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
    {
        return false;
    }

    float curtime = GetGameTime();

    float rawDangerT  = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger), NumberType_Int32));
    float rawDangerCT = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + 4), NumberType_Int32));
    float timestampT  = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp), NumberType_Int32));
    float timestampCT = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + 4), NumberType_Int32));

    dangerT = rawDangerT - ((curtime - timestampT) / DANGER_DECAY_RATE_SEC);
    if (dangerT < 0.0)
    {
        dangerT = 0.0;
    }

    dangerCT = rawDangerCT - ((curtime - timestampCT) / DANGER_DECAY_RATE_SEC);
    if (dangerCT < 0.0)
    {
        dangerCT = 0.0;
    }

    return true;
}

//========================================================================================
// MODULE 3 EVENT HANDLERS
//========================================================================================

/**
 * 回合开始事件：对全图 NavArea 执行一次危险度软衰减
 */
void Module3_OnRoundStart()
{
    if (!Item_IsArmed(ITEM_M3_DECAY))
    {
        return;
    }

    DecayAllNavAreaDanger();
}

/**
 * 地图加载 (OnMapStart)：nav 网格重建地址全变，必须重解 TheNavAreas 容器
 * （拿旧指针 decay 等于往释放过的内存写；不清零只重解，danger 本就全零无需衰减）
 *
 * 【例外：不随业务态守卫】对应 events.inc 契约 (c)。
 *   模块关闭时若不重建，g_pTheNavAreas* 会停留在已释放的旧网格上，下次启用即野指针写。
 */
void Module3_OnMapStart()
{
    g_pTheNavAreasCount = Address_Null;
    g_pTheNavAreasBase = Address_Null;

    bool resolved = PrepModule3_DecayBackend();
    Item_SetReady(ITEM_M3_DECAY, resolved);

    if (!resolved)
    {
        EnableModule3_Danger(false);
        LogError("[BotRouteFix] [Module 3] Failed to re-resolve TheNavAreas on map start; decay item FAILED.");
        return;
    }

    EnableModule3_Danger(IsModuleEnabledByRule(3));
}

//========================================================================================
// MODULE 3 INTERNAL PRIMITIVES
//========================================================================================

/**
 * 对全图 NavArea 执行危险度软衰减
 *
 * 先结算自然衰减，再保留 DANGER_ROUND_RETENTION，最后同步 timestamp。
 */
static void DecayAllNavAreaDanger()
{
    if (g_pTheNavAreasCount == Address_Null || g_pTheNavAreasBase == Address_Null)
    {
        return;
    }

    int count = LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
    if (count <= 0)
    {
        return;
    }

    Address pArray = LoadAddressFromAddress(g_pTheNavAreasBase);
    if (pArray == Address_Null)
    {
        return;
    }

    float curtime = GetGameTime();
    int decayCount = 0;

    for (int i = 0; i < count; i++)
    {
        Address pArea = LoadAddressFromAddress(pArray + view_as<Address>(i * g_iPtrSize));
        if (pArea == Address_Null)
        {
            continue;
        }

        // 1-2. 沉降结算：统一走 Module3_GetNavAreaDanger，避免沉降公式多处重复
        float effDangerT, effDangerCT;
        if (!Module3_GetNavAreaDanger(pArea, effDangerT, effDangerCT))
        {
            continue;
        }

        // 3. 软衰减：按设定比例保留残余记忆 (默认保留 20% 即软衰减 80%)
        float newDangerT  = effDangerT * DANGER_ROUND_RETENTION;
        float newDangerCT = effDangerCT * DANGER_ROUND_RETENTION;

        // 噪声过滤截断
        if (newDangerT < DANGER_NOISE_THRESHOLD)
        {
            newDangerT = 0.0;
        }
        if (newDangerCT < DANGER_NOISE_THRESHOLD)
        {
            newDangerCT = 0.0;
        }

        // 4. 写回衰减后的危险度并重置时间戳为 curtime
        StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger), view_as<int>(newDangerT), NumberType_Int32);
        StoreToAddress(pArea + view_as<Address>(g_iOffset_Danger + 4), view_as<int>(newDangerCT), NumberType_Int32);
        StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp), view_as<int>(curtime), NumberType_Int32);
        StoreToAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + 4), view_as<int>(curtime), NumberType_Int32);

        if (newDangerT > 0.0 || newDangerCT > 0.0)
        {
            decayCount++;
        }
    }

    DebugLog("[BotRouteFix] ============== Round Start ==============");
    DebugLog("[BotRouteFix] [Module 3] Decayed danger (80%% reduction) across %d NavAreas (%d active) for round start.",
        count, decayCount);
}


