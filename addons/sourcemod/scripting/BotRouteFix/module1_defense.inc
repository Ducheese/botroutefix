#if defined _botroutefix_module1_defense_included
    #endinput
#endif
#define _botroutefix_module1_defense_included

//========================================================================================
// [模块 1] CT 守点概率重构与防开局全队前冲
//
// 业务背景与引擎逆向分析：
//
// 1. CT 开局 33.3% 全队发疯前冲 (IsDefenseRushing):
//    - 源码位置: `bot/cs_bot_manager.cpp:147-149` 与 `bot/states/cs_bot_idle.cpp:407`
//    - 机制分析: 引擎每回合开局掷骰 `m_isDefenseRushing = (RandomFloat(0,100) <= 33.3f)`。
//               在炸弹拆除图 (Defuse Scenario) 下，CT Bot 进入 `IdleState::OnUpdate` 时，
//               若判定 `IsDefenseRushing` 为真，则直接执行 `me->Hunt()` 弃点突击。
//               导致 1/3 的回合整队 CT 彻底放弃包点防守，演变为白给送人头。
//    - 补丁方案: 对应指令为 `0F 85 <rel32>` (JNE -> Hunt)。将其 6 字节彻底 NOP 穿透，
//               使 CT 永不进入 rush 弃点分支，正常顺延到包点防守与战术决策逻辑。
//
// 2. CT 守包点概率笑话 (guardBombsiteChance):
//    - 源码位置: `bot/states/cs_bot_idle.cpp:413-436`
//    - 机制分析: 原生公式为 `guardBombsiteChance = -34.0f * me->GetMorale()`。
//               当 CT 顺风连胜或开局初始士气为 +1 时，计算出的守点概率恒为 -34%！
//               由于 `RandomFloat(0,100) < -34.0f` 恒为假，CT 永远无法进入守点状态。
//    - 补丁方案: 将公式原地重写为 `70.0f - 10.0f * Morale`。
//               士气 -3(大劣): 100% 坚守包点
//               士气  0(均势):  70% 守点
//               士气 +1(微优):  60% 守点
//               士气 +3(大优):  40% 守点 (其余 60% 主动游走抓人)
//               - 64 位: 26 字节纯 SSE 汇编精准替换 (`cvtsi2ss xmm6, eax`)。
//               - 32 位: 55 字节完整 x87 FPU 状态机重写，严格遵守跨 call 浮点栈契约。
//               ★ 机器码立即数固化说明: 当前注入的机器码已将公式 (70.0 - 10.0 * Morale)
//               固化为底层汇编立即数 (add eax, 70 与 imul eax, -10)；下方的 GUARD_CHANCE_*
//               宏定义仅用于 CalculateGuardBombsiteChance 等辅助查询与理论对齐，修改宏
//               不会直接改变二进制立即数（后续若需完全动态自定义需配合机器码动态组装）。
//========================================================================================

// 守点概率公式宏定义 (Chance = BASE - STEP * Morale，注：当前机器码内已固化立即数 70 与 -10)
#define GUARD_CHANCE_BASE       70.0    // 基础守点概率 (士气 0 时 70%)
#define GUARD_CHANCE_STEP       10.0    // 每级士气增减步长 (百分比)
#define GUARD_CHANCE_MIN        0.0     // 守点概率下限
#define GUARD_CHANCE_MAX        100.0   // 守点概率上限

// 模块内存补丁实例
MemoryPatch g_PatchGuardBombsite;
MemoryPatch g_PatchDefenseRush;

// 业务开启态（唯一事实来源；底层 isPatched 由 MemoryPatch 自己维护）
static bool g_bModule1_Active = false;

/**
 * 初始化并激活模块 1 所有内存补丁
 *
 * @return              是否全部成功激活
 */
bool InitModule1_Defense()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 1] Error: Gamedata is not loaded, cannot initialize defense patches!");
        return false;
    }

    EnableModule1_Defense(true);
    if (!g_bModule1_Active)
    {
        LogError("[BotRouteFix] [Module 1] Failed to inject defense patches!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 1] CT guard chance & anti-rush patches activated [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 动态启用或停用模块 1（幂等：同态重复调用不重写代码段）
 */
void EnableModule1_Defense(bool enable)
{
    if (enable == g_bModule1_Active)
    {
        return;
    }

    if (enable)
    {
        bool bGuard = PrepGuardBombsitePatch();
        bool bRush  = PrepDefenseRushPatch();

        // 全成或全败：任一失败立即回滚另一条，杜绝半态驻留
        if (!bGuard || !bRush)
        {
            if (bGuard)
            {
                RestoreGuardBombsitePatch();
            }
            if (bRush)
            {
                RestoreDefenseRushPatch();
            }
            g_bModule1_Active = false;
            LogError("[BotRouteFix] [Module 1] Enable aborted (guard=%d, rush=%d), all patches rolled back!", bGuard, bRush);
            return;
        }

        g_bModule1_Active = true;
        return;
    }

    RestoreGuardBombsitePatch();
    RestoreDefenseRushPatch();
    g_bModule1_Active = false;
}

/**
 * 还原模块 1（插件卸载：恢复原始字节并复位业务态）
 */
void RestoreModule1_Defense()
{
    EnableModule1_Defense(false);
    LogMessage("[BotRouteFix] [Module 1] Patches safely restored to original bytes.");
}

/**
 * 查询模块 1 业务开启态
 */
bool IsModule1_Active()
{
    return g_bModule1_Active;
}

/**
 * 查询守点概率补丁当前是否生效
 */
bool IsGuardBombsitePatched()
{
    return g_PatchGuardBombsite.isPatched;
}

/**
 * 查询防前冲补丁当前是否生效
 */
bool IsDefenseRushPatched()
{
    return g_PatchDefenseRush.isPatched;
}

/**
 * 计算给定士气值对应的理论守点概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              守点概率 (0.0 ~ 100.0)
 */
float CalculateGuardBombsiteChance(int morale)
{
    float chance = GUARD_CHANCE_BASE - GUARD_CHANCE_STEP * float(morale);
    if (chance > GUARD_CHANCE_MAX)
    {
        chance = GUARD_CHANCE_MAX;
    }
    if (chance < GUARD_CHANCE_MIN)
    {
        chance = GUARD_CHANCE_MIN;
    }
    return chance;
}

//========================================================================================
// PATCH 1: GUARD BOMBSITE CHANCE FORMULA REWRITE (70 - 10 * Morale)
//========================================================================================

bool PrepGuardBombsitePatch()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "IdleState_GuardBombsiteChance");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 1] Failed to locate signature for IdleState_GuardBombsiteChance!");
        return false;
    }

    int patchOffset = GameConfGetOffset(g_hGameConf, "GuardBombsite_PatchOffset");
    if (patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 1] Failed to read GuardBombsite_PatchOffset!");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);

    if (g_bIsWin64)
    {
        // -------------------------------------------------------------------------------
        // 64-bit 架构实现 (Steam x64) - 26 字节透明替换
        // -------------------------------------------------------------------------------
        g_PatchGuardBombsite.Init("GuardBombsite_x64", targetAddr, 26);

        // 幂等性检测：检查是否已注入 (8B 87 50 1E 00 00 = mov eax, [rdi+1E50h])
        // 有备份 = 自家补丁，保持 isPatched；无备份 = 外来残留，记 false（无基线可还原）
        if (LoadFromAddress(targetAddr, NumberType_Int8) == 0x8B
            && LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8) == 0x87
            && LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int8) == 0x50)
        {
            if (!g_PatchGuardBombsite.hasBackup)
            {
                g_PatchGuardBombsite.isPatched = false;
            }
            LogMessage("[BotRouteFix] [Module 1] Guard bombsite patch already present @ %X, skipping", targetAddr);
            return true;
        }

        // 预检验证：严格比对 26 字节完整操作码模板 (跳过漂移的寻址字节)
        int chkOff64[18] = {0,1,2,3,8,9,10,11,12,13,14,15,16,17,18,19,20,21};
        int chkVal64[18] = {0x66,0x0F,0x6E,0xB7,0x41,0x0F,0x28,0xC8,0x0F,0x5B,0xF6,0x0F,0x28,0xC7,0xF3,0x0F,0x59,0x35};
        for (int i = 0; i < 18; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff64[i]), NumberType_Int8);
            if (got != chkVal64[i])
            {
                LogError("[BotRouteFix] [Module 1] 64-bit template mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    chkOff64[i], chkVal64[i], got);
                return false;
            }
        }

        // 语义锚点验证：原代码中 mulss 常量必须准确为 -34.0f
        int kRel64 = LoadFromAddress(targetAddr + view_as<Address>(22), NumberType_Int32);
        Address kAddr64 = targetAddr + view_as<Address>(26 + kRel64);
        if (view_as<float>(LoadFromAddress(kAddr64, NumberType_Int32)) != -34.0)
        {
            LogError("[BotRouteFix] [Module 1] Constant mismatch (expected -34.0f), patch aborted!");
            return false;
        }

        // 构造机器码: 70.0 - 10.0 * Morale (严格 26 字节对齐)
        int patchBytes64[26] = {
            0x8B, 0x87, 0x50, 0x1E, 0x00, 0x00, // +0  mov eax, [rdi+1E50h] (m_morale)
            0x6B, 0xC0, 0xF6,                   // +6  imul eax, -10
            0x83, 0xC0, 0x46,                   // +9  add eax, 70
            0xF3, 0x0F, 0x2A, 0xF0,             // +12 cvtsi2ss xmm6, eax
            0x41, 0x0F, 0x28, 0xC8,             // +16 movaps xmm1, xmm8 (100.0f)
            0x0F, 0x28, 0xC7,                   // +20 movaps xmm0, xmm7 (0.0f)
            0x90, 0x90, 0x90                    // +23 3x NOPs 填充
        };

        return g_PatchGuardBombsite.Apply(patchBytes64);
    }
    else
    {
        // -------------------------------------------------------------------------------
        // 32-bit 架构实现 (v91/v92) - 55 字节完整 x87 FPU 重写
        // -------------------------------------------------------------------------------
        g_PatchGuardBombsite.Init("GuardBombsite_x86", targetAddr, 55);

        // 幂等性检测：检查是否已注入 (6B C0 F6 = imul eax, -10 at targetAddr + 9)
        if (LoadFromAddress(targetAddr + view_as<Address>(9), NumberType_Int8) == 0x6B
            && LoadFromAddress(targetAddr + view_as<Address>(10), NumberType_Int8) == 0xC0
            && LoadFromAddress(targetAddr + view_as<Address>(11), NumberType_Int8) == 0xF6)
        {
            if (!g_PatchGuardBombsite.hasBackup)
            {
                g_PatchGuardBombsite.isPatched = false;
            }
            LogMessage("[BotRouteFix] [Module 1] Guard bombsite patch already present @ %X, skipping", targetAddr);
            return true;
        }

        // 预检验证：严格比对 32 位原始 55 字节模板操作码
        int chkOff[33] = {0,1,6,7,8,9,10,15,16,18,19,20,21,22,23,24,25,26,27,28,33,34,36,37,38,39,40,45,46,47,48,49,50};
        int chkVal[33] = {0xD9,0x05,0x83,0xEC,0x08,0x8B,0x87,0x89,0x45,0xD9,0x5C,0x24,0x04,0xD9,0xEE,0xD9,0x1C,0x24,0xFF,0x15,0xDB,0x45,0x83,0xC4,0x08,0xD8,0x0D,0xDF,0xF1,0xDD,0xD8,0x0F,0x86};
        for (int i = 0; i < 33; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff[i]), NumberType_Int8);
            if (got != chkVal[i])
            {
                LogError("[BotRouteFix] [Module 1] 32-bit opcode template mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    chkOff[i], chkVal[i], got);
                return false;
            }
        }

        // 动态提取版本特定的操作数地址与跳转偏移
        int addr100   = LoadFromAddress(targetAddr + view_as<Address>(2), NumberType_Int32);
        int moraleOff = LoadFromAddress(targetAddr + view_as<Address>(11), NumberType_Int32);
        int rfAddr    = LoadFromAddress(targetAddr + view_as<Address>(29), NumberType_Int32);
        int kAddr     = LoadFromAddress(targetAddr + view_as<Address>(41), NumberType_Int32);
        int jbeRel    = LoadFromAddress(targetAddr + view_as<Address>(51), NumberType_Int32);

        // 语义常量验证：浮点常量必须准确为 100.0f 与 -34.0f
        if (view_as<float>(LoadFromAddress(view_as<Address>(addr100), NumberType_Int32)) != 100.0
            || view_as<float>(LoadFromAddress(view_as<Address>(kAddr), NumberType_Int32)) != -34.0)
        {
            LogError("[BotRouteFix] [Module 1] Float constant verification failed (expected 100.0f/-34.0f), patch aborted!");
            return false;
        }

        // 构造机器码: 保持浮点栈平衡，反转 jbe 为 jae
        int patchBytes32[55] = {
            0x83, 0xEC, 0x08,                   // +0  sub esp, 8
            0x8B, 0x87, 0, 0, 0, 0,             // +3  mov eax, [edi + moraleOff]
            0x6B, 0xC0, 0xF6,                   // +9  imul eax, -10
            0x83, 0xC0, 0x46,                   // +12 add eax, 70
            0x89, 0x04, 0x24,                   // +15 mov [esp], eax
            0xDB, 0x04, 0x24,                   // +18 fild dword [esp]       ; st(0) = chance
            0xD9, 0xEE,                         // +21 fldz                  ; st(0) = 0.0, st(1) = chance
            0xD9, 0x1C, 0x24,                   // +23 fstp dword [esp]      ; [esp] = 0.0, st(0) = chance
            0xD9, 0x05, 0, 0, 0, 0,             // +26 fld [100.0]           ; st(0) = 100.0, st(1) = chance
            0xD9, 0x5C, 0x24, 0x04,             // +32 fstp dword [esp+4]    ; [esp+4] = 100.0, st(0) = chance
            0xFF, 0x15, 0, 0, 0, 0,             // +36 call [RandomFloat]    ; st(0) = R, st(1) = chance
            0x83, 0xC4, 0x08,                   // +42 add esp, 8
            0xDF, 0xF1,                         // +45 fcomip st, st(1)      ; 比较 R 与 chance
            0xDD, 0xD8,                         // +47 fstp st               ; 弹出 chance
            0x0F, 0x83, 0, 0, 0, 0              // +49 jae skip              ; 若 R >= chance 则跳过守点
        };

        // 拼接动态提取的 32 位地址与相对跳转
        for (int b = 0; b < 4; b++)
        {
            patchBytes32[5 + b]  = (moraleOff >> (b * 8)) & 0xFF;
            patchBytes32[28 + b] = (addr100 >> (b * 8)) & 0xFF;
            patchBytes32[38 + b] = (rfAddr >> (b * 8)) & 0xFF;
            patchBytes32[51 + b] = (jbeRel >> (b * 8)) & 0xFF;
        }

        return g_PatchGuardBombsite.Apply(patchBytes32);
    }
}

void RestoreGuardBombsitePatch()
{
    g_PatchGuardBombsite.Restore();
}

//========================================================================================
// PATCH 2: CT ANTI-RUSH (IsDefenseRushing NOP BYPASS)
//========================================================================================

bool PrepDefenseRushPatch()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "IdleState_DefenseRush");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 1] Failed to locate signature for IdleState_DefenseRush!");
        return false;
    }

    int patchOffset = GameConfGetOffset(g_hGameConf, "DefenseRush_PatchOffset");
    if (patchOffset == -1)
    {
        LogError("[BotRouteFix] [Module 1] Failed to read DefenseRush_PatchOffset!");
        return false;
    }

    Address targetAddr = sigAddr + view_as<Address>(patchOffset);
    g_PatchDefenseRush.Init("DefenseRush_NOP", targetAddr, 6);

    // 幂等性检测：检查是否已打上 NOP (0x90 0x90)
    if (LoadFromAddress(targetAddr, NumberType_Int8) == 0x90
        && LoadFromAddress(targetAddr + view_as<Address>(1), NumberType_Int8) == 0x90)
    {
        if (!g_PatchDefenseRush.hasBackup)
        {
            g_PatchDefenseRush.isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 1] Anti-Rush NOP patch already present @ %X, skipping", targetAddr);
        return true;
    }

    // 预检验证：比对 JNE 指令 (0F 85) 及其紧随的上下文
    if (g_bIsWin64)
    {
        int chkOff64[6] = {0, 1, 6, 7, 8, 9};
        int chkVal64[6] = {0x0F, 0x85, 0x48, 0x8B, 0xCE, 0xE8}; // jne; mov rcx, rsi; call
        for (int i = 0; i < 6; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff64[i]), NumberType_Int8);
            if (got != chkVal64[i])
            {
                LogError("[BotRouteFix] [Module 1] 64-bit anti-rush template mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    chkOff64[i], chkVal64[i], got);
                return false;
            }
        }
    }
    else
    {
        int chkOff32[5] = {0, 1, 6, 7, 8};
        int chkVal32[5] = {0x0F, 0x85, 0x8B, 0xCB, 0xE8}; // jne; mov ecx, ebx; call
        for (int i = 0; i < 5; i++)
        {
            int got = LoadFromAddress(targetAddr + view_as<Address>(chkOff32[i]), NumberType_Int8);
            if (got != chkVal32[i])
            {
                LogError("[BotRouteFix] [Module 1] 32-bit anti-rush template mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    chkOff32[i], chkVal32[i], got);
                return false;
            }
        }
    }

    // 写入 6 个 NOP (0x90) 穿透该跳转
    return g_PatchDefenseRush.NopOut();
}

void RestoreDefenseRushPatch()
{
    g_PatchDefenseRush.Restore();
}
