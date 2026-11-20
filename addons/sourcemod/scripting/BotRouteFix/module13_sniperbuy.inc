#if defined _botroutefix_module13_sniperbuy_included
    #endinput
#endif
#define _botroutefix_module13_sniperbuy_included

//========================================================================================
// [模块 13] 随机买狙概率重构 sniperRifleChance (50-10*Morale)
//
// 业务背景与引擎逆向分析：
//
// 1. 原生机制 (cs_bot_buy.cpp:522-524, BuyState::OnUpdate 随机买分支)：
//      const float sniperRifleChance = 50.0f;
//      bool wantSniper = (RandomFloat(0, 100) < sniperRifleChance);
//    只决定 SNIPER_RIFLE 进不进随机候选名单（之后再均匀抽）；画像偏好买走另一条分支，
//    不受影响。bot_randombuy 开启时 wantSniper 恒真（cmovnz 短路，保留）。
//
// 2. 补丁方案：wantSniper = (RandomFloat(0,100) < 50.0f - 10.0f * Morale)。
//      士气 -3(大劣): 80% 想买狙
//      士气  0(均势): 50% 想买狙（= 原生）
//      士气 +3(大优): 20% 想买狙
//    注意稀释效应：wantSniper=true 只是"狙进名单"，真买到还要再抽一次，实际买狙率更低。
//
// 3. 二进制实证：
//    - 32 位（v91/v92 逐字节同形，签名各 n=1 @ 0x102B76FF / 0x102B9ACF）：
//      fld [100.0] → team 检查 → masterPrimary 选择 → RandomFloat(0,100) → fld [50.0] →
//      fcomip → jbe/mov cl,1 → bot_randombuy OR。66 字节整窗原地重写：骰子改走整数
//      imul eax,-10 / add eax,50 / fild，setbe 保持与原生 jbe 完全一致的严格小于语义
//      （R < chance ⟺ !(chance <= R)，浮点相等零测集也不差分毫）。
//      edi 在窗内恒为 me（入口 mov ecx,edi 调 GetTeamNumber，此后只读；774C/9B1C 才重载），
//      故 morale 取 [edi+g_iOffset_Morale]；eax=team（GetTeamNumber 残留）复用。
//    - 64 位（签名 n=1 @ 0x18035ECA2，comiss 在 +12）：
//      RandomFloat(0,100) → comiss xmm0, [50.0 共用常量] → setb al → randombuy OR。
//      50.0 是 HideState ambush 骰子共用的常量，不可直改；窗内 46 字节塞不下
//      math(20B)+6 loads(35B)+setb(3B)=58B，故用 jmp-out cave：comiss(7B) 改 E9 + 2 NOP，
//      cave 落在 0x1804247C1（前驱 C3 无 fall-through，21×CC + 10B 对齐 NOP 共 31B，
//      由 Gamedata SniperBuy13_CaveOffset 动态配置相对位移 0xC5B13，写前全量校验，漂移则 abort）。
//      cave 用已死寄存器 ecx/xmm1（r11/r12d/rbx/rcx/r14/r8d 在后均重载；eax 不碰，xmm0 只读），
//      E9 回跳保留 flags 给 setb。
//    ★ 机器码立即数固化说明: 当前注入的机器码已将公式 (50.0 - 10.0 * Morale)
//      固化为底层汇编立即数 (add ecx/eax, 50 与 imul ecx/eax, -10)；下方的 SNIPER_BUY_CHANCE_*
//      宏定义仅供 CalculateSniperBuyChance 等辅助查询与理论对齐，修改宏不会直接改变底层汇编立即数。
//
// 事项契约:
//   - 唯一事项 = 买狙公式重写（ITEM_M13_BUY）。
//   - 32 位单段整窗重写；64 位 jmp-in + cave 双段，全成或全败。
//   - 物理写入失败证伪探测结论 → Ready 清空并回滚，不允许半态驻留。
//========================================================================================

//========================================================================================
// MODULE 13 DEFINES & GLOBALS
//========================================================================================

// 随机买狙击枪意愿概率公式宏定义 (Chance = BASE - STEP * Morale，注：当前机器码内已固化立即数 50 与 -10)
#define SNIPER_BUY_CHANCE_BASE  50.0    // 基础买狙意愿概率 (士气 0 时 50%)
#define SNIPER_BUY_CHANCE_STEP  10.0    // 每级士气增减步长 (百分比)
#define SNIPER_BUY_CHANCE_MIN   0.0     // 买狙意愿概率下限
#define SNIPER_BUY_CHANCE_MAX   100.0   // 买狙意愿概率上限

static MemoryPatch g_PatchSniperBuy_Main;
static MemoryPatch g_PatchSniperBuy_Cave;

// Prep 构造、Apply 原样写入的机器码缓冲
static int g_BuyMainBytes[66];
static int g_BuyCaveBytes[25];
static bool g_bBuyUseCave = false;    // true = 64 位 jmp-in + cave 双段

// 模块规则开启态（Enable 幂等缓存；业务判据统一用 Item_IsArmed）
static bool g_bModule13_Active = false;

//========================================================================================
// MODULE 13 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：买狙公式重写（ITEM_M13_BUY）
 *
 * 按架构定位/校验/备份并构造机器码，不写入代码段（写入由 Apply 执行）。
 * 任一后端失败返回 false，Init 会把事项标记为 FAILED。
 *
 * @return              true = 买狙事项后端就绪
 */
static bool PrepModule13_BuyBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 13] Gamedata missing; sniper-buy item FAILED.");
        return false;
    }

    if (g_iOffset_Morale <= 0)
    {
        LogError("[BotRouteFix] [Module 13] Morale offset missing; sniper-buy item FAILED.");
        return false;
    }

    g_bBuyUseCave = g_bIsWin64;
    return g_bBuyUseCave ? PrepModule13_CaveSite() : PrepModule13_RewriteSite();
}

//----------------------------------------------------------------------------------------
// 32-BIT: 66 字节整窗重写（v91/v92 同形）
// 窗布局（相对签名起点）：
//   +0  D9 05 <a100>      fld [100.0]          +6  83 F8 02      cmp eax,2
//   +9  BA <T>            mov edx,BuyInfoT     +14 B9 <CT>       mov ecx,BuyInfoCT
//   +19 0F 45 CA          cmovnz ecx,edx       +22 83 EC 08      sub esp,8
//   +25 89 4D <v10>       mov [ebp+var_10],ecx +28 33 F6         xor esi,esi
//   +30 D9 5C 24 04       fstp [esp+4]         +34 D9 EE         fldz
//   +36 D9 1C 24          fstp [esp]           +39 FF 15 <rf>    call [RandomFloat]
//   +45 D9 05 <a50>       fld [50.0]           +51 83 C4 08      add esp,8
//   +54 DF F1             fcomip               +56 DD D8         fstp st
//   +58 76 04             jbe                  +60 B1 01         mov cl,1
//   +62 EB 02             jmp                  +64 32 C9         xor cl,cl
//----------------------------------------------------------------------------------------

/**
 * 事项 M13 买狙公式重写的 32 位后端准备：定位/校验/备份并构造 66 字节
 *
 * 失败语义：签名/模板/常量/备份任一失败返回 false，整个买狙事项 FAILED。
 *
 * @return              true = 重写后端就绪
 */
static bool PrepModule13_RewriteSite()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "SniperBuy13_Dice");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 13] Failed to locate signature for SniperBuy13_Dice!");
        return false;
    }

    g_PatchSniperBuy_Main.Init("SniperBuy_Rewrite", sigAddr, 66);

    // 幂等性检测：+21 已是 push imm32（0x68）说明重写已驻留（原生此处是 0xCA/cmovnz 尾字节，sub esp 在 +22）；
    // 有备份=自家补丁；无备份=外来改写，记 false（无基线可还原）
    bool alreadyPatched = (LoadFromAddress(sigAddr + view_as<Address>(21), NumberType_Int8) == 0x68);
    if (alreadyPatched && !g_PatchSniperBuy_Main.hasBackup)
    {
        g_PatchSniperBuy_Main.isPatched = false;
        LogError("[BotRouteFix] [Module 13] Rewrite already present without baseline @ %X; sniper-buy item FAILED.", sigAddr);
        return false;
    }

    if (alreadyPatched)
    {
        LogMessage("[BotRouteFix] [Module 13] Rewrite patch already present @ %X, skipping", sigAddr);
    }
    else
    {
        // 预检：比对关键操作码（漂移的地址/槽位字节跳过；33 F6 / 32 C9 必须成对出现）
        int chkOff[24] = {0,1,6,7,8,9,14,19,20,21,22,23,24,25,28,29,39,40,54,55,56,57,64,65};
        int chkVal[24] = {0xD9,0x05,0x83,0xF8,0x02,0xBA,0xB9,0x0F,0x45,0xCA,0x83,0xEC,0x08,0x89,0x33,0xF6,0xFF,0x15,0xDF,0xF1,0xDD,0xD8,0x32,0xC9};
        for (int i = 0; i < 24; i++)
        {
            int got = LoadFromAddress(sigAddr + view_as<Address>(chkOff[i]), NumberType_Int8);
            if (got != chkVal[i])
            {
                LogError("[BotRouteFix] [Module 13] 32-bit template mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    chkOff[i], chkVal[i], got);
                return false;
            }
        }
    }

    if (!g_PatchSniperBuy_Main.BackupOriginal())
    {
        LogError("[BotRouteFix] [Module 13] Rewrite baseline backup failed; sniper-buy item FAILED.");
        return false;
    }

    // 版本相关地址与槽位一律从原始基线读取：已驻留时当前字节已被本插件覆盖，
    // 只有备份里还留着真实操作数（addrT/addrCT/var10/addrRF）。
    int addr100 = g_PatchSniperBuy_Main.originalBytes[2]
        | (g_PatchSniperBuy_Main.originalBytes[3] << 8)
        | (g_PatchSniperBuy_Main.originalBytes[4] << 16)
        | (g_PatchSniperBuy_Main.originalBytes[5] << 24);
    int addrT = g_PatchSniperBuy_Main.originalBytes[10]
        | (g_PatchSniperBuy_Main.originalBytes[11] << 8)
        | (g_PatchSniperBuy_Main.originalBytes[12] << 16)
        | (g_PatchSniperBuy_Main.originalBytes[13] << 24);
    int addrCT = g_PatchSniperBuy_Main.originalBytes[15]
        | (g_PatchSniperBuy_Main.originalBytes[16] << 8)
        | (g_PatchSniperBuy_Main.originalBytes[17] << 16)
        | (g_PatchSniperBuy_Main.originalBytes[18] << 24);
    int var10 = g_PatchSniperBuy_Main.originalBytes[27];
    int addrRF = g_PatchSniperBuy_Main.originalBytes[41]
        | (g_PatchSniperBuy_Main.originalBytes[42] << 8)
        | (g_PatchSniperBuy_Main.originalBytes[43] << 16)
        | (g_PatchSniperBuy_Main.originalBytes[44] << 24);

    // 语义锚点：100.0 常量校验（50.0 不再被引用，无需校验）
    if (view_as<float>(LoadFromAddress(view_as<Address>(addr100), NumberType_Int32)) != 100.0)
    {
        LogError("[BotRouteFix] [Module 13] Constant mismatch (expected 100.0f), patch aborted!");
        return false;
    }

    // 构造 66 字节：选择逻辑原样 + push 传参 + 整数骰子 + setbe 精确语义
    int p[66] = {
        0x83, 0xF8, 0x02,                   // +0  cmp eax,2 (team 残留)
        0xBA, 0, 0, 0, 0,                   // +3  mov edx,BuyInfoT
        0xB9, 0, 0, 0, 0,                   // +8  mov ecx,BuyInfoCT
        0x0F, 0x45, 0xCA,                   // +13 cmovnz ecx,edx
        0x89, 0x4D, 0,                      // +16 mov [ebp+var_10],ecx
        0x33, 0xF6,                         // +19 xor esi,esi (count=0)
        0x68, 0x00, 0x00, 0xC8, 0x42,       // +21 push 100.0f
        0x6A, 0x00,                         // +26 push 0
        0xFF, 0x15, 0, 0, 0, 0,             // +28 call [RandomFloat] → st(0)=R
        0x8B, 0x87, 0, 0, 0, 0,             // +34 mov eax,[edi+morale] (edi=me)
        0x6B, 0xC0, 0xF6,                   // +40 imul eax,-10
        0x83, 0xC0, 0x32,                   // +43 add eax,50
        0x89, 0x04, 0x24,                   // +46 mov [esp],eax
        0xDB, 0x04, 0x24,                   // +49 fild [esp] → st=chance
        0x83, 0xC4, 0x08,                   // +52 add esp,8
        0xDF, 0xF1,                         // +55 fcomip (chance vs R)
        0xDD, 0xD8,                         // +57 fstp st
        0x0F, 0x96, 0xC1,                   // +59 setbe cl (chance<=R ⟺ !(R<chance))
        0x80, 0xF1, 0x01,                   // +62 xor cl,1 → cl=(R<chance)
        0x90                                // +65 NOP 对齐 66
    };

    // 拼接动态地址（morale 偏移走全局缓存 g_iOffset_Morale）
    for (int b = 0; b < 4; b++)
    {
        p[4 + b]  = (addrT >> (b * 8)) & 0xFF;
        p[9 + b]  = (addrCT >> (b * 8)) & 0xFF;
        p[30 + b] = (addrRF >> (b * 8)) & 0xFF;
        p[36 + b] = (g_iOffset_Morale >> (b * 8)) & 0xFF;
    }
    p[18] = var10;

    for (int i = 0; i < 66; i++)
    {
        g_BuyMainBytes[i] = p[i];
    }

    LogMessage("[BotRouteFix] [Module 13] Rewrite block verified @ %X", sigAddr);
    return true;
}

//----------------------------------------------------------------------------------------
// 64-BIT: jmp-out cave（comiss 7B → E9 + 2 NOP）
// cave @ comiss + 0xC5AB3（前驱 C3，21×CC + 10B 对齐 NOP 共 31B，写前全量校验）
//----------------------------------------------------------------------------------------

/**
 * 事项 M13 买狙公式重写的 64 位后端准备：jmp-in + cave 定位/校验/备份并构造两段字节
 *
 * 失败语义：签名/模板/常量/常量池/备份任一失败返回 false，整个买狙事项 FAILED。
 *
 * @return              true = 双段后端就绪
 */
static bool PrepModule13_CaveSite()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "SniperBuy13_Dice");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 13] Failed to locate signature for SniperBuy13_Dice!");
        return false;
    }

    // comiss 在签名 +12（0F 2F 05），back-target（mov r11）在 comiss+7
    Address comissAddr = sigAddr + view_as<Address>(12);

    // cave 地址 = comiss + caveOffset（同模块相对位移，ASLR 安全；优先从 gamedata 读取，默认兜底 0xC5B13）
    int caveOffset = GameConfGetOffset(g_hGameConf, "SniperBuy13_CaveOffset");
    if (caveOffset <= 0)
    {
        caveOffset = 0xC5B13;
    }
    Address caveAddr = comissAddr + view_as<Address>(caveOffset);

    g_PatchSniperBuy_Main.Init("SniperBuy_JmpIn", comissAddr, 7);
    g_PatchSniperBuy_Cave.Init("SniperBuy_Cave", caveAddr, 25);

    // 幂等性检测：comiss 已被 E9 替换说明 cave 已挂（必须先查，否则 opcode 预检误报）；
    // 有备份=自家补丁；无备份=外来改写，记 false（无基线可还原）
    bool alreadyPatched = (LoadFromAddress(comissAddr, NumberType_Int8) == 0xE9);
    if (alreadyPatched && (!g_PatchSniperBuy_Main.hasBackup || !g_PatchSniperBuy_Cave.hasBackup))
    {
        g_PatchSniperBuy_Main.isPatched = false;
        g_PatchSniperBuy_Cave.isPatched = false;
        LogError("[BotRouteFix] [Module 13] Cave hook already present without baseline @ %X; sniper-buy item FAILED.", comissAddr);
        return false;
    }

    if (alreadyPatched)
    {
        LogMessage("[BotRouteFix] [Module 13] Cave hook already present @ %X, skipping", comissAddr);
    }
    else
    {
        if (LoadFromAddress(comissAddr, NumberType_Int8) != 0x0F
            || LoadFromAddress(comissAddr + view_as<Address>(1), NumberType_Int8) != 0x2F
            || LoadFromAddress(comissAddr + view_as<Address>(2), NumberType_Int8) != 0x05)
        {
            LogError("[BotRouteFix] [Module 13] 64-bit comiss opcode mismatch, patch aborted!");
            return false;
        }

        // 语义锚点：comiss 的比较常量必须确为 50.0f（RIP 解算）
        int rel50 = LoadFromAddress(comissAddr + view_as<Address>(3), NumberType_Int32);
        Address float50 = comissAddr + view_as<Address>(7 + rel50);
        if (view_as<float>(LoadFromAddress(float50, NumberType_Int32)) != 50.0)
        {
            LogError("[BotRouteFix] [Module 13] Constant mismatch (expected 50.0f), patch aborted!");
            return false;
        }

        // 前驱必须是 C3（ret），排除 fall-through 落入 cave 的可能
        if (LoadFromAddress(caveAddr - view_as<Address>(1), NumberType_Int8) != 0xC3)
        {
            LogError("[BotRouteFix] [Module 13] Cave predecessor is not C3, patch aborted!");
            return false;
        }

        int caveExpect[31] = {
            0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
            0xCC,0xCC,0xCC,0xCC,0xCC,0x66,0x66,0x0F,0x1F,0x84,0x00,0x00,0x00,0x00,0x00
        };
        for (int i = 0; i < 31; i++)
        {
            int got = LoadFromAddress(caveAddr + view_as<Address>(i), NumberType_Int8);
            if (got != caveExpect[i])
            {
                LogError("[BotRouteFix] [Module 13] Cave site mismatch at +%d (expected %02X, got %02X), patch aborted!",
                    i, caveExpect[i], got);
                return false;
            }
        }
    }

    if (!g_PatchSniperBuy_Main.BackupOriginal() || !g_PatchSniperBuy_Cave.BackupOriginal())
    {
        LogError("[BotRouteFix] [Module 13] Cave/jmp-in baseline backup failed; sniper-buy item FAILED.");
        return false;
    }

    // 构造 cave（25B）：ecx/xmm1 已死（后均重载），eax/xmm0/r14 不碰，flags 由 comiss 重建
    Address backAddr = comissAddr + view_as<Address>(7);
    int backRel = view_as<int>(backAddr - (caveAddr + view_as<Address>(25)));
    int caveBytes[25] = {
        0x41, 0x8B, 0x8E, 0, 0, 0, 0,       // +0 mov ecx,[r14+morale] (r14=me)
        0x6B, 0xC9, 0xF6,                   // +7 imul ecx,-10
        0x83, 0xC1, 0x32,                   // +10 add ecx,50
        0xF3, 0x0F, 0x2A, 0xC9,             // +13 cvtsi2ss xmm1,ecx
        0x0F, 0x2F, 0xC1,                   // +17 comiss xmm0,xmm1 (R vs chance)
        0xE9, 0, 0, 0, 0                    // +20 jmp back (mov r11)
    };
    for (int b = 0; b < 4; b++)
    {
        caveBytes[3 + b]  = (g_iOffset_Morale >> (b * 8)) & 0xFF;
        caveBytes[21 + b] = (backRel >> (b * 8)) & 0xFF;
    }

    // 构造 jmp-in（7B）：E9 <cave> + 2 NOP（盖掉 7B comiss）
    int jmpRel = view_as<int>(caveAddr - (comissAddr + view_as<Address>(5)));
    int jmpBytes[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    for (int b = 0; b < 4; b++)
    {
        jmpBytes[1 + b] = (jmpRel >> (b * 8)) & 0xFF;
    }

    for (int i = 0; i < 25; i++)
    {
        g_BuyCaveBytes[i] = caveBytes[i];
    }
    for (int i = 0; i < 7; i++)
    {
        g_BuyMainBytes[i] = jmpBytes[i];
    }

    LogMessage("[BotRouteFix] [Module 13] Cave block verified @ %X (jmp-in @ %X)", caveAddr, comissAddr);
    return true;
}

//========================================================================================
// MODULE 13 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 13 的唯一事项：随机买狙公式重写
 *
 * @return              true = 事项已武装
 */
bool InitModule13_SniperBuy()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M13_BUY, false);
        LogError("[BotRouteFix] [Module 13] Gamedata missing; sniper-buy item FAILED.");
        return false;
    }

    Item_SetReady(ITEM_M13_BUY, PrepModule13_BuyBackend());
    EnableModule13_SniperBuy(true);
    if (!Item_IsArmed(ITEM_M13_BUY))
    {
        LogModuleAllFailed(13);
        return false;
    }

    LogModuleArmed(13, "Sniper buy", "1");
    return true;
}

/**
 * 动态启用或停用模块 13
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule13_SniperBuy(bool enable)
{
    if (enable == g_bModule13_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule13_BuyBackend();
        g_bModule13_Active = false;
        return;
    }

    g_bModule13_Active = ApplyModule13_BuyBackend();
}

/**
 * 还原模块 13（插件卸载：恢复原始字节并复位业务态）
 */
void RestoreModule13_SniperBuy()
{
    EnableModule13_SniperBuy(false);
    LogMessage("[BotRouteFix] [Module 13] Sniper buy patches safely restored to original bytes.");
}

//========================================================================================
// MODULE 13 PUBLIC GETTERS & FORMULA
//========================================================================================

/**
 * 计算给定士气值对应的理论想买狙概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              概率 (0.0 ~ 100.0)
 */
float Module13_CalculateSniperBuyChance(int morale)
{
    float chance = SNIPER_BUY_CHANCE_BASE - SNIPER_BUY_CHANCE_STEP * float(morale);
    if (chance > SNIPER_BUY_CHANCE_MAX)
    {
        chance = SNIPER_BUY_CHANCE_MAX;
    }
    else if (chance < SNIPER_BUY_CHANCE_MIN)
    {
        chance = SNIPER_BUY_CHANCE_MIN;
    }
    return chance;
}

//========================================================================================
// MODULE 13 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用买狙公式事项：全成或全败
 *
 * 物理写入失败证伪了 Prep 的模板校验，必须同步清 Ready 并回滚已写段落。
 *
 * @return              true = 补丁已驻留
 */
static bool ApplyModule13_BuyBackend()
{
    if (!Item_IsReady(ITEM_M13_BUY))
    {
        Item_SetArmed(ITEM_M13_BUY, false);
        return false;
    }

    if (g_bBuyUseCave)
    {
        // 先写 cave 再挂 jmp-in，避免 jmp 指向未初始化 cave 的窗口
        if (!g_PatchSniperBuy_Cave.Apply(g_BuyCaveBytes))
        {
            Item_SetReady(ITEM_M13_BUY, false);
            Item_SetArmed(ITEM_M13_BUY, false);
            LogError("[BotRouteFix] [Module 13] Cave write failed; sniper-buy item FAILED.");
            return false;
        }

        if (!g_PatchSniperBuy_Main.Apply(g_BuyMainBytes))
        {
            g_PatchSniperBuy_Cave.Restore();
            Item_SetReady(ITEM_M13_BUY, false);
            Item_SetArmed(ITEM_M13_BUY, false);
            LogError("[BotRouteFix] [Module 13] Jmp-in write failed; cave rolled back, sniper-buy item FAILED.");
            return false;
        }
    }
    else
    {
        if (!g_PatchSniperBuy_Main.Apply(g_BuyMainBytes))
        {
            Item_SetReady(ITEM_M13_BUY, false);
            Item_SetArmed(ITEM_M13_BUY, false);
            LogError("[BotRouteFix] [Module 13] Rewrite write failed; sniper-buy item FAILED.");
            return false;
        }
    }

    Item_SetArmed(ITEM_M13_BUY, true);
    return true;
}

/**
 * 还原买狙公式事项：写回两段原始字节并解除武装
 */
static void RestoreModule13_BuyBackend()
{
    g_PatchSniperBuy_Main.Restore();
    g_PatchSniperBuy_Cave.Restore();
    Item_SetArmed(ITEM_M13_BUY, false);
}

