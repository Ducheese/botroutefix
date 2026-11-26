#if defined _botroutefix_module14_defendrush_included
    #endinput
#endif
#define _botroutefix_module14_defendrush_included

//========================================================================================
// [模块 14] 开局前沿架枪概率重构 defendRushChance (40-10*Morale)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生公式 (cs_bot_idle.cpp:203/444/678/757，四处一字不差):
//      float defendRushChance = -17.0f * (me->GetMorale() - 2);
//      if (me->IsSniper() || RandomFloat(0.0f, 100.0f) < defendRushChance) → MoveToInitialEncounter()
//    士气 +3/+2 时概率 <= 0%（永不架前沿），-3 时 85%。高士气 Bot 开局直接全员前冲，
//    前沿架枪形同虚设；且常数写死，调参只能改源码重编 DLL。
//
// 2. 补丁方案：公式原地改写为 40.0f - 10.0f * Morale（即 -10.0f * (Morale - 4)）。
//      士气 -3(大劣):  70% 架前沿
//      士气  0(均势):  40% 架前沿
//      士气 +1(微优):  30% 架前沿
//      士气 +3(大优):  10% 架前沿（其余 90% 前压冲敌家）
//    狙击手 IsSniper() 短路特权保留（100% 架，不走骰子）；IsSafe() 开局窗口门禁保留。
//
// 3. 二进制形状（v91/v92/Steam64 三版实测同构，find_bytes 各 n=1）：
//    - 32 位 x87：lea eax, [esi-2]（8D 46 FE）→ fild → fmul [-17.0]（D8 0D <abs32>）
//    - 64 位 SSE：mov eax, [rdi+1E50h] → sub eax, 2（83 E8 02）→ movd/cvtdq2ps → mulss [-17.0]
//    四处共用同一份 .rdata 浮点常量（他处无引用已验），B 常数是整数减法 immediate。
//    因此补丁 = 1 处共享 float（-17.0 → -10.0）+ 4 处单字节（-2 → -4）：
//      32 位：lea 末字节 FE → FC；64 位：sub 末字节 02 → 04。
//    float 地址运行时从 Site1 的 fmul/mulss 操作数动态推导（32 位取 abs32，64 位 RIP 解算），
//    并校验确为 -17.0f 后才写，防止常量池漂移误伤。
//    ★ 机器码立即数与常量固化说明: 当前注入的机器码已将公式 40.0 - 10.0 * Morale (即 -10.0*(M-4))
//      固化为底层汇编立即数 4 (sub/lea 4) 与共享只读常量池 float (-10.0f)；下方的 DEFEND_RUSH_CHANCE_*
//      宏定义仅用于 CalculateDefendRushChance 等辅助查询与理论对齐，修改宏不会直接驱动机器码修改。
//
// 事项契约:
//   - 唯一事项 = 前沿架枪公式重写（ITEM_M14_DEFEND）；4 lea + 1 float 是该事项的后端链。
//   - 全成或全败：任一站点失败即回滚全部，绝不允许 -17*(M-4) 之类错公式半态驻留。
//   - 物理写入失败证伪探测结论 → Ready 清空并回滚。
//========================================================================================

//========================================================================================
// MODULE 14 DEFINES & GLOBALS
//========================================================================================

// 开局前沿架枪概率公式宏定义 (Chance = BASE - STEP * Morale，注：当前机器码内已固化立即数 4 与常量 -10.0f)
#define DEFEND_RUSH_CHANCE_BASE 40.0    // 基础前沿架枪概率 (士气 0 时 40%)
#define DEFEND_RUSH_CHANCE_STEP 10.0    // 每级士气增减步长 (百分比)
#define DEFEND_RUSH_CHANCE_MIN  0.0     // 前沿架枪概率下限
#define DEFEND_RUSH_CHANCE_MAX  100.0   // 前沿架枪概率上限

static MemoryPatch g_PatchDefendRush_Lea[4];
static MemoryPatch g_PatchDefendRush_Float;

// Prep 构造、Apply 原样写入的补丁字节
static int g_DefendRushSiteByte[4];
static int g_DefendRushFloatBytes[4];

// 模块规则开启态（Enable 幂等缓存；业务判据统一用 Item_IsArmed）
static bool g_bModule14_Active = false;

//========================================================================================
// MODULE 14 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：前沿架枪公式重写（ITEM_M14_DEFEND）
 *
 * 逐站点定位/校验/备份，再校验共享 float 常量池；不写入代码段（写入由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把事项标记为 FAILED。
 * 失败语义：任一依赖缺失返回 false，Init 将事项标记 FAILED。
 *
 * @return              true = 4 lea + 1 float 全部就绪
 */
static bool PrepModule14_DefendBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 14] Gamedata missing; defend-rush item FAILED.");
        return false;
    }

    for (int i = 0; i < 4; i++)
    {
        if (!PrepModule14_Site(i))
        {
            return false;
        }
    }

    return PrepModule14_FloatBackend();
}

/**
 * 事项 M14 前沿架枪公式重写的单站点后端准备：-2 → -4（1 字节）
 *
 * 失败语义：签名/偏移/操作码/立即数/备份任一失败返回 false，整个事项 FAILED（全成或全败）。
 *
 * @param siteIndex     0..3
 * @return              true = 该站点后端就绪
 */
static bool PrepModule14_Site(int siteIndex)
{
    char sigName[32];
    Format(sigName, sizeof(sigName), "DefendRush14_Site%d", siteIndex + 1);
    Address sigAddr = GameConfGetAddress(g_hGameConf, sigName);
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 14] Failed to locate signature for %s!", sigName);
        return false;
    }

    char offName[32];
    Format(offName, sizeof(offName), "DefendRush14_Site%d_LeaImm", siteIndex + 1);
    int immOffset = GameConfGetOffset(g_hGameConf, offName);
    if (immOffset == -1)
    {
        LogError("[BotRouteFix] [Module 14] Failed to read %s!", offName);
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(immOffset);
    char patchName[32];
    Format(patchName, sizeof(patchName), "DefendRush_Lea%d", siteIndex + 1);

    // Init 必须在幂等检测之前：同目标重入时它保留原始基线备份
    g_PatchDefendRush_Lea[siteIndex].Init(patchName, targetAddr, 1);

    int b0 = LoadFromAddress(targetAddr - view_as<Address>(2), NumberType_Int8);
    int b1 = LoadFromAddress(targetAddr - view_as<Address>(1), NumberType_Int8);
    int b2 = LoadFromAddress(targetAddr, NumberType_Int8);

    int expect0, expect1, origImm, newImm;
    if (g_bIsWin64)
    {
        // 64 位：83 E8 02（sub eax, 2）→ 83 E8 04
        expect0 = 0x83;
        expect1 = 0xE8;
        origImm = 0x02;
        newImm = 0x04;
    }
    else
    {
        // 32 位：8D 46 FE（lea eax, [esi-2]）→ 8D 46 FC
        expect0 = 0x8D;
        expect1 = 0x46;
        origImm = 0xFE;
        newImm = 0xFC;
    }

    // 操作码前缀不受补丁影响，任何状态下都必须匹配
    if (b0 != expect0 || b1 != expect1)
    {
        LogError("[BotRouteFix] [Module 14] Opcode mismatch at site %d (expected %02X %02X, got %02X %02X), patch aborted!",
            siteIndex + 1, expect0, expect1, b0, b1);
        return false;
    }

    if (b2 == newImm)
    {
        // 幂等：已驻留。有备份=自家补丁；无备份=外来改写，无基线可还原
        if (!g_PatchDefendRush_Lea[siteIndex].hasBackup)
        {
            g_PatchDefendRush_Lea[siteIndex].isPatched = false;
            LogError("[BotRouteFix] [Module 14] Site %d already patched without baseline @ %X; defend-rush item FAILED.",
                siteIndex + 1, targetAddr);
            return false;
        }
        LogMessage("[BotRouteFix] [Module 14] Site %d patch already present @ %X, skipping", siteIndex + 1, targetAddr);
    }
    else
    {
        if (b2 != origImm)
        {
            LogError("[BotRouteFix] [Module 14] Immediate mismatch at site %d (expected %02X, got %02X), patch aborted!",
                siteIndex + 1, origImm, b2);
            return false;
        }

        if (!g_PatchDefendRush_Lea[siteIndex].BackupOriginal())
        {
            LogError("[BotRouteFix] [Module 14] Site %d baseline backup failed; defend-rush item FAILED.", siteIndex + 1);
            return false;
        }
    }

    g_DefendRushSiteByte[siteIndex] = newImm;
    LogMessage("[BotRouteFix] [Module 14] Site %d defend-rush block verified @ %X", siteIndex + 1, targetAddr);
    return true;
}

/**
 * 事项 M14 前沿架枪公式重写的共享常量后端准备：-17.0f → -10.0f
 *
 * 地址从 Site1 的乘法操作数动态推导，并校验 Site2~4 共用同一常量池。
 * 失败语义：任一校验失败返回 false，整个事项 FAILED（全成或全败）。
 *
 * @return              true = 共享常量后端就绪
 */
static bool PrepModule14_FloatBackend()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "DefendRush14_Site1");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 14] Failed to locate signature for DefendRush14_Site1!");
        return false;
    }

    int relOffset = GameConfGetOffset(g_hGameConf, "DefendRush14_Site1_FmulRel");
    if (relOffset == -1)
    {
        LogError("[BotRouteFix] [Module 14] Failed to read DefendRush14_Site1_FmulRel!");
        return false;
    }

    Address floatAddr;
    if (g_bIsWin64)
    {
        // 64 位：F3 0F 59 35 <rel32>（mulss），RIP 相对解算
        if (LoadFromAddress(sigAddr + view_as<Address>(relOffset - 4), NumberType_Int8) != 0xF3
            || LoadFromAddress(sigAddr + view_as<Address>(relOffset - 3), NumberType_Int8) != 0x0F
            || LoadFromAddress(sigAddr + view_as<Address>(relOffset - 2), NumberType_Int8) != 0x59
            || LoadFromAddress(sigAddr + view_as<Address>(relOffset - 1), NumberType_Int8) != 0x35)
        {
            LogError("[BotRouteFix] [Module 14] 64-bit mulss opcode mismatch, float patch aborted!");
            return false;
        }
        int rel = LoadFromAddress(sigAddr + view_as<Address>(relOffset), NumberType_Int32);
        floatAddr = sigAddr + view_as<Address>(relOffset + 4 + rel);
    }
    else
    {
        // 32 位：D8 0D <abs32>（fmul），绝对地址直取
        if (LoadFromAddress(sigAddr + view_as<Address>(relOffset - 2), NumberType_Int8) != 0xD8
            || LoadFromAddress(sigAddr + view_as<Address>(relOffset - 1), NumberType_Int8) != 0x0D)
        {
            LogError("[BotRouteFix] [Module 14] 32-bit fmul opcode mismatch, float patch aborted!");
            return false;
        }
        floatAddr = view_as<Address>(LoadFromAddress(sigAddr + view_as<Address>(relOffset), NumberType_Int32));
    }

    // Init 必须在幂等检测之前：同目标重入时它保留原始基线备份
    g_PatchDefendRush_Float.Init("DefendRush_Float", floatAddr, 4);

    // 语义锚点验证：该常量必须是 -17.0f（原始）或 -10.0f（自家已改）
    float fval = view_as<float>(LoadFromAddress(floatAddr, NumberType_Int32));
    if (fval == -10.0)
    {
        if (!g_PatchDefendRush_Float.hasBackup)
        {
            g_PatchDefendRush_Float.isPatched = false;
            LogError("[BotRouteFix] [Module 14] Float already patched without baseline @ %X; defend-rush item FAILED.", floatAddr);
            return false;
        }
        LogMessage("[BotRouteFix] [Module 14] Shared float patch already present @ %X, skipping", floatAddr);
    }
    else
    {
        if (fval != -17.0)
        {
            LogError("[BotRouteFix] [Module 14] Constant mismatch (expected -17.0f, got %f), float patch aborted!", fval);
            return false;
        }

        if (!g_PatchDefendRush_Float.BackupOriginal())
        {
            LogError("[BotRouteFix] [Module 14] Float baseline backup failed; defend-rush item FAILED.");
            return false;
        }
    }

    // 共池断言：Site2~4 的乘法操作数必须指向同一常量，否则说明 DLL 换了常量池布局
    // windows 四站同形，fmul disp 偏移恒 14；win64 各站 mulss rel 偏移 {30, 26, 30, 23}
    //（Site2 用 REX.R 变体 F3 44 0F 59 05，指令短 2 字节；Site4 签名无前向锚，起点近 7 字节）
    for (int s = 1; s < 4; s++)
    {
        char sigN[32];
        Format(sigN, sizeof(sigN), "DefendRush14_Site%d", s + 1);
        Address sigAddrN = GameConfGetAddress(g_hGameConf, sigN);
        if (sigAddrN == Address_Null)
        {
            LogError("[BotRouteFix] [Module 14] Failed to locate signature for %s during float pool check!", sigN);
            return false;
        }

        Address floatAddrN;
        if (g_bIsWin64)
        {
            int fmulRelN[4] = {30, 26, 30, 23};
            int relN = LoadFromAddress(sigAddrN + view_as<Address>(fmulRelN[s]), NumberType_Int32);
            floatAddrN = sigAddrN + view_as<Address>(fmulRelN[s] + 4 + relN);
        }
        else
        {
            floatAddrN = view_as<Address>(LoadFromAddress(sigAddrN + view_as<Address>(14), NumberType_Int32));
        }

        if (floatAddrN != floatAddr)
        {
            LogError("[BotRouteFix] [Module 14] Float pool split at site %d (%X vs %X), float patch aborted!",
                s + 1, floatAddrN, floatAddr);
            return false;
        }
    }

    g_DefendRushFloatBytes[0] = 0x00;
    g_DefendRushFloatBytes[1] = 0x00;
    g_DefendRushFloatBytes[2] = 0x20;  // -10.0f 小端
    g_DefendRushFloatBytes[3] = 0xC1;

    LogMessage("[BotRouteFix] [Module 14] Shared float verified @ %X", floatAddr);
    return true;
}

//========================================================================================
// MODULE 14 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 14 的唯一事项：前沿架枪公式重写（ITEM_M14_DEFEND）
 *
 * 失败语义：任一后端缺失 → LogModuleAllFailed + return false（事项 FAILED，不静默降级）。
 *
 * @return              true = 事项已武装
 */
bool InitModule14_DefendRush()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M14_DEFEND, false);
        LogError("[BotRouteFix] [Module 14] Gamedata missing; defend-rush item FAILED.");
        return false;
    }

    Item_SetReady(ITEM_M14_DEFEND, PrepModule14_DefendBackend());
    EnableModule14_DefendRush(true);
    if (!g_bModule14_Active)
    {
        LogModuleAllFailed(14);
        return false;
    }

    LogModuleArmed(14, "Defend rush", "1");
    return true;
}

/**
 * 动态启用或停用模块 14（事项 ITEM_M14_DEFEND 的调度入口；幂等；4 lea + 1 float 全成或全败）
 *
 * 只做调度：只武装 Ready 的事项；单事项故 Active == Armed。
 * 失败语义：无返回值；Apply 内部失败只清 Armed 并就地 LogError。
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule14_DefendRush(bool enable)
{
    if (enable == g_bModule14_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule14_DefendBackend();
        g_bModule14_Active = false;
        return;
    }

    g_bModule14_Active = ApplyModule14_DefendBackend();
}

/**
 * 还原模块 14（事项 ITEM_M14_DEFEND；插件卸载：恢复原始字节并复位业务态）。
 *
 * 失败语义：无（尽力还原；幂等）。
 */
void RestoreModule14_DefendRush()
{
    EnableModule14_DefendRush(false);
    LogMessage("[BotRouteFix] [Module 14] Defend rush patches safely restored to original bytes.");
}

//========================================================================================
// MODULE 14 PUBLIC GETTERS & FORMULA
//========================================================================================

/**
 * 计算给定士气值对应的理论架前沿概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              架前沿概率 (0.0 ~ 100.0)
 */
float Module14_CalculateDefendRushChance(int morale)
{
    float chance = DEFEND_RUSH_CHANCE_BASE - DEFEND_RUSH_CHANCE_STEP * float(morale);
    if (chance > DEFEND_RUSH_CHANCE_MAX)
    {
        chance = DEFEND_RUSH_CHANCE_MAX;
    }
    else if (chance < DEFEND_RUSH_CHANCE_MIN)
    {
        chance = DEFEND_RUSH_CHANCE_MIN;
    }
    return chance;
}

//========================================================================================
// MODULE 14 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用前沿架枪事项（ITEM_M14_DEFEND）：全成或全败
 *
 * 物理写入失败证伪了 Prep 的模板校验，必须同步清 Ready 并回滚已写站点。
 * 失败语义：写入失败同步清 Ready 并回滚已写站点，事项 FAILED。
 *
 * @return              true = 4 lea + 1 float 全部驻留
 */
static bool ApplyModule14_DefendBackend()
{
    if (!Item_IsReady(ITEM_M14_DEFEND))
    {
        Item_SetArmed(ITEM_M14_DEFEND, false);
        return false;
    }

    for (int i = 0; i < 4; i++)
    {
        int oneByte[1];
        oneByte[0] = g_DefendRushSiteByte[i];

        if (!g_PatchDefendRush_Lea[i].Apply(oneByte))
        {
            RestoreModule14_DefendBackend();
            Item_SetReady(ITEM_M14_DEFEND, false);
            LogError("[BotRouteFix] [Module 14] Site %d write failed; all patches rolled back, defend-rush item FAILED.", i + 1);
            return false;
        }
    }

    if (!g_PatchDefendRush_Float.Apply(g_DefendRushFloatBytes))
    {
        // 已驻留的 lea 必须回滚，否则与旧 float 组成 -17*(M-4) 错公式半态驻留
        RestoreModule14_DefendBackend();
        Item_SetReady(ITEM_M14_DEFEND, false);
        LogError("[BotRouteFix] [Module 14] Shared float write failed; all patches rolled back, defend-rush item FAILED.");
        return false;
    }

    Item_SetArmed(ITEM_M14_DEFEND, true);
    return true;
}

/**
 * 还原前沿架枪事项（ITEM_M14_DEFEND）：写回 4 站点与共享 float 的原始字节并解除武装。
 *
 * 失败语义：无（幂等；写回 + Armed(false)）。
 */
static void RestoreModule14_DefendBackend()
{
    for (int i = 0; i < 4; i++)
    {
        g_PatchDefendRush_Lea[i].Restore();
    }
    g_PatchDefendRush_Float.Restore();
    Item_SetArmed(ITEM_M14_DEFEND, false);
}

