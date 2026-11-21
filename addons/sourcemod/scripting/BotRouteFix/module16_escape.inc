#if defined _botroutefix_module16_escape_included
    #endinput
#endif
#define _botroutefix_module16_escape_included

//========================================================================================
// [模块 16] 逃跑武器纪律（双方）：刀逃 + 见敌切枪
//
// 1. 原生机制 (states/cs_bot_escape_from_bomb.cpp:20-58)：
//      OnEnter : EquipKnife()            // 切刀跑得快
//      OnUpdate: EquipKnife()            // 每拍调用（持刀 no-op，非刀立即切回刀）
//      OnExit  : EquipBestWeapon()       // 退场才换枪
//    入口 (cs_bot_update.cpp:627-648)：已下包 + 自己知精确位 + GetBombTimeLeft() < gonnaBlow
//    (8-2*aggr，持钳 ×0.66) + 非拆包 + 非交火；CT/T 同门（无阵营检查）。
//    原生缺陷：逃跑态 OnUpdate 无敌人检查，每拍强制持刀；"撞敌换枪"只写在
//    AttackState::OnEnter (attack.cpp:51)，而逃跑态从不进 Attack（无攻击投递口），
//    所以切刀之后整个逃跑过程再也不碰主武器——被追上就是拿刀挨打。
//
// 2. 本模块方案：不改 OnUpdate（与原生每拍切刀打架会来回抽搐），
//    改为在全局 CCSBot::EquipKnife 的 Pre 钩上裁决：
//      - 只介入 逃跑态（task == ESCAPE_FROM_BOMB。EscapeFromBomb() 先 SetTask 后
//        SetState，statemachine.cpp:67-71）+ 有存活敌人（m_enemy CHandle 解码验活验队）
//      - 此刻持刀：SDKCall CCSBot::EquipBestWeapon(mustEquip=true) 立即换枪（绕过节流），
//        并 MRES_Supercede 挡掉本拍的原生切刀
//      - 此刻已持枪：只 Supercede（把刀继续压住），不再重复换枪
//      - 敌人消失/死亡：不再拦截，原生下一拍自动切回刀恢复逃速（自愈，无状态残留）
//    m_enemy 有半拍视觉滞后（视觉实锤 §二），即"确认威胁后拔枪"，语义正确。
//
// 3. 二进制签名（find_bytes 各 n=1，见 gamedata）：
//    - CCSBot_EquipKnife：v91 0x102AD9F0 / v92 0x102AFEA0（一套 32 位签名通吃）/
//      64 位 0x1803525F0（cmp eax, 1Ch = WEAPON_KNIFE 28 + SelectItem 尾跳）。
//    - CCSBot_EquipBestWeapon：v91 0x102AD800 / v92 0x102AFCB0 / 64 位 0x180352380
//      （mustEquip 短路 minEquipInterval 节流 + 主武器类型白名单 + 尾调 EquipKnife 兜底）。
//
// 4. 递归防御：EquipBestWeapon 兜底会尾调 EquipKnife（weapon.cpp:502），
//    而本模块又在钩里调 EquipBestWeapon——用 g_bModule16Equipping 重入闸切断；
//    重入期间的 EquipKnife 一律放行原生（此时确实没有更好的武器，选刀就是答案）。
//
// 5. 事项契约：唯一事项 = 逃跑切枪纪律 (ITEM_M16_ESCAPE)。Detour 归本模块所有；
//    EquipBestWeapon SDKCall 属基础设施（sdkcalls.inc 统一创建），缺失即事项 FAILED。
//========================================================================================

//========================================================================================
// MODULE 16 DEFINES & GLOBALS
//========================================================================================

// 本模块私有状态（static；句柄与偏移不出本文件）
static bool g_bModule16_Active = false;         // Enable 幂等短路缓存（业务判据一律 Item_IsArmed）
static bool g_bModule16Hooked = false;          // per-hook 真实启用态
static Handle g_hEquipKnifeDetour = null;       // 自有 Detour（Restore 时销毁）
static bool g_bModule16Equipping = false;       // EquipBestWeapon 重入闸
static int g_iOffset_Enemy = -1;              // m_enemy 偏移（CCSBot_Enemy_Offset）
static int g_iTotalEscapeArmed = 0;             // 遥测：实际拔枪次数（离散事件）

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

    LogMessage("[BotRouteFix] [Module 16] Escape offsets (task=%d, enemy=%d) and EquipKnife detour verified.",
        g_iOffset_Task, g_iOffset_Enemy);
    return true;
}

/**
 * 挂载 CCSBot::EquipKnife Detour（事项 ITEM_M16_ESCAPE；创建句柄并绑定签名）
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
    if (!Item_IsArmed(ITEM_M16_ESCAPE))
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

    LogMessage("[BotRouteFix] [Module 16] Escape hooks safely restored.");
}

//========================================================================================
// MODULE 16 PUBLIC GETTERS & TELEMETRY
//========================================================================================

/**
 * 事项 M16 的遥测：累计实际拔枪次数。
 *
 * 只读；只在"持刀 → 敌在 → 换枪成功"的离散瞬间累加，不参与事项状态。
 *
 * @return              累计拔枪次数
 */
int Module16_GetTotalEscapeArmed()
{
    return g_iTotalEscapeArmed;
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
// MODULE 16 DETOUR & HOOK (CCSBOT::EQUIPKNIFE::PRE)
//========================================================================================

/**
 * 事项 M16 的唯一入口钩子（CCSBot::EquipKnife Pre）。
 *
 * 只在"逃跑态 + 有存活敌人"时接管：持刀则换枪并挡住本拍切刀，已持枪则只挡切刀；
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

    // 已持枪：只压住刀，不重复换枪（原生命令是"切回刀"，这里拒之）
    if (!Escape_IsUsingKnife(client))
    {
        return MRES_Supercede;
    }

    // 无枪可拔（主武器/手枪槽都空）：刀就是最优解，放行原生，不做无意义调用
    if (GetPlayerWeaponSlot(client, 0) <= 0 && GetPlayerWeaponSlot(client, 1) <= 0)
    {
        return MRES_Ignored;
    }

    // 持刀 + 有敌 + 有枪：立即拔枪（绕过节流），并挡住本拍切刀
    g_bModule16Equipping = true;
    BotEquipBestWeapon(client, true);
    g_bModule16Equipping = false;

    // 只有真正换成枪才记一次（兜底失败不计遥测、不打日志，避免每拍刷屏）
    if (!Escape_IsUsingKnife(client))
    {
        g_iTotalEscapeArmed++;
        DebugLog("[BotRouteFix] [Module 16] Bot %N escape knife suppressed, weapon equipped (enemy present)", client);
    }

    return MRES_Supercede;
}

//========================================================================================
// MODULE 16 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用事项 M16（ITEM_M16_ESCAPE）：启用自己的 Pre 钩子。
 *
 * 只注入不探测（定位/签名绑定已在 Prep 完成）。
 * 失败语义：钩子启用失败属动态环境抖动，只清 Armed 保留 Ready（待下次 Enable 重试），
 * 不证伪 Prep 的探测结论。
 *
 * @return              true = 事项已武装
 */
static bool ApplyModule16_EscapeBackend()
{
    if (!Item_IsReady(ITEM_M16_ESCAPE) || g_hEquipKnifeDetour == null)
    {
        Item_SetArmed(ITEM_M16_ESCAPE, false);
        return false;
    }

    if (!SyncDetourEnabled(g_hEquipKnifeDetour, g_bModule16Hooked,
        false, Hook_EquipKnife_Pre, true, "Module 16"))
    {
        Item_SetArmed(ITEM_M16_ESCAPE, false);
        LogError("[BotRouteFix] [Module 16] Escape item unarmed: EquipKnife detour enable failed.");
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
    Item_SetArmed(ITEM_M16_ESCAPE, false);
}
