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
//========================================================================================

// 危险度软衰减与网格安全宏定义
#define DANGER_DECAY_RATE_SEC   120.0   // 引擎原生自然沉降速率 (1.0 每 120 秒)
#define DANGER_ROUND_RETENTION  0.2     // 开局危险度保留比例 (0.2 即软衰减 80%)
#define DANGER_NOISE_THRESHOLD  0.05    // 浮点底噪截断阈值 (低于此值直接归零)
#define NAV_AREA_MAX_SANITY     100000  // 导航网格总数安全断言上限 (防野指针读崩溃)

// 模块内部专用偏移缓存
static int g_iOffset_Danger = -1;
static int g_iOffset_DangerTimestamp = -1;
static int g_iPtrSize = 4;

/**
 * 初始化模块 3 所需的内部偏移与 TheNavAreas 容器锚点解析
 *
 * @return              是否初始化成功
 */
bool InitModule3_Danger()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 3] Error: Gamedata is not loaded, cannot initialize danger module!");
        return false;
    }

    // 1. 读取所需内部偏移
    g_iOffset_Danger          = GameConfGetOffset(g_hGameConf, "NavArea_Danger_Offset");
    g_iOffset_DangerTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_DangerTimestamp_Offset");
    g_iPtrSize                = GameConfGetOffset(g_hGameConf, "Pointer_Size");

    if (g_iPtrSize <= 0)
    {
        g_iPtrSize = g_bIsWin64 ? 8 : 4;
    }

    if (g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1)
    {
        LogError("[BotRouteFix] [Module 3] Failed to read NavArea danger offsets from gamedata!");
        return false;
    }

    // 2. 解析全局 TheNavAreas 容器地址 (CUtlVector<CNavArea*> TheNavAreas)
    if (!ResolveTheNavAreas())
    {
        return false;
    }

    return true;
}

/**
 * 模块 3 清理释放
 */
void RestoreModule3_Danger()
{
    // 无持久内存 Patch，仅做状态复位
}

/**
 * 动态启用或停用模块 3
 */
void EnableModule3_Danger(bool enable)
{
    // 模块 3 完全基于 round_start 事件驱动，事件派发器已挂载 g_cvEnable 守卫
    #pragma unused enable
}


/**
 * 回合开始事件触发 (round_start)
 * 执行全图 NavArea 危险度 80% 软衰减
 */
void Module3_OnRoundStart()
{
    DecayAllNavAreaDanger();
}

/**
 * 地图加载 (OnMapStart)：nav 网格重建地址全变，必须重解 TheNavAreas 容器
 * （拿旧指针 decay 等于往释放过的内存写；不清零只重解，danger 本就全零无需衰减）
 */
void Module3_OnMapStart()
{
    g_pTheNavAreasCount = Address_Null;
    g_pTheNavAreasBase = Address_Null;

    if (!ResolveTheNavAreas())
    {
        LogError("[BotRouteFix] [Module 3] Failed to re-resolve TheNavAreas on map start!");
    }
}

/**
 * 全图 NavArea 危险度 80% 软衰减结算
 */
void DecayAllNavAreaDanger()
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

        // 1. 读取原始危险度数值与最后更新时间戳
        float rawDangerT    = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger), NumberType_Int32));
        float rawDangerCT   = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_Danger + 4), NumberType_Int32));
        float timestampT    = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp), NumberType_Int32));
        float timestampCT   = view_as<float>(LoadFromAddress(pArea + view_as<Address>(g_iOffset_DangerTimestamp + 4), NumberType_Int32));

        // 2. 沉降结算：先扣除自上次更新以来自然流逝的衰减量 (速率 1.0 / DANGER_DECAY_RATE_SEC)
        float effDangerT = rawDangerT - ((curtime - timestampT) / DANGER_DECAY_RATE_SEC);
        if (effDangerT < 0.0)
        {
            effDangerT = 0.0;
        }

        float effDangerCT = rawDangerCT - ((curtime - timestampCT) / DANGER_DECAY_RATE_SEC);
        if (effDangerCT < 0.0)
        {
            effDangerCT = 0.0;
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

/**
 * 查询全局已加载的 NavArea 总数量
 */
int GetTotalNavAreaCount()
{
    if (g_pTheNavAreasCount == Address_Null)
    {
        return 0;
    }
    return LoadFromAddress(g_pTheNavAreasCount, NumberType_Int32);
}

/**
 * 查询指定 CNavArea 的当前有效危险度 (结算后)
 *
 * @param pArea         CNavArea 指针
 * @param dangerT       输出 T 阵营危险度
 * @param dangerCT      输出 CT 阵营危险度
 * @return              是否查询成功
 */
bool GetNavAreaDanger(Address pArea, float &dangerT, float &dangerCT)
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
// NAV AREA RESOLUTION PRIMITIVES
//========================================================================================

/**
 * 解析全局 TheNavAreas 容器指针 (CUtlVector<CNavArea*> TheNavAreas)
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
    else
    {
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

        // 逐个验候选 pair：base 先过邻近性检查（相对 anchor ±8MB，防野地址读崩服；
        // 不写死 0x10000000 基址——某些加载器会把 server.dll 重定基到 0x65 等他址），
        // 再验 count 是否 sane；首个全过即用；全灭则报错供诊断。
        Address pairAddr = Address_Null;
        int seenPairs = 0;
        for (int off = 0; off < 0x700; off++)
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
                if (baseDelta > 0x800000)
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
        if (areaCount < 0 || areaCount > 100000)
        {
            LogError("[BotRouteFix] [Module 3] Nav-area count is unreasonable (%d)!", areaCount);
            return false;
        }

        LogMessage("[BotRouteFix] [Module 3] Resolved 32-bit TheNavAreas (Base: @%X, Count: @%X, Areas: %d)",
            g_pTheNavAreasBase, g_pTheNavAreasCount, areaCount);
        return true;
    }
}
