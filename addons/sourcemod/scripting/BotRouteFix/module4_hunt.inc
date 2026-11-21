#if defined _botroutefix_module4_hunt_included
    #endinput
#endif
#define _botroutefix_module4_hunt_included

//========================================================================================
// [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
//
// 业务背景与引擎逆向分析：
//
// 1. 45秒开局敌方老家短路锁定 (cs_bot_hunt.cpp:164-175):
//    - 源码逻辑:
//        const float earlyGameTime = 45.0f;
//        if (TheCSBots()->GetElapsedRoundTime() < earlyGameTime && !me->HasVisitedEnemySpawn())
//        {
//            CBaseEntity *enemySpawn = TheCSBots()->GetRandomSpawn( OtherTeam( me->GetTeamNumber() ) );
//            if ( enemySpawn )
//                m_huntArea = TheNavMesh->GetNavArea( enemySpawn->WorldSpaceCenter() );
//        }
//    - 机制痛点: 前 45 秒内只要进入 HuntState，Bot 目标区域强制锁死为敌方基地，
//               导致双方开局 10~20 秒必在中路或主干道扎堆撞车，战术深度尽失。
//    - 解决机制: 在 Pre 钩子中直接将 Bot 结构体内的 `m_hasVisitedEnemySpawn` (紧随 `m_isStopping`
//               之后的 1 字节 bool，32位 +7201 / 64位 +8449) 置为 true (1)，使条件判定恒假，
//               彻底跳过老家锁定分支，从第 0 秒即走全图区域搜索。
//
// 2. 最久未清理 (Oldest-Cleared) 扫描与 Valve 遗漏的 if (cs_bot_hunt.cpp:178-217):
//    - 源码逻辑:
//        m_huntArea = NULL; float oldest = 0.0f;
//        FOR_EACH_VEC( TheNavAreas, it ) { ... if (age > oldest) { oldest = age; m_huntArea = area; } }
//        // if all the areas were too small, pick one at random
//        int which = RandomInt( 0, areaCount-1 );
//        FOR_EACH_VEC( TheNavAreas, hit ) { m_huntArea = TheNavAreas[hit]; if (which == areaCount) break; --which; }
//    - 机制痛点: 第二段随机选取逻辑 Valve 完全漏掉了 `if (m_huntArea == NULL)` 守卫！
//               导致第一段辛辛苦苦扫描出来的"最久未清理区域"被无条件随机覆盖成全图任意 area，
//               引擎原生基于 `m_clearedTimestamp` 的搜敌认领机制名存实亡。
//    - 补丁方案: NOP 掉随机覆盖循环中对 `m_huntArea` 的写回指令 (不碰 RandomInt 与循环计数):
//        * 32 位: `89 4B 04` (mov [ebx+4], ecx) -> 3x NOP
//        * 64 位: `49 89 56 08` (mov [r14+8], rdx) -> 4x NOP
//      使最久未清理扫描结果得以保留，赋予时间戳认领机制真实的数学支点。
//
// 3. 目标区域盖章认领 (Target Area Claiming):
//    - 对应结构: `CNavArea::m_clearedTimestamp[MAX_NAV_TEAMS]` (32位 180, 64位 248)。
//    - 下标约定: `teamID = me->GetTeamNumber() - 1`，下标 `(team - 1) % 2`:
//        * T  (Team 2) -> (2 - 1) % 2 = 1 (m_clearedTimestamp + 4)
//        * CT (Team 3) -> (3 - 1) % 2 = 0 (m_clearedTimestamp + 0)
//    - 闭环效果: 在 Post 钩子中将当前确定的 `m_huntArea` 对应阵营时间戳盖上 `curtime` (age 归零)。
//      由于扫描逻辑是严格大于 `age > oldest` (oldest 起始 0.0f)，刚被队友认领的区域 age 恒为 0，
//      同队其他 Bot 绝不会重复选取，实现完全无排队无死锁的天然多路分散搜敌！
//    - 容错与自愈策略: 随机覆盖补丁支持通过 HuntState_RandomBypass_PatchOffset_Alt 备用偏移自愈迁址；
//      若仍失配则 ITEM_M4_NOP 标记为 FAILED；ITEM_M4_HUNT 独立运行（45s 破除与认领盖戳正常工作），
//      模块整体报告 DEGRADED，不阻断主插件启动。
//
// 事项契约:
//   - 两个独立事项：random-overwrite NOP / HuntState dhook writes（45s bypass + 认领）。
//   - NOP 失败只让 NOP FAILED，dhook 事项仍可工作，模块显示 DEGRADED。
//   - NOP 的写回与基线备份分离，禁止无基线时假装可用；dhook 的 Pre/Post 必须同时成功。
//========================================================================================

//========================================================================================
// MODULE 4 DEFINES & GLOBALS
//========================================================================================

#define HUNT_RANDOM_NOP_SIZE_64          4
#define HUNT_RANDOM_NOP_SIZE_32          3
#define HUNT_VISUAL_HEARTBEAT_SEC        0.3
#define HUNT_EARLY_GAME_BYPASS_VALUE     1

static MemoryPatch g_PatchHuntRandomBypass;
static Handle g_hHuntStateDetour = null;

// 模块内部启用缓存（仅用于 Enable 幂等短路；逐事项判据统一用 Item_IsArmed(ITEM_M4_NOP/HUNT)）
static bool g_bModule4_Active = false;
static bool g_bHuntPreHooked = false;
static bool g_bHuntPostHooked = false;

static int g_iOffset_HuntArea = -1;
static int g_iOffset_ClearedTimestamp = -1;
static int g_iOffset_HasVisitedEnemySpawn = -1;

static int g_iTotalHuntClaims = 0;
// 【编译器缺陷补偿】同 M11：全局 Address 数组 4B/元分配、8B 步进，故声明双倍长度。
static Address g_pVisualHuntArea[(MAXPLAYERS + 1) * 2];
static float g_flVisualHuntTimestamp[MAXPLAYERS + 1];

//========================================================================================
// MODULE 4 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：随机覆盖 NOP
 *
 * 只定位目标、校验模板并备份原始字节，不写入 NOP。
 * 任一后端失败都返回 false，Init 会把 NOP 事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = NOP 事项后端就绪
 */
static bool PrepModule4_NopBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 4] Gamedata missing; NOP item FAILED.");
        return false;
    }

    Address sigAddr = GameConfGetAddress(g_hGameConf, "HuntState_RandomBypass");
    int patchOffset = GameConfGetOffset(g_hGameConf, "HuntState_RandomBypass_PatchOffset");
    if (sigAddr == Address_Null || patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 4] Random bypass signature/offset missing; NOP item FAILED.");
        return false;
    }

    int patchSize = g_bIsWin64 ? HUNT_RANDOM_NOP_SIZE_64 : HUNT_RANDOM_NOP_SIZE_32;

    // 预检验证：严格比对写回指令操作码 (32位: 89 4B 04, 64位: 49 89 56 08)
    Address targetAddr = sigAddr + view_as<Address>(patchOffset);

    if (!IsHuntRandomBypassInstruction(targetAddr))
    {
        // 主偏移模板失配时试备用偏移（v91/v92 同函数差 3 字节：1241/1238），命中即迁址
        int altOffset = GameConfGetOffset(g_hGameConf, "HuntState_RandomBypass_PatchOffset_Alt");
        if (altOffset != -1 && altOffset != patchOffset)
        {
            Address altAddr = sigAddr + view_as<Address>(altOffset);
            if (IsHuntRandomBypassInstruction(altAddr))
            {
                LogMessage("[BotRouteFix] [Module 4] Random bypass relocated to alt offset %d @ %X",
                    altOffset, altAddr);
                targetAddr = altAddr;
            }
        }
    }

    if (!IsHuntRandomBypassInstruction(targetAddr))
    {
        LogError("[BotRouteFix] [Module 4] Random bypass template mismatch @ %X; NOP item FAILED.", targetAddr);
        return false;
    }

    g_PatchHuntRandomBypass.Init("HuntRandom_NOP", targetAddr, patchSize);

    // 幂等性检测：检查是否已全部为 NOP (0x90)
    // 有备份 = 自家补丁，保持旗标；无备份 = 外来已旁路，记 false（无基线可还原）
    if (!g_PatchHuntRandomBypass.hasBackup)
    {
        bool allNop = true;
        for (int i = 0; i < patchSize; i++)
        {
            if (LoadFromAddress(targetAddr + view_as<Address>(i), NumberType_Int8) != 0x90)
            {
                allNop = false;
                break;
            }
        }

        if (allNop)
        {
            LogError("[BotRouteFix] [Module 4] Random bypass already NOP without baseline @ %X; NOP item FAILED.",
                targetAddr);
            return false;
        }
    }

    if (!g_PatchHuntRandomBypass.BackupOriginal())
    {
        LogError("[BotRouteFix] [Module 4] Random bypass baseline backup failed; NOP item FAILED.");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 4] Random bypass block verified @ %X [%s]",
        targetAddr, g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 事项后端：HuntState dhook writes（ITEM_M4_HUNT）
 *
 * 条件 = hunt area 偏移 + cleared timestamp 偏移 + hasVisitedEnemySpawn 偏移 + Detour 挂载。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = HuntState 事项后端就绪
 */
static bool PrepModule4_HuntBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 4] Gamedata missing; HuntState item FAILED.");
        return false;
    }

    g_iOffset_HuntArea = GameConfGetOffset(g_hGameConf, "HuntState_HuntArea_Offset");
    g_iOffset_ClearedTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_ClearedTimestamp_Offset");

    int isStoppingOffset = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");
    g_iOffset_HasVisitedEnemySpawn = (isStoppingOffset > 0) ? (isStoppingOffset + 1) : -1;

    if (g_iOffset_HuntArea <= 0 || g_iOffset_ClearedTimestamp <= 0 || g_iOffset_HasVisitedEnemySpawn <= 0)
    {
        LogError("[BotRouteFix] [Module 4] HuntState offsets missing (area=%d, cleared=%d, visited=%d); HuntState item FAILED.",
            g_iOffset_HuntArea, g_iOffset_ClearedTimestamp, g_iOffset_HasVisitedEnemySpawn);
        return false;
    }

    LogMessage("[BotRouteFix] [Module 4] HuntState offsets verified (area=%d, cleared=%d, visited=%d).",
        g_iOffset_HuntArea, g_iOffset_ClearedTimestamp, g_iOffset_HasVisitedEnemySpawn);
    return MountHuntStateDetour();
}

/**
 * 挂载 HuntState::OnUpdate Detour（事项 ITEM_M4_HUNT 的执行载体）
 *
 * 失败语义：句柄创建或签名绑定失败返回 false 并就地清理。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountHuntStateDetour()
{
    if (g_hHuntStateDetour != null)
    {
        return true;
    }

    g_hHuntStateDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hHuntStateDetour == null)
    {
        LogError("[BotRouteFix] [Module 4] Failed to create DHook detour instance!");
        return false;
    }

    if (!DHookSetFromConf(g_hHuntStateDetour, g_hGameConf, SDKConf_Signature, "HuntState_OnUpdate"))
    {
        LogError("[BotRouteFix] [Module 4] Failed to set HuntState_OnUpdate signature from gamedata!");
        delete g_hHuntStateDetour;
        g_hHuntStateDetour = null;
        return false;
    }

    DHookAddParam(g_hHuntStateDetour, HookParamType_CBaseEntity);
    return true;
}

//========================================================================================
// MODULE 4 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 4 的两个独立事项：随机覆盖 NOP（ITEM_M4_NOP）/ HuntState dhook writes（ITEM_M4_HUNT）
 *
 * 失败语义：全灭时 LogModuleAllFailed + return false；部分成功按事项各自 FAILED，不连带。
 *
 * @return              true = 至少一个事项武装成功
 */
bool InitModule4_Hunt()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M4_NOP, false);
        Item_SetReady(ITEM_M4_HUNT, false);
        LogError("[BotRouteFix] [Module 4] Gamedata missing; both items FAILED.");
        return false;
    }

    bool nopReady = PrepModule4_NopBackend();
    bool huntReady = PrepModule4_HuntBackend();

    Item_SetReady(ITEM_M4_NOP, nopReady);
    Item_SetReady(ITEM_M4_HUNT, huntReady);

    EnableModule4_Hunt(true);
    if (!Item_IsArmed(ITEM_M4_NOP) && !Item_IsArmed(ITEM_M4_HUNT))
    {
        LogModuleAllFailed(4);
        return false;
    }

    char armedDetail[64];
    Format(armedDetail, sizeof(armedDetail), "NOP=%d, hook=%d",
        Item_IsArmed(ITEM_M4_NOP), Item_IsArmed(ITEM_M4_HUNT));
    LogModuleArmed(4, "HuntState", armedDetail);
    return true;
}

/**
 * 动态启用或停用模块 4（事项 ITEM_M4_NOP / ITEM_M4_HUNT 的调度入口）
 *
 * 两个事项分别武装、分别还原；NOP 失败不影响 HuntState 钩子，反之亦然。
 * 规则关闭时若 Hunt 后端就绪，钩子保持只读观察挂载（不写内存、不计数），
 * 用于可视化原生 Hunt 选区；后端缺失时钩子彻底停用。
 * 失败语义：只做调度，无返回值；两个事项分别按 Ready 武装，Active = OR(Armed)。
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule4_Hunt(bool enable)
{
    if (enable == g_bModule4_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule4_NopBackend();
        RestoreModule4_HuntBackend();
        g_bModule4_Active = false;

        // 观察模式：后端就绪但规则关闭时，仅保持 Post 钩子挂载记录原生选区（Pre 无需启用）
        if (Item_IsReady(ITEM_M4_HUNT) && g_hHuntStateDetour != null)
        {
            SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostHooked, true,
                Hook_HuntState_OnUpdate_Post, true, "Module 4");
        }
        return;
    }

    // ITEM_M4_NOP 与 ITEM_M4_HUNT 独立武装；任一事项武装即视为模块部分激活
    bool nopArmed = ApplyModule4_NopBackend();
    bool huntArmed = ApplyModule4_HuntBackend();
    g_bModule4_Active = (nopArmed || huntArmed);

    if (g_bModule4_Active && (!nopArmed || !huntArmed))
    {
        char partialDetail[64];
        Format(partialDetail, sizeof(partialDetail), "NOP=%d, hunt=%d", nopArmed, huntArmed);
        LogModulePartial(4, partialDetail);
    }
}

/**
 * 还原模块 4（事项 ITEM_M4_NOP / ITEM_M4_HUNT；插件卸载）
 *
 * 彻底退出观察模式并脱钩，确保两个钩子先安全停用后再释放 Detour 句柄。
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule4_Hunt()
{
    EnableModule4_Hunt(false);
    RestoreModule4_HuntBackend();

    if (g_hHuntStateDetour != null)
    {
        delete g_hHuntStateDetour;
        g_hHuntStateDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 4] HuntState hooks and memory patches safely restored.");
}

//========================================================================================
// MODULE 4 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 查询累计认领的 HuntState 区域次数
 *
 * @return              自插件加载以来的累计认领次数
 */
int Module4_GetTotalHuntClaims()
{
    return g_iTotalHuntClaims;
}

/**
 * 获取 Bot 当前认领的搜敌 NavArea 指针
 *
 * 只有最近一次 Post 钩子写入且未超过心跳窗口时才返回有效指针。
 *
 * @param client        客户端索引
 * @return              当前 hunt area；Address_Null = 无有效目标
 */
Address Module4_GetHuntArea(int client)
{
    if (!IsValidClient(client, true))
    {
        return Address_Null;
    }

    // 状态心跳超时判定 (0.3s):
    // 若 Bot 已脱离 HuntState (如包匪转入 MoveTo 运包/下包、或遇敌交火)，立即自动熄灭激光线
    if (GetGameTime() - g_flVisualHuntTimestamp[client] > HUNT_VISUAL_HEARTBEAT_SEC)
    {
        return Address_Null;
    }

    return g_pVisualHuntArea[client];
}

//========================================================================================
// MODULE 4 EVENT HANDLERS
//========================================================================================

/**
 * 回合开始：清空所有 Bot 的搜敌可视化心跳
 */
void Module4_OnRoundStart()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        g_pVisualHuntArea[i] = Address_Null;
        g_flVisualHuntTimestamp[i] = 0.0;
    }
}

//========================================================================================
// MODULE 4 INSTRUCTION VALIDATION PRIMITIVES
//========================================================================================

/**
 * 判断目标地址是否为预期的随机覆盖写回指令
 *
 * 64 位：49 89 56 08 (mov [r14+8], rdx)
 * 32 位：89 4B 04    (mov [ebx+4], ecx)
 *
 * @param addr          待校验地址
 * @return              true = 模板匹配
 */
static bool IsHuntRandomBypassInstruction(Address addr)
{
    if (g_bIsWin64)
    {
        return LoadFromAddress(addr, NumberType_Int8) == 0x49
            && LoadFromAddress(addr + view_as<Address>(1), NumberType_Int8) == 0x89
            && LoadFromAddress(addr + view_as<Address>(2), NumberType_Int8) == 0x56
            && LoadFromAddress(addr + view_as<Address>(3), NumberType_Int8) == 0x08;
    }

    return LoadFromAddress(addr, NumberType_Int8) == 0x89
        && LoadFromAddress(addr + view_as<Address>(1), NumberType_Int8) == 0x4B
        && LoadFromAddress(addr + view_as<Address>(2), NumberType_Int8) == 0x04;
}

//========================================================================================
// MODULE 4 DETOUR & HOOKS (HUNTSTATE::ONUPDATE)
//========================================================================================

/**
 * HuntState::OnUpdate Pre：破除 45 秒敌方老家锁定（事项 ITEM_M4_HUNT 的执行入口）。
 *
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * @param pThis         HuntState 指针（未使用）
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_HuntState_OnUpdate_Pre(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M4_HUNT))
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    // 破除 45 秒直冲老家限制：置 m_hasVisitedEnemySpawn = true
    // 强制 HuntState 从第 0 秒即走 TheNavAreas 全图分散搜索
    Address pBot = GetEntityAddress(client);
    if (pBot != Address_Null && g_iOffset_HasVisitedEnemySpawn > 0)
    {
        StoreToAddress(pBot + view_as<Address>(g_iOffset_HasVisitedEnemySpawn),
            HUNT_EARLY_GAME_BYPASS_VALUE, NumberType_Int8);
    }

    return MRES_Ignored;
}

/**
 * HuntState::OnUpdate Post：记录当前 hunt area（观察）+ 盖时间戳（仅武装时写入）
 *
 * 观察与写入分离：可视化记录是只读观测，事项未武装时也执行，用于 M4 关闭时
 * 查看原生选区；盖章认领与计数是事项写操作。
 * 失败语义：回调内不判定事项成败；条件不符一律 MRES_Ignored，不拦截原生。
 *
 * @param pThis         HuntState 指针
 * @param hParams       DHook 参数
 * @return              MRES_Ignored
 */
public MRESReturn Hook_HuntState_OnUpdate_Post(Address pThis, DHookParam hParams)
{
    if (pThis == Address_Null || g_iOffset_HuntArea <= 0 || g_iOffset_ClearedTimestamp <= 0)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    int team = GetClientTeam(client);
    if (team != 2 && team != 3)
    {
        return MRES_Ignored;
    }

    // 引擎原生下标契约: (GetTeamNumber() - 1) % 2 -> T=1, CT=0
    int teamIdx = (team - 1) % 2;
    // 读取 HuntState 当前所选定的 m_huntArea（初次读取处收紧真实容器白名单门禁）
    Address pArea = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_HuntArea));
    if (!IsValidAddress(pArea) || !Module3_IsKnownNavArea(pArea))
    {
        g_pVisualHuntArea[client] = Address_Null;
        return MRES_Ignored;
    }

    // 观察记录：只写插件自有可视化缓存，不碰引擎内存
    float curtime = GetGameTime();
    g_pVisualHuntArea[client] = pArea;
    g_flVisualHuntTimestamp[client] = curtime;

    // 以下为事项写操作：只有 HuntState 事项武装时才盖章认领
    if (!Item_IsArmed(ITEM_M4_HUNT))
    {
        return MRES_Ignored;
    }

    // 盖章认领该区域: 设置 m_clearedTimestamp[teamIdx] = curtime
    // 刚被认领的区域 age 恒为 0，同队其他 Bot 绝不会重复选取，实现完全无排队的多路分散搜敌
    Address pClearedTimestampAddr = pArea
        + view_as<Address>(g_iOffset_ClearedTimestamp + teamIdx * 4);
    StoreToAddress(pClearedTimestampAddr, view_as<int>(curtime), NumberType_Int32);

    g_iTotalHuntClaims++;
    return MRES_Ignored;
}

//========================================================================================
// MODULE 4 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用 NOP 事项（ITEM_M4_NOP）：只有 Prep 已成功备份时才写入 NOP
 *
 * 失败语义：写入失败同步清 Ready 并回滚，事项 FAILED。
 *
 * @return              true = NOP 已驻留
 */
static bool ApplyModule4_NopBackend()
{
    if (!Item_IsReady(ITEM_M4_NOP) || g_PatchHuntRandomBypass.address == Address_Null)
    {
        Item_SetArmed(ITEM_M4_NOP, false);
        return false;
    }

    if (!g_PatchHuntRandomBypass.isPatched)
    {
        // NOP 穿透该写回指令
        if (!g_PatchHuntRandomBypass.NopOut())
        {
            Item_SetReady(ITEM_M4_NOP, false);
            Item_SetArmed(ITEM_M4_NOP, false);
            LogError("[BotRouteFix] [Module 4] Random bypass NOP write failed; NOP item FAILED.");
            return false;
        }
    }

    Item_SetArmed(ITEM_M4_NOP, true);
    return true;
}

/**
 * 应用 HuntState 事项（ITEM_M4_HUNT）：Pre/Post 两个钩子必须同时成功
 *
 * 失败语义：任一钩子启用失败则回滚已启用的另一个，只清 Armed 保留 Ready，事项待下次 Enable 重试。
 *
 * @return              true = 两个钩子均已启用
 */
static bool ApplyModule4_HuntBackend()
{
    if (!Item_IsReady(ITEM_M4_HUNT) || g_hHuntStateDetour == null)
    {
        Item_SetArmed(ITEM_M4_HUNT, false);
        return false;
    }

    bool preOk = SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreHooked, false,
        Hook_HuntState_OnUpdate_Pre, true, "Module 4");
    bool postOk = SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostHooked, true,
        Hook_HuntState_OnUpdate_Post, true, "Module 4");

    // 全成或全败：任一钩子失败立即回滚，绝不留半挂载态
    if (!preOk || !postOk)
    {
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreHooked, false,
            Hook_HuntState_OnUpdate_Pre, false, "Module 4");
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostHooked, true,
            Hook_HuntState_OnUpdate_Post, false, "Module 4");
        Item_SetArmed(ITEM_M4_HUNT, false);
        LogError("[BotRouteFix] [Module 4] HuntState hook item FAILED (pre=%d, post=%d).", preOk, postOk);
        return false;
    }

    Item_SetArmed(ITEM_M4_HUNT, true);
    return true;
}

/**
 * 还原 NOP 事项（ITEM_M4_NOP）：写回原始字节并解除武装。
 *
 * 失败语义：无（幂等；写回 + Armed(false)）。
 */
static void RestoreModule4_NopBackend()
{
    g_PatchHuntRandomBypass.Restore();
    Item_SetArmed(ITEM_M4_NOP, false);
}

/**
 * 还原 HuntState 事项（ITEM_M4_HUNT）：先停用两个钩子，再清 Armed。
 *
 * 失败语义：无（幂等；摘钩 + Armed(false)）。
 */
static void RestoreModule4_HuntBackend()
{
    SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreHooked, false,
        Hook_HuntState_OnUpdate_Pre, false, "Module 4");
    SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostHooked, true,
        Hook_HuntState_OnUpdate_Post, false, "Module 4");
    Item_SetArmed(ITEM_M4_HUNT, false);
}
