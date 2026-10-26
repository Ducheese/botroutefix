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
//========================================================================================
// 开局前沿架枪概率公式宏定义 (Chance = BASE - STEP * Morale，注：当前机器码内已固化立即数 4 与常量 -10.0f)
#define DEFEND_RUSH_CHANCE_BASE 40.0    // 基础前沿架枪概率 (士气 0 时 40%)
#define DEFEND_RUSH_CHANCE_STEP 10.0    // 每级士气增减步长 (百分比)
#define DEFEND_RUSH_CHANCE_MIN  0.0     // 前沿架枪概率下限
#define DEFEND_RUSH_CHANCE_MAX  100.0   // 前沿架枪概率上限

MemoryPatch g_PatchDefendRush14_Lea[4];
MemoryPatch g_PatchDefendRush14_Float;

/**
 * 初始化并激活模块 14 所有内存补丁（4 lea + 1 float，全成或全败）
 *
 * @return              是否全部成功激活
 */
bool InitModule14_DefendRush()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 14] Error: Gamedata is not loaded, cannot initialize defendRush patches!");
        return false;
    }

    for (int i = 0; i < 4; i++)
    {
        if (!PrepDefendRush14_Site(i))
        {
            LogError("[BotRouteFix] [Module 14] Failed to inject defendRush lea patch at site %d!", i + 1);
            // 已驻留的 lea 必须回滚，否则与旧 float 组成 -17*(M-4) 错公式半态驻留
            for (int j = 0; j < 4; j++)
            {
                g_PatchDefendRush14_Lea[j].Restore();
            }
            return false;
        }
    }

    if (!PrepDefendRush14_Float())
    {
        LogError("[BotRouteFix] [Module 14] Failed to inject defendRush shared float patch!");
        for (int j = 0; j < 4; j++)
        {
            g_PatchDefendRush14_Lea[j].Restore();
        }
        return false;
    }

    LogMessage("[BotRouteFix] [Module 14] defendRushChance (40-10*M) patches activated [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 还原模块 14 的所有内存补丁
 */
void RestoreModule14_DefendRush()
{
    for (int i = 0; i < 4; i++)
    {
        g_PatchDefendRush14_Lea[i].Restore();
    }
    RestoreDefendRush14_Float();
    LogMessage("[BotRouteFix] [Module 14] Patches safely restored to original bytes.");
}

/**
 * 动态启用或停用模块 14 补丁
 */
void EnableModule14_DefendRush(bool enable)
{
    if (enable)
    {
        if (!IsDefendRush14Patched())
        {
            bool ok = true;
            for (int i = 0; i < 4; i++)
            {
                if (!PrepDefendRush14_Site(i))
                {
                    ok = false;
                    break;
                }
            }
            if (ok && !PrepDefendRush14_Float())
            {
                ok = false;
            }
            if (!ok)
            {
                RestoreModule14_DefendRush();
            }
        }
    }
    else
    {
        for (int i = 0; i < 4; i++)
        {
            g_PatchDefendRush14_Lea[i].Restore();
        }
        g_PatchDefendRush14_Float.Restore();
    }
}

/**
 * 查询模块 14 补丁是否完整生效（4 lea 全挂 + float 已改）
 */
bool IsDefendRush14Patched()
{
    if (!g_PatchDefendRush14_Float.isPatched)
    {
        return false;
    }
    for (int i = 0; i < 4; i++)
    {
        if (!g_PatchDefendRush14_Lea[i].isPatched)
        {
            return false;
        }
    }
    return true;
}

/**
 * 计算给定士气值对应的理论架前沿概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              架前沿概率 (0.0 ~ 100.0)
 */
float CalculateDefendRushChance(int morale)
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
// 单站点 lea/sub 补丁：-2 → -4（1 字节）
//========================================================================================

bool PrepDefendRush14_Site(int siteIndex)
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
    Format(patchName, sizeof(patchName), "DefendRush14_Lea%d", siteIndex + 1);
    g_PatchDefendRush14_Lea[siteIndex].Init(patchName, targetAddr, 1);

    int b0 = LoadFromAddress(targetAddr - view_as<Address>(2), NumberType_Int8);
    int b1 = LoadFromAddress(targetAddr - view_as<Address>(1), NumberType_Int8);
    int b2 = LoadFromAddress(targetAddr, NumberType_Int8);

    if (g_bIsWin64)
    {
        // 64 位：83 E8 02（sub eax, 2）→ 83 E8 04
        if (b0 != 0x83 || b1 != 0xE8)
        {
            LogError("[BotRouteFix] [Module 14] 64-bit opcode mismatch at site %d (expected 83 E8, got %02X %02X), patch aborted!",
                siteIndex + 1, b0, b1);
            return false;
        }
        if (b2 == 0x04)
        {
            if (!g_PatchDefendRush14_Lea[siteIndex].hasBackup)
            {
                g_PatchDefendRush14_Lea[siteIndex].isPatched = false;
            }
            LogMessage("[BotRouteFix] [Module 14] Site %d patch already present @ %X, skipping", siteIndex + 1, targetAddr);
            return true;
        }
        if (b2 != 0x02)
        {
            LogError("[BotRouteFix] [Module 14] 64-bit imm mismatch at site %d (expected 02, got %02X), patch aborted!",
                siteIndex + 1, b2);
            return false;
        }
        int oneByte[1] = {0x04};
        return g_PatchDefendRush14_Lea[siteIndex].Apply(oneByte);
    }
    else
    {
        // 32 位：8D 46 FE（lea eax, [esi-2]）→ 8D 46 FC
        if (b0 != 0x8D || b1 != 0x46)
        {
            LogError("[BotRouteFix] [Module 14] 32-bit opcode mismatch at site %d (expected 8D 46, got %02X %02X), patch aborted!",
                siteIndex + 1, b0, b1);
            return false;
        }
        if (b2 == 0xFC)
        {
            if (!g_PatchDefendRush14_Lea[siteIndex].hasBackup)
            {
                g_PatchDefendRush14_Lea[siteIndex].isPatched = false;
            }
            LogMessage("[BotRouteFix] [Module 14] Site %d patch already present @ %X, skipping", siteIndex + 1, targetAddr);
            return true;
        }
        if (b2 != 0xFE)
        {
            LogError("[BotRouteFix] [Module 14] 32-bit imm mismatch at site %d (expected FE, got %02X), patch aborted!",
                siteIndex + 1, b2);
            return false;
        }
        int oneByte[1] = {0xFC};
        return g_PatchDefendRush14_Lea[siteIndex].Apply(oneByte);
    }
}

//========================================================================================
// 共享 float 补丁：-17.0f → -10.0f（地址从 Site1 的乘法操作数动态推导）
//========================================================================================

bool PrepDefendRush14_Float()
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

    // 语义锚点验证：该常量必须确为 -17.0f（四处共用，他处无引用已在逆向时验明）
    float fval = view_as<float>(LoadFromAddress(floatAddr, NumberType_Int32));
    if (fval == -10.0)
    {
        if (!g_PatchDefendRush14_Float.hasBackup)
        {
            g_PatchDefendRush14_Float.isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 14] Shared float patch already present @ %X, skipping", floatAddr);
        return true;
    }
    if (fval != -17.0)
    {
        LogError("[BotRouteFix] [Module 14] Constant mismatch (expected -17.0f, got %f), float patch aborted!", fval);
        return false;
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

    g_PatchDefendRush14_Float.Init("DefendRush14_Float", floatAddr, 4);
    int floatBytes[4] = {0x00, 0x00, 0x20, 0xC1}; // -10.0f 小端
    return g_PatchDefendRush14_Float.Apply(floatBytes);
}

void RestoreDefendRush14_Float()
{
    g_PatchDefendRush14_Float.Restore();
}
