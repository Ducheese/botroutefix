#if defined _botroutefix_module16_escape_included
    #endinput
#endif
#define _botroutefix_module16_escape_included

//========================================================================================
// [模块 16] 逃跑武器纪律（双方）：刀逃 + 见敌切枪
//
// 1. 原生机制 (states/cs_bot_escape_from_bomb.cpp:20-67)：
//      OnEnter : EquipKnife()            // 切刀跑得快（走 SelectItem，不启动 5 秒计时器）
//      OnUpdate: EquipKnife()            // 每拍调用（持刀 no-op，非刀立即切回刀）
//      OnExit  : EquipBestWeapon()       // 退场才换枪
//    入口 (cs_bot_update.cpp:631-648)：已下包 + 自己知精确位 + GetBombTimeLeft() < gonnaBlow
//    (8-2*aggr，持钳 ×0.66) + 非拆包 + 非交火；CT/T 同门（无阵营检查）。
//    原生缺陷（两件套）：
//      (a) 切入帧 5 秒节流：逃跑撞敌无豁免照进 Attack（update.cpp:441-458），Attack()
//          内部置 m_isAttacking=true 并调 AttackState::OnEnter（statemachine.cpp:388-389）。
//          AttackState::OnEnter (attack.cpp:51-52) 虽为逃跑者换枪，但默认 mustEquip=false，
//          受 5 秒 minEquipInterval 节流（weapon.cpp:463-474）；若 Bot 过去 5 秒内曾切过枪，
//          原生换枪直接被节流吞掉变为静默 no-op，手里依然是刀。
//      (b) 交火期逃跑态被旁路：主循环调度器（update.cpp:783-790）在 m_isAttacking 时
//          走 m_attackState.OnUpdate 而非逃跑 OnUpdate；整场交火期间逃跑态 OnUpdate
//          彻底停摆，EquipKnife 零调用。若切入帧未换出枪，则交火内零换枪逻辑，握刀战至死。
//
// 2. 本模块方案（双防线，切入与脱战闭环）：
//    防线一·接触线：AttackState::OnEnter Post 钩（Hook_AttackOnEnter_Escape_Post）：
//      - 绑定 AttackState_OnEnter（THISCALL, Void, ThisPointer_Address + 参一 CBaseEntity）；
//      - 命中 Task==9（Attack() 不改 Task，同帧共存恒为 9）+ 有活敌 + 持刀时介入；
//      - 原生 :52 若未被节流（已持枪），Post 天然 no-op 放行；若持刀（被节流卡死），
//        立即以 mustEquip=true 穿透节流拔枪，彻底解决切入帧拿刀挨打的致命伤；
//      - 真正换出枪时累加接触线遥测 contact（排除全无枪兜底）。
//    防线二·维持线：CCSBot::EquipKnife Pre 钩（Hook_EquipKnife_Pre）：
//      - 负责脱战保枪（StopAttacking 不清 m_enemy，脱战回逃跑 OnUpdate 时压制切刀；
//        遥测记 suppressed）与极少数远端 holding 预摆（持刀且有敌时换枪，真换出记 drawn）；
//      - 敌死或进入 Idle 清 m_enemy 后自愈放行切刀恢复全速逃跑（逃跑移速 250 u/s）。
//
// 3. 二进制签名（find_bytes 各 n=1，见 gamedata）：
//    - CCSBot_EquipKnife：v91 0x102AD9F0 / v92 0x102AFEA0 / 64 位 0x1803525F0
//    - AttackState_OnEnter：v91 0x102B5EB0 / v92 0x102B8270 / 64 位 0x18035CDC0
//      （按 "Crouch and hold attack!" 字符串引用复核过真入口；DHook 实际吃签名，
//      三架构 find_bytes 均为 n=1 且命中同一函数，与 PushPostureContext/掷骰反编译一致）
//    - CCSBot_EquipBestWeapon：v91 0x102AD800 / v92 0x102AFCB0 / 64 位 0x180352380
//
// 4. 递归防御：EquipBestWeapon 兜底会尾调 EquipKnife（weapon.cpp:502），
//    而本模块两钩内均调 EquipBestWeapon——共用 g_bModule16Equipping 重入闸切断；
//    重入期间的 EquipKnife 一律放行原生（此时确实没有更好的武器，选刀就是答案）。
//
// 5. 事项契约：唯一事项 = 逃跑切枪纪律 (ITEM_M16_ESCAPE)。两个 Detour
//    （EquipKnife Pre + AttackState::OnEnter Post）归本模块所有；
//    EquipBestWeapon SDKCall 属基础设施（sdkcalls.inc 统一创建），缺失即事项 FAILED。
//========================================================================================

//========================================================================================
// MODULE 16 DEFINES & GLOBALS
//========================================================================================

// 本模块私有状态（static；句柄与偏移不出本文件）
static bool g_bModule16_Active = false;         // Enable 幂等短路缓存（业务判据一律 Item_IsArmed）
static bool g_bModule16Hooked = false;          // per-hook 真实启用态（维持线）
static Handle g_hEquipKnifeDetour = null;       // 自有 Detour（维持线，Restore 时销毁）
static Handle g_hAttackEnterDetour = null;      // 自有 Detour（接触线，Restore 时销毁）
static bool g_bModule16AttackHooked = false;    // per-hook 真实启用态（接触线）
static bool g_bModule16Equipping = false;       // EquipBestWeapon 重入闸（两钩共用）
static int g_iOffset_Enemy = -1;              // m_enemy 偏移（CCSBot_Enemy_Offset）
static int g_iTotalEscapeArmed = 0;             // 遥测：维持线拔枪次数（离散事件）
static int g_iTotalEscapeContactArmed = 0;      // 遥测：接触线拔枪次数（切入帧破节流）
static int g_iTotalEscapeSuppressed = 0;        // 遥测：维持线持枪压刀次数（脱战保枪帧数）

//========================================================================================
// MODULE 16 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项 M16 逃跑切枪（ITEM_M16_ESCAPE）的唯一后端入口：核验共享依赖并挂载 Detour。
 *
 * 只探测不注入：不启用钩子（启用归 Apply），不写任何游戏内存。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 事项后端就绪
 */
static bool PrepModule16_EscapeBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: gamedata missing.");
        return false;
    }

    if (g_hSDKCall_EquipBestWeapon == null)
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: EquipBestWeapon SDKCall unavailable.");
        return false;
    }

    g_iOffset_Enemy = GameConfGetOffset(g_hGameConf, "CCSBot_Enemy_Offset");
    if (g_iOffset_Enemy <= 0 || g_iOffset_Task <= 0)
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: enemy/task offset missing (enemy=%d, task=%d).",
            g_iOffset_Enemy, g_iOffset_Task);
        return false;
    }

    if (!MountEquipKnifeDetour())
    {
        return false;
    }

    if (!MountAttackEnterDetour())
    {
        return false;
    }

    LogMessage("[BotRouteFix] [Module 16] Escape offsets (task=%d, enemy=%d) and detours (EquipKnife + AttackState::OnEnter) verified.",
        g_iOffset_Task, g_iOffset_Enemy);
    return true;
}

/**
 * 挂载 CCSBot::EquipKnife Detour（事项 ITEM_M16_ESCAPE 维持线；创建句柄并绑定签名）
 *
 * 只探测不使能：句柄就绪供 ApplyModule16_EscapeBackend 挂载。
 * 失败语义：句柄创建失败或签名缺失返回 false 并就地清理。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountEquipKnifeDetour()
{
    if (g_hEquipKnifeDetour != null)
    {
        return true;
    }

    g_hEquipKnifeDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hEquipKnifeDetour == null)
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: EquipKnife detour creation failed.");
        return false;
    }

    if (!DHookSetFromConf(g_hEquipKnifeDetour, g_hGameConf, SDKConf_Signature, "CCSBot_EquipKnife"))
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: EquipKnife signature missing.");
        delete g_hEquipKnifeDetour;
        g_hEquipKnifeDetour = null;
        return false;
    }

    return true;
}

/**
 * 挂载 AttackState::OnEnter Post Detour（事项 ITEM_M16_ESCAPE 接触线；创建句柄并绑定签名）
 *
 * 只探测不使能：句柄就绪供 ApplyModule16_EscapeBackend 挂载。
 * 失败语义：句柄创建失败或签名缺失返回 false 并就地清理。
 *
 * @return              true = Detour 句柄已创建并绑定签名
 */
static bool MountAttackEnterDetour()
{
    if (g_hAttackEnterDetour != null)
    {
        return true;
    }

    g_hAttackEnterDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
    if (g_hAttackEnterDetour == null)
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: AttackState::OnEnter detour creation failed.");
        return false;
    }

    if (!DHookSetFromConf(g_hAttackEnterDetour, g_hGameConf, SDKConf_Signature, "AttackState_OnEnter"))
    {
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: AttackState_OnEnter signature missing.");
        delete g_hAttackEnterDetour;
        g_hAttackEnterDetour = null;
        return false;
    }

    // OnEnter(CCSBot *me) 带一个实体参数；参一为 CCSBot 实体
    DHookAddParam(g_hAttackEnterDetour, HookParamType_CBaseEntity);

    return true;
}

//========================================================================================
// MODULE 16 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 16 唯一事项（ITEM_M16_ESCAPE）。
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 *
 * @return              true = 事项已武装
 */
bool InitModule16_Escape()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M16_ESCAPE, false);
        LogError("[BotRouteFix] [Module 16] Escape item FAILED: gamedata missing.");
        return false;
    }

    Item_SetReady(ITEM_M16_ESCAPE, PrepModule16_EscapeBackend());
    EnableModule16_Escape(true);
    if (!g_bModule16_Active)
    {
        LogModuleAllFailed(16);
        return false;
    }

    LogModuleArmed(16, "Escape", "1");
    return true;
}

/**
 * 启用/停用模块 16（事项 ITEM_M16_ESCAPE 的调度入口）。
 *
 * 只做调度：重探不在此处，武装结果由 Apply 写入 Armed；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 *
 * @param enable        true = 武装；false = 还原并解除武装
 */
void EnableModule16_Escape(bool enable)
{
    if (enable == g_bModule16_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule16_EscapeBackend();
        g_bModule16_Active = false;
        return;
    }

    g_bModule16_Active = ApplyModule16_EscapeBackend();
}

/**
 * 还原模块 16（事项 ITEM_M16_ESCAPE；插件卸载/规则关闭）：摘钩、解除武装、销毁自有 Detour 句柄。
 *
 * 失败语义：无（尽力还原；句柄销毁后置 null，下次启用以 Prep 重建）。
 */
void RestoreModule16_Escape()
{
    EnableModule16_Escape(false);

    if (g_hEquipKnifeDetour != null)
    {
        // 卸载路径无条件摘钩，不依赖 per-hook 缓存
        SyncDetourEnabled(g_hEquipKnifeDetour, g_bModule16Hooked,
            false, Hook_EquipKnife_Pre, false, "Module 16");
        delete g_hEquipKnifeDetour;
        g_hEquipKnifeDetour = null;
    }

    if (g_hAttackEnterDetour != null)
    {
        // 卸载路径无条件摘钩，不依赖 per-hook 缓存
        SyncDetourEnabled(g_hAttackEnterDetour, g_bModule16AttackHooked,
            true, Hook_AttackOnEnter_Escape_Post, false, "Module 16");
        delete g_hAttackEnterDetour;
        g_hAttackEnterDetour = null;
    }

    LogMessage("[BotRouteFix] [Module 16] Escape hooks safely restored.");
}

//========================================================================================
// MODULE 16 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M16 的遥测：维持线累计实际拔枪次数。
 *
 * 只读；只在"持刀 → 敌在 → 换枪成功"的离散瞬间累加，不参与事项状态。
 *
 * @return              累计维持线拔枪次数
 */
int Module16_GetTotalEscapeArmed()
{
    return g_iTotalEscapeArmed;
}

/**
 * 事项 M16 的遥测：接触线切入帧拔枪累计次数。
 *
 * 只读；只在"overlay 入口持刀 + 敌在 + 破节流换枪成功"的离散瞬间累加。
 *
 * @return              累计接触线拔枪次数
 */
int Module16_GetTotalEscapeContactArmed()
{
    return g_iTotalEscapeContactArmed;
}

/**
 * 事项 M16 的遥测：维持线持枪压刀累计次数。
 *
 * 只读；维持线静默分支（已持枪，只 Supercede 脱战保枪）每拍累加，不打逐行日志。
 *
 * @return              累计持枪压刀帧数
 */
int Module16_GetTotalEscapeSuppressed()
{
    return g_iTotalEscapeSuppressed;
}

//========================================================================================
// MODULE 16 INTERNAL PRIMITIVES
//========================================================================================

/**
 * 事项 M16 内部原语：是否正手持近战武器/小刀。
 *
 * 优先以 Slot 2（WEAPON_SLOT_KNIFE）实体比对兜底非标近战，再以类名匹配标准 weapon_knife。
 * 失败语义：武器实体无效/非刀时返回 false（调用方按"已持枪"处理，只会压刀不换枪）。
 *
 * @param client        客户端索引
 * @return              true = 手持小刀/近战武器
 */
static bool Escape_IsUsingKnife(int client)
{
    int weapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (weapon <= 0 || !IsValidEntity(weapon))
    {
        return false;
    }

    // 槽位 2 为近战武器槽：实体一致即认定为持刀（兼顾改类名的非标近战实体）
    int knife = GetPlayerWeaponSlot(client, 2);
    if (knife > 0 && weapon == knife)
    {
        return true;
    }

    char classname[32];
    GetEntityClassname(weapon, classname, sizeof(classname));
    return StrEqual(classname, "weapon_knife");
}

/**
 * 事项 M16 内部原语：是否持有存活敌方目标（m_enemy CHandle 解码 + 验活 + 验队）。
 *
 * CHandle 恒 4 字节；两架构下槽位都落在低 12 位内（32 位 & 0xFFF / 64 位低 16 位），
 * 沿用模块 11 的既有解码约定。槽位会被复用，故必须验队防误指队友。
 * 失败语义：偏移缺失/句柄空/目标已死/同队时返回 false（调用方视为无敌，放行原生刀逃）。
 *
 * @param client        客户端索引
 * @param pBot          CCSBot 实体地址（免二次解析）
 * @return              true = 有存活敌人
 */
static bool Escape_HasLiveEnemy(int client, Address pBot)
{
    if (!IsValidAddress(pBot) || g_iOffset_Enemy <= 0)
    {
        return false;
    }

    int handle = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Enemy), NumberType_Int32);
    if (handle == -1 || handle == 0)
    {
        return false;
    }

    int enemy = handle & 0xFFF;
    if (!IsValidClient(enemy, true) || enemy == client)
    {
        return false;
    }

    return GetClientTeam(enemy) != GetClientTeam(client);
}

//========================================================================================
// MODULE 16 DETOUR & HOOK (CCSBOT::EQUIPKNIFE::PRE - 防线二·维持线)
//========================================================================================

/**
 * 事项 M16 的维持线入口钩子（CCSBot::EquipKnife Pre）。
 *
 * 负责脱战保枪（StopAttacking 不清 m_enemy，脱战回逃跑 OnUpdate 时压制切刀）与 holding 预摆；
 * 持刀则换枪并挡住本拍切刀，已持枪则只挡切刀；
 * 其余（未武装/重入/非逃跑/无敌人/无枪）一律 MRES_Ignored 直通原生。
 * 失败语义：回调内不判定事项成败；SDKCall 失败时刀保持不动，下一拍重试。
 *
 * @param pThis         CCSBot 实体地址（EquipKnife 无参数）
 * @return              MRES_Supercede = 拦截原生切刀；MRES_Ignored = 放行
 */
public MRESReturn Hook_EquipKnife_Pre(Address pThis)
{
    if (!Item_IsArmed(ITEM_M16_ESCAPE))
    {
        return MRES_Ignored;
    }

    if (pThis == Address_Null)
    {
        return MRES_Ignored;
    }

    // 重入闸：EquipBestWeapon 兜底尾调 EquipKnife 时放行（此时选刀就是正确结论）
    if (g_bModule16Equipping)
    {
        return MRES_Ignored;
    }

    int client = ResolveBotClient(pThis);
    if (client == 0)
    {
        return MRES_Ignored;
    }

    // 只介入逃跑态（EscapeFromBomb 先 SetTask(9) 后 SetState）
    if (GetEntData(client, g_iOffset_Task, 4) != BOT_TASK_ESCAPE_FROM_BOMB)
    {
        return MRES_Ignored;
    }

    // 只有确认敌人存在，才违反原生"刀逃"纪律
    if (!Escape_HasLiveEnemy(client, pThis))
    {
        return MRES_Ignored;
    }

    // 已持枪：只压住刀，不重复换枪（脱战保枪）。遥测记 suppressed，不打逐行日志
    if (!Escape_IsUsingKnife(client))
    {
        g_iTotalEscapeSuppressed++;
        return MRES_Supercede;
    }

    // 持刀 + 有敌：立即拔枪（holding 预摆），并挡住本拍切刀
    g_bModule16Equipping = true;
    BotEquipBestWeapon(client, true);
    g_bModule16Equipping = false;

    // 只有真正换出非刀武器才计入维持线拔枪遥测（排除无枪兜底）
    if (!Escape_IsUsingKnife(client))
    {
        g_iTotalEscapeArmed++;
        DebugLog("[BotRouteFix] [Module 16] Bot %N escape knife suppressed, weapon equipped (enemy present)", client);
    }

    return MRES_Supercede;
}

//========================================================================================
// MODULE 16 DETOUR & HOOK (ATTACKSTATE::ONENTER::POST - 防线一·接触线)
//========================================================================================

/**
 * 事项 M16 的接触线入口钩子（AttackState::OnEnter Post）。
 *
 * 补原生 attack.cpp:51-52 的切入帧节流缺口：overlay 入口那一拍，原生 EquipBestWeapon
 * 可能被 5 秒节流静默吞掉，此后每拍走 m_attackState.OnUpdate（update.cpp:783-790），
 * 逃跑 OnUpdate 停摆、EquipKnife 零调用——维持线钩够不着，必须在这里出手。
 * Post 时机天然幂等：原生彩票中了（已持枪）就是 no-op，持刀才 mustEquip=true 兜底。
 * 只在"逃跑态 + 持刀 + 有存活敌人"时接管；其余一律 MRES_Ignored。
 * 失败语义：回调内不判定事项成败；与维持线共用 g_bModule16Equipping 重入闸。
 *
 * @param pThis         AttackState 指针（非 bot；bot 取自 hParams 参一）
 * @param hParams       DHook 参数（参一 = CCSBot，沿用 M5 的取参约定）
 * @return              MRES_Ignored（Post 钩无可拦截之事）
 */
public MRESReturn Hook_AttackOnEnter_Escape_Post(Address pThis, DHookParam hParams)
{
    if (!Item_IsArmed(ITEM_M16_ESCAPE))
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

    // 只介入逃跑态（Attack 不改 Task，Task 恒为 9）
    if (GetEntData(client, g_iOffset_Task, 4) != BOT_TASK_ESCAPE_FROM_BOMB)
    {
        return MRES_Ignored;
    }

    Address pBot = GetEntityAddress(client);

    // 只有确认敌人存在，才违反原生"刀逃"纪律
    if (!Escape_HasLiveEnemy(client, pBot))
    {
        return MRES_Ignored;
    }

    // 原生彩票已中（已持枪）：无需介入（天然幂等）
    if (!Escape_IsUsingKnife(client))
    {
        return MRES_Ignored;
    }

    // 接触帧持刀 + 有敌：原生 :52 碰上 5 秒节流未切出枪，立即以 mustEquip=true 穿透节流拔枪
    g_bModule16Equipping = true;
    BotEquipBestWeapon(client, true);
    g_bModule16Equipping = false;

    // 只有真换出了枪（非刀）才记接触线拔枪数（排除无枪兜底）
    if (!Escape_IsUsingKnife(client))
    {
        g_iTotalEscapeContactArmed++;
        DebugLog("[BotRouteFix] [Module 16] Bot %N escape contact gun equipped (attack entry)", client);
    }

    return MRES_Ignored;
}

//========================================================================================
// MODULE 16 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用事项 M16（ITEM_M16_ESCAPE）：启用维持线 Pre 钩子 + 接触线 Post 钩子。
 *
 * 只注入不探测（定位/签名绑定已在 Prep 完成）。
 * 失败语义：任一钩子启用失败属动态环境抖动，只清 Armed 保留 Ready
 * （待下次 Enable 重试），不证伪 Prep 的探测结论；已启用的那个一并摘掉，
 * 不留半武装。
 *
 * @return              true = 事项已武装
 */
static bool ApplyModule16_EscapeBackend()
{
    if (!Item_IsReady(ITEM_M16_ESCAPE) || g_hEquipKnifeDetour == null
        || g_hAttackEnterDetour == null)
    {
        Item_SetArmed(ITEM_M16_ESCAPE, false);
        return false;
    }

    bool knifeOk = SyncDetourEnabled(g_hEquipKnifeDetour, g_bModule16Hooked,
        false, Hook_EquipKnife_Pre, true, "Module 16");
    bool contactOk = SyncDetourEnabled(g_hAttackEnterDetour, g_bModule16AttackHooked,
        true, Hook_AttackOnEnter_Escape_Post, true, "Module 16");

    if (!knifeOk || !contactOk)
    {
        SyncDetourEnabled(g_hEquipKnifeDetour, g_bModule16Hooked,
            false, Hook_EquipKnife_Pre, false, "Module 16");
        SyncDetourEnabled(g_hAttackEnterDetour, g_bModule16AttackHooked,
            true, Hook_AttackOnEnter_Escape_Post, false, "Module 16");
        Item_SetArmed(ITEM_M16_ESCAPE, false);
        LogError("[BotRouteFix] [Module 16] Escape item unarmed: detour enable failed (knife=%d, contact=%d).",
            knifeOk, contactOk);
        return false;
    }

    Item_SetArmed(ITEM_M16_ESCAPE, true);
    return true;
}

/**
 * 还原事项 M16：摘钩并解除武装。
 *
 * 句柄销毁归 RestoreModule16_Escape（生命周期层），此处只处理 armed 与钩子开关。
 * 失败语义：无（幂等；句柄为 null 时 Sync 内部 no-op）。
 */
static void RestoreModule16_EscapeBackend()
{
    SyncDetourEnabled(g_hEquipKnifeDetour, g_bModule16Hooked,
        false, Hook_EquipKnife_Pre, false, "Module 16");
    SyncDetourEnabled(g_hAttackEnterDetour, g_bModule16AttackHooked,
        true, Hook_AttackOnEnter_Escape_Post, false, "Module 16");
    Item_SetArmed(ITEM_M16_ESCAPE, false);
}
