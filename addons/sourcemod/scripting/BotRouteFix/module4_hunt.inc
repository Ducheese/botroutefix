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
//      若仍失配则优雅降级为 flush-only 模式（保持 45s bypass 与盖章认领正常工作，不阻断主插件启动）。
//========================================================================================

// 模块内存补丁与 Detour 句柄
MemoryPatch g_PatchHuntRandomBypass;
Handle g_hHuntStateDetour = null;
int g_iTotalHuntClaims = 0;

// 业务开启态与 per-hook 真实启用态（DHook 状态机唯一事实来源）
static bool g_bModule4_Active = false;
static bool g_bHuntPreEnabled = false;
static bool g_bHuntPostEnabled = false;

// 模块内部专用偏移缓存
static int g_iOffset_HuntArea = -1;
static int g_iOffset_ClearedTimestamp = -1;
static int g_iOffset_HasVisitedEnemySpawn = -1;

// 可视化搜敌追踪缓存 (Bot 当前认领的 m_huntArea 与心跳时间戳)
static Address g_pVisualHuntArea[MAXPLAYERS + 1];
static float g_flVisualHuntTimestamp[MAXPLAYERS + 1];

/**
 * 初始化模块 4 所需的偏移、内存补丁与 DHook Detour
 *
 * @return              是否初始化成功
 */
bool InitModule4_Hunt()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 4] Error: Gamedata is not loaded, cannot initialize hunt module!");
        return false;
    }

    // 1. 读取所需内部偏移
    g_iOffset_HuntArea         = GameConfGetOffset(g_hGameConf, "HuntState_HuntArea_Offset");
    g_iOffset_ClearedTimestamp = GameConfGetOffset(g_hGameConf, "NavArea_ClearedTimestamp_Offset");
    int isStoppingOff          = GameConfGetOffset(g_hGameConf, "CCSBot_IsStopping_Offset");

    if (isStoppingOff != -1)
    {
        g_iOffset_HasVisitedEnemySpawn = isStoppingOff + 1;
    }

    if (g_iOffset_HuntArea == -1 || g_iOffset_ClearedTimestamp == -1 || g_iOffset_HasVisitedEnemySpawn == -1)
    {
        LogError("[BotRouteFix] [Module 4] Failed to read one or more HuntState offsets from gamedata!");
        return false;
    }

    // 2. 挂载 HuntState::OnUpdate Pre/Post Detour（创建句柄，启用态交给 Enable 统一裁决）
    if (!MountHuntStateDetour())
    {
        LogError("[BotRouteFix] [Module 4] Failed to hook HuntState::OnUpdate detour!");
        return false;
    }

    // 3. 套用业务态（随机覆盖 NOP 允许降级，钩子必须成功）
    EnableModule4_Hunt(true);
    if (!g_bModule4_Active)
    {
        delete g_hHuntStateDetour;
        g_hHuntStateDetour = null;
        LogError("[BotRouteFix] [Module 4] Failed to enable HuntState detours!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 4] HuntState overhaul active (45s Rush Bypass + Target Claim + Random NOP) [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 动态启用或停用模块 4（幂等；补丁与 Pre/Post 双钩子同步收敛）
 */
void EnableModule4_Hunt(bool enable)
{
    if (enable == g_bModule4_Active)
    {
        return;
    }

    if (!enable)
    {
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreEnabled, false, Hook_HuntState_OnUpdate_Pre, false, "Module 4");
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostEnabled, true, Hook_HuntState_OnUpdate_Post, false, "Module 4");
        g_PatchHuntRandomBypass.Restore();
        g_bModule4_Active = false;
        return;
    }

    // 随机覆盖 NOP 失败仅降级为 flush-only，不阻断模块启用
    if (!PrepHuntRandomBypassPatch())
    {
        LogMessage("[BotRouteFix] [Module 4] Warning: Random bypass patch skipped (running in flush-only mode).");
    }

    bool bPre  = SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreEnabled, false, Hook_HuntState_OnUpdate_Pre, true, "Module 4");
    bool bPost = SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostEnabled, true, Hook_HuntState_OnUpdate_Post, true, "Module 4");

    if (!bPre || !bPost)
    {
        // 全成或全败：任一钩子失败立即回滚，绝不留半挂载态
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPreEnabled, false, Hook_HuntState_OnUpdate_Pre, false, "Module 4");
        SyncDetourEnabled(g_hHuntStateDetour, g_bHuntPostEnabled, true, Hook_HuntState_OnUpdate_Post, false, "Module 4");
        g_PatchHuntRandomBypass.Restore();
        g_bModule4_Active = false;
        LogError("[BotRouteFix] [Module 4] Enable aborted (pre=%d, post=%d), all resources rolled back!", bPre, bPost);
        return;
    }

    g_bModule4_Active = true;
}

/**
 * 还原模块 4（插件卸载：摘除钩子并释放句柄）
 */
void RestoreModule4_Hunt()
{
    EnableModule4_Hunt(false);

    if (g_hHuntStateDetour != null)
    {
        delete g_hHuntStateDetour;
        g_hHuntStateDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 4] HuntState hooks and memory patches safely restored.");
}

/**
 * 查询模块 4 业务开启态
 */
bool IsModule4_Active()
{
    return g_bModule4_Active;
}

/**
 * 查询 HuntState Detour 钩子是否完全就绪且激活（Pre 与 Post 双钩均处于启用态）
 */
bool IsHuntStateHooked()
{
    return (g_hHuntStateDetour != null && g_bHuntPreEnabled && g_bHuntPostEnabled);
}

/**
 * 查询随机覆盖 NOP 补丁是否生效
 */
bool IsHuntRandomBypassed()
{
    return g_PatchHuntRandomBypass.isPatched;
}

/**
 * 获取累计认领的区域计数
 */
int GetTotalHuntClaims()
{
    return g_iTotalHuntClaims;
}

//========================================================================================
// PATCH: HUNT RANDOM-OVERRIDE BYPASS
//========================================================================================

static bool PrepHuntRandomBypassPatch()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "HuntState_RandomBypass");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 4] Failed to locate signature for HuntState_RandomBypass!");
        return false;
    }

    int patchOffset = GameConfGetOffset(g_hGameConf, "HuntState_RandomBypass_PatchOffset");
    if (patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 4] Failed to read HuntState_RandomBypass_PatchOffset!");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);
    int patchSize = g_bIsWin64 ? 4 : 3;

    g_PatchHuntRandomBypass.Init("HuntRandom_NOP", targetAddr, patchSize);

    // 幂等性检测：检查是否已全部为 NOP (0x90)
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
        // 有备份 = 自家补丁，保持旗标；无备份 = 外来已旁路，记 false（无基线可还原）
        if (!g_PatchHuntRandomBypass.hasBackup)
        {
            g_PatchHuntRandomBypass.isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 4] Random bypass patch already present @ %X, skipping", targetAddr);
        return true;
    }

    // 预检验证：严格比对写回指令操作码 (32位: 89 4B 04, 64位: 49 89 56 08)
    bool isExpected = false;
    if (g_bIsWin64)
    {
        isExpected = (LoadFromAddress(targetAddr, NumberType_Int8) == 0x49
            && LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8) == 0x89
            && LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int8) == 0x56
            && LoadFromAddress(targetAddr + view_as<Address>(3), NumberType_Int8) == 0x08);
    }
    else
    {
        isExpected = (LoadFromAddress(targetAddr, NumberType_Int8) == 0x89
            && LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8) == 0x4B
            && LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int8) == 0x04);
    }

    if (!isExpected)
    {
        // 主偏移模板失配时试备用偏移（v91/v92 同函数差 3 字节：1241/1238），命中即迁址
        int altOffset = GameConfGetOffset(g_hGameConf, "HuntState_RandomBypass_PatchOffset_Alt");
        if (altOffset != -1 && altOffset != patchOffset)
        {
            Address altAddr = sigAddr + view_as<Address>(altOffset);
            bool altExpected = false;
            if (g_bIsWin64)
            {
                altExpected = (LoadFromAddress(altAddr, NumberType_Int8) == 0x49
                    && LoadFromAddress(altAddr + view_as<Address>(1), NumberType_Int8) == 0x89
                    && LoadFromAddress(altAddr + view_as<Address>(2), NumberType_Int8) == 0x56
                    && LoadFromAddress(altAddr + view_as<Address>(3), NumberType_Int8) == 0x08);
            }
            else
            {
                altExpected = (LoadFromAddress(altAddr, NumberType_Int8) == 0x89
                    && LoadFromAddress(altAddr + view_as<Address>(1), NumberType_Int8) == 0x4B
                    && LoadFromAddress(altAddr + view_as<Address>(2), NumberType_Int8) == 0x04);
            }

            if (altExpected)
            {
                LogMessage("[BotRouteFix] [Module 4] Random bypass relocated to alt offset %d @ %X", altOffset, altAddr);
                targetAddr = altAddr;
                g_PatchHuntRandomBypass.Init("HuntRandom_NOP", targetAddr, patchSize);
                isExpected = true;
            }
        }
    }

    if (!isExpected)
    {
        LogMessage("[BotRouteFix] [Module 4] Warning: Random bypass template mismatch @ %X, bypass skipped (flush-only mode)!", targetAddr);
        return false;
    }

    // NOP 穿透该写回指令
    return g_PatchHuntRandomBypass.NopOut();
}

//========================================================================================
// DETOUR: HUNTSTATE::ONUPDATE
//========================================================================================

static bool MountHuntStateDetour()
{
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

public MRESReturn Hook_HuntState_OnUpdate_Pre(Address pThis, DHookParam hParams)
{
    if (!g_bModule4_Active)
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
        StoreToAddress(pBot + view_as<Address>(g_iOffset_HasVisitedEnemySpawn), 1, NumberType_Int8);
    }

    return MRES_Ignored;
}

public MRESReturn Hook_HuntState_OnUpdate_Post(Address pThis, DHookParam hParams)
{
    if (!g_bModule4_Active)
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    int client = hParams.Get(1);
    if (!IsBot(client, true))
    {
        return MRES_Ignored;
    }

    int team = GetClientTeam(client);
    if (team != 2 && team != 3) // 2=T, 3=CT
    {
        return MRES_Ignored;
    }

    // 引擎原生下标契约: (GetTeamNumber() - 1) % 2 -> T=1, CT=0
    int teamIdx = (team - 1) % 2;

    // 读取 HuntState 当前所选定的 m_huntArea
    Address pArea = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_HuntArea));
    if (pArea == Address_Null)
    {
        g_pVisualHuntArea[client] = Address_Null;
        return MRES_Ignored;
    }

    float curtime = GetGameTime();
    g_pVisualHuntArea[client] = pArea;
    g_flVisualHuntTimestamp[client] = curtime;

    // 盖章认领该区域: 设置 m_clearedTimestamp[teamIdx] = curtime
    // 刚被认领的区域 age 恒为 0，同队其他 Bot 绝不会重复选取，实现完全无排队的多路分散搜敌
    Address pClearedTimestampAddr = pArea + view_as<Address>(g_iOffset_ClearedTimestamp + teamIdx * 4);
    StoreToAddress(pClearedTimestampAddr, view_as<int>(curtime), NumberType_Int32);

    g_iTotalHuntClaims++;
    // DebugLog("[BotRouteFix] [Module 4] Bot %N claimed oldest-cleared area @ %X (teamIdx: %d, curtime: %.1f)",
    //     client, pArea, teamIdx, curtime);
    return MRES_Ignored;
}

/**
 * 获取 Bot 当前认领的搜敌 NavArea 指针
 */
Address Module4_GetHuntArea(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
    {
        return Address_Null;
    }

    // 状态心跳超时判定 (0.3s):
    // 若 Bot 已脱离 HuntState (如包匪转入 MoveTo 运包/下包、或遇敌交火)，立即自动熄灭激光线
    if (GetGameTime() - g_flVisualHuntTimestamp[client] > 0.3)
    {
        return Address_Null;
    }

    return g_pVisualHuntArea[client];
}

/**
 * 回合开始时复位所有搜敌目标
 */
void Module4_OnRoundStart()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        g_pVisualHuntArea[i] = Address_Null;
        g_flVisualHuntTimestamp[i] = 0.0;
    }
}
