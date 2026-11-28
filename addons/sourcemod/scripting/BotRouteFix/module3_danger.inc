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
//   - 唯一事项 = danger soft-decay；后端 = danger/timestamp 偏移 + NavAreas 容器就绪态。
//   - 本模块没有常驻补丁；换图随 NavAreas_OnMapStart() 重解后同步刷新事项就绪与武装态。
//   - TheNavAreas 容器归第 0 层 nav_measure.inc 无条件维护；本模块不再承载跨模块导航基建。
//========================================================================================

//========================================================================================
// MODULE 3 DEFINES & GLOBALS
//========================================================================================

// 危险度软衰减宏定义（沉降速率 DANGER_DECAY_RATE_SEC 统一定义于 global.inc）
#define DANGER_ROUND_RETENTION      0.2     // 开局保留 20% 危险度（即衰减 80%）
#define DANGER_NOISE_THRESHOLD      0.05    // 低于该值直接归零，消除浮点底噪

// Danger/DangerTimestamp 与 TheNavAreas 容器均由 nav_measure.inc 统一解析与维护，本文件只用不存
// 模块内部启用缓存（仅用于 Enable 幂等短路；业务判据统一用 Item_IsArmed(ITEM_M3_DECAY)）
static bool g_bModule3_Active = false;

//========================================================================================
// MODULE 3 ITEM BACKENDS (called by Init / MapStart)
//========================================================================================

/**
 * 事项后端：危险度软衰减
 *
 * 只做校验、偏移缓存与 TheNavAreas 容器就绪门禁，不武装事项。
 * 任一后端缺失都返回 false，对应事项由 Init / OnMapStart 标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，对应事项标记 FAILED。
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

    // 偏移与 TheNavAreas 容器由 InitNavMeasure / NavAreas_OnMapStart 统一解析，此处只做门禁
    if (g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
    {
        LogError("[BotRouteFix] [Module 3] Danger offsets missing (danger=%d, timestamp=%d); decay item FAILED.",
            g_iOffset_Danger, g_iOffset_DangerTimestamp);
        return false;
    }

    LogMessage("[BotRouteFix] [Module 3] Danger offsets verified (danger=%d, timestamp=%d).",
        g_iOffset_Danger, g_iOffset_DangerTimestamp);

    if (!NavAreas_IsReady())
    {
        LogError("[BotRouteFix] [Module 3] TheNavAreas container unavailable; decay item FAILED.");
        return false;
    }

    return true;
}

//========================================================================================
// MODULE 3 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 3 的唯一事项：全图危险度软衰减（ITEM_M3_DECAY）
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
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
    if (!g_bModule3_Active)
    {
        LogModuleAllFailed(3);
        return false;
    }

    LogModuleArmed(3, "Danger decay", "1");
    return true;
}

/**
 * 动态启用或停用模块 3（事项 ITEM_M3_DECAY 的调度入口）
 *
 * 事项没有常驻补丁，Enable(false) 只撤销武装标记；round_start 不再执行衰减。
 * 失败语义：只做调度，无返回值；武装态 = enable && Ready。
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
 * 还原模块 3（事项 ITEM_M3_DECAY；插件卸载：无持久补丁，只复位业务态）。
 *
 * 本模块无常驻补丁与钩子，故无 Apply/RestoreBackend，Enable 即直写 Armed。
 * 失败语义：无（尽力还原；幂等）。
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

    if (!IsValidAddress(pArea) || g_iOffset_Danger <= 0 || g_iOffset_DangerTimestamp <= 0)
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

/**
 * 遍历 TheNavAreas，逐 area 读沉降后双阵营 danger，写 CSV；摘要同时进
 * 服务端日志（LogMessage）与调用者回显（ReplyToCommand）。
 *
 * 事项 M3 高危可视化与手动落盘的落盘入口（跨模块只读观测 + 文件输出）。
 * 失败语义：容器未解析/空或异常/文件打不开，一律 LogError 并返回（无文件或空文件）。
 * 门禁：无（命令即授权；与 M3 事项启停解耦）。
 *
 * @param tag           文件名标签（data/botroutefix_danger_<tag>.csv）
 * @param client        回显目标（0 = 服务端控制台）
 */
void Module3_DumpDangerDist(const char[] tag, int client = 0)
{
    if (!NavAreas_IsReady())
    {
        LogError("[BotRouteFix] [Module 3] Dump aborted: TheNavAreas unresolved.");
        return;
    }
    int count = NavAreas_GetCount();
    if (count <= 0)
    {
        LogError("[BotRouteFix] [Module 3] Dump aborted: TheNavAreas empty or invalid.");
        return;
    }

    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "data/botroutefix_danger_%s.csv", tag);
    Handle hFile = OpenFile(path, "w");
    if (hFile == null)
    {
        LogError("[BotRouteFix] [Module 3] Dump aborted: cannot open %s.", path);
        return;
    }

    WriteFileLine(hFile, "index,danger_t,danger_ct");

    int binsT[8];
    int binsCT[8];
    float maxT = 0.0, maxCT = 0.0, sumT = 0.0, sumCT = 0.0;
    int nzT = 0, nzCT = 0, written = 0;

    for (int i = 0; i < count; i++)
    {
        Address pArea = NavAreas_GetAt(i);
        if (!IsValidAddress(pArea))
        {
            continue;
        }
        float dT, dCT;
        if (!Module3_GetNavAreaDanger(pArea, dT, dCT))
        {
            continue;
        }
        WriteFileLine(hFile, "%d,%.3f,%.3f", i, dT, dCT);
        written++;

        binsT[DangerDistBin(dT)]++;
        binsCT[DangerDistBin(dCT)]++;
        if (dT > 0.0) { nzT++; sumT += dT; if (dT > maxT) { maxT = dT; } }
        if (dCT > 0.0) { nzCT++; sumCT += dCT; if (dCT > maxCT) { maxCT = dCT; } }
    }
    delete hFile;

    LogMessage("[BotRouteFix] [Module 3] Dump %s: %d/%d areas -> %s", tag, written, count, path);
    LogMessage("[BotRouteFix] [Module 3] Dist %s T : nz=%d max=%.2f mean=%.3f bins[0|0.05|0.2|0.5|1|2|5|inf]=%d %d %d %d %d %d %d %d",
        tag, nzT, maxT, sumT / float(count), binsT[0], binsT[1], binsT[2], binsT[3], binsT[4], binsT[5], binsT[6], binsT[7]);
    LogMessage("[BotRouteFix] [Module 3] Dist %s CT: nz=%d max=%.2f mean=%.3f bins[0|0.05|0.2|0.5|1|2|5|inf]=%d %d %d %d %d %d %d %d",
        tag, nzCT, maxCT, sumCT / float(count), binsCT[0], binsCT[1], binsCT[2], binsCT[3], binsCT[4], binsCT[5], binsCT[6], binsCT[7]);
    ReplyToCommand(client, "[BotRouteFix] [Module 3] Dump %s: %d/%d areas -> %s", tag, written, count, path);
    ReplyToCommand(client, "[BotRouteFix] [Module 3] T : nz=%d max=%.2f mean=%.3f bins=%d %d %d %d %d %d %d %d",
        nzT, maxT, sumT / float(count), binsT[0], binsT[1], binsT[2], binsT[3], binsT[4], binsT[5], binsT[6], binsT[7]);
    ReplyToCommand(client, "[BotRouteFix] [Module 3] CT: nz=%d max=%.2f mean=%.3f bins=%d %d %d %d %d %d %d %d",
        nzCT, maxCT, sumCT / float(count), binsCT[0], binsCT[1], binsCT[2], binsCT[3], binsCT[4], binsCT[5], binsCT[6], binsCT[7]);
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
 * 地图加载 (OnMapStart)：TheNavAreas 容器已由 botroutefix.sp 首步调 NavAreas_OnMapStart() 无条件重解，
 * 此处同步刷新 M3 自身的 Ready / Armed 事项态（规则关闭态亦同步 Ready，避免后续启用残留旧态）。
 */
void Module3_OnMapStart()
{
    bool resolved = PrepModule3_DecayBackend();
    Item_SetReady(ITEM_M3_DECAY, resolved);

    if (!resolved)
    {
        EnableModule3_Danger(false);
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
    if (!NavAreas_IsReady())
    {
        return;
    }

    int count = NavAreas_GetCount();
    if (count <= 0)
    {
        return;
    }

    float curtime = GetGameTime();
    int decayCount = 0;

    for (int i = 0; i < count; i++)
    {
        Address pArea = NavAreas_GetAt(i);
        if (!IsValidAddress(pArea))
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

    DebugLog("[BotRouteFix] [Module 3] Decayed danger (80%% reduction) across %d NavAreas (%d active) for round start.",
        count, decayCount);
}

/**
 * 直方图分箱：0 | (0,0.05) | [0.05,0.2) | [0.2,0.5) | [0.5,1) | [1,2) | [2,5) | [5,+inf)
 *
 * 事项 M3 高危可视化收集与手动落盘的共用分箱原语（纯函数，无失败态）。
 */
static int DangerDistBin(float d)
{
    if (d <= 0.0) return 0;
    if (d < 0.05) return 1;
    if (d < 0.2)  return 2;
    if (d < 0.5)  return 3;
    if (d < 1.0)  return 4;
    if (d < 2.0)  return 5;
    if (d < 5.0)  return 6;
    return 7;
}


