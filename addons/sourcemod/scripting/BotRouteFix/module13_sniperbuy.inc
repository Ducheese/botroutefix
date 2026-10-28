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
//========================================================================================
// 随机买狙击枪意愿概率公式宏定义 (Chance = BASE - STEP * Morale，注：当前机器码内已固化立即数 50 与 -10)
#define SNIPER_BUY_CHANCE_BASE  50.0    // 基础买狙意愿概率 (士气 0 时 50%)
#define SNIPER_BUY_CHANCE_STEP  10.0    // 每级士气增减步长 (百分比)
#define SNIPER_BUY_CHANCE_MIN   0.0     // 买狙意愿概率下限
#define SNIPER_BUY_CHANCE_MAX   100.0   // 买狙意愿概率上限

MemoryPatch g_PatchSniperBuy13_Main;
MemoryPatch g_PatchSniperBuy13_Cave;

// 业务开启态
static bool g_bModule13_Active = false;

/**
 * 初始化并激活模块 13
 *
 * @return              是否成功激活
 */
bool InitModule13_SniperBuy()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 13] Error: Gamedata is not loaded, cannot initialize sniper-buy patch!");
        return false;
    }

    if (g_iOffset_Morale <= 0)
    {
        LogError("[BotRouteFix] [Module 13] Error: Morale offset not cached, cannot build chance math!");
        return false;
    }

    EnableModule13_SniperBuy(true);
    if (!g_bModule13_Active)
    {
        LogError("[BotRouteFix] [Module 13] Failed to inject sniper-buy patch!");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 13] sniperRifleChance (50-10*M) patch activated [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 动态启用或停用模块 13（幂等；main/cave 两段补丁全成或全败）
 */
void EnableModule13_SniperBuy(bool enable)
{
    if (enable == g_bModule13_Active)
    {
        return;
    }

    if (enable)
    {
        bool ok = g_bIsWin64 ? PrepSniperBuy13_Cave() : PrepSniperBuy13_Rewrite();
        if (!ok)
        {
            // 全成或全败：失败立即写回，杜绝"jmp 已挂、cave 未写"的半态
            g_PatchSniperBuy13_Main.Restore();
            g_PatchSniperBuy13_Cave.Restore();
            g_bModule13_Active = false;
            LogError("[BotRouteFix] [Module 13] Enable aborted (%s), all patches rolled back!",
                g_bIsWin64 ? "64-bit cave" : "32-bit rewrite");
            return;
        }

        g_bModule13_Active = true;
        return;
    }

    g_PatchSniperBuy13_Main.Restore();
    g_PatchSniperBuy13_Cave.Restore();
    g_bModule13_Active = false;
}

/**
 * 还原模块 13（插件卸载：恢复原始字节并复位业务态）
 */
void RestoreModule13_SniperBuy()
{
    EnableModule13_SniperBuy(false);
    LogMessage("[BotRouteFix] [Module 13] Patch safely restored to original bytes.");
}

/**
 * 查询模块 13 业务开启态
 */
bool IsModule13_Active()
{
    return g_bModule13_Active;
}

/**
 * 查询模块 13 补丁是否完全生效（64位需 Main 与 Cave 双段补丁均生效）
 */
bool IsSniperBuy13Patched()
{
    if (g_bIsWin64)
    {
        return (g_PatchSniperBuy13_Main.isPatched && g_PatchSniperBuy13_Cave.isPatched);
    }
    return g_PatchSniperBuy13_Main.isPatched;
}

/**
 * 计算给定士气值对应的理论想买狙概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              概率 (0.0 ~ 100.0)
 */
float CalculateSniperBuyChance(int morale)
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
//========================================================================================

static bool PrepSniperBuy13_Rewrite()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "SniperBuy13_Dice");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 13] Failed to locate signature for SniperBuy13_Dice!");
        return false;
    }

    g_PatchSniperBuy13_Main.Init("SniperBuy13_Rewrite", sigAddr, 66);

    // 幂等性检测：+21 已是 push imm32（0x68）说明重写已驻留（原生此处是 0xCA/cmovnz 尾字节，sub esp 在 +22）；
    // 有备份=自家补丁，保持旗标；无备份才记 false
    if (LoadFromAddress(sigAddr + view_as<Address>(21), NumberType_Int8) == 0x68)
    {
        if (!g_PatchSniperBuy13_Main.hasBackup)
        {
            g_PatchSniperBuy13_Main.isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 13] Rewrite patch already present @ %X, skipping", sigAddr);
        return true;
    }

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

    // 动态提取版本相关地址与槽位
    int addr100 = LoadFromAddress(sigAddr + view_as<Address>(2), NumberType_Int32);
    int addrT   = LoadFromAddress(sigAddr + view_as<Address>(10), NumberType_Int32);
    int addrCT  = LoadFromAddress(sigAddr + view_as<Address>(15), NumberType_Int32);
    int var10   = LoadFromAddress(sigAddr + view_as<Address>(27), NumberType_Int8);
    int addrRF  = LoadFromAddress(sigAddr + view_as<Address>(41), NumberType_Int32);

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

    return g_PatchSniperBuy13_Main.Apply(p);
}

//========================================================================================
// 64-BIT: jmp-out cave（comiss 7B → E9 + 2 NOP）
// cave @ comiss + 0xC5AB3（前驱 C3，21×CC + 10B 对齐 NOP 共 31B，写前全量校验）
//========================================================================================

static bool PrepSniperBuy13_Cave()
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, "SniperBuy13_Dice");
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 13] Failed to locate signature for SniperBuy13_Dice!");
        return false;
    }

    // comiss 在签名 +12（0F 2F 05），back-target（mov r11）在 comiss+7
    Address comissAddr = sigAddr + view_as<Address>(12);

    // 幂等性检测：comiss 已被 E9 替换说明 cave 已挂（必须先查，否则 opcode 预检误报）；
    // 有备份=自家补丁，保持旗标；无备份才记 false
    if (LoadFromAddress(comissAddr, NumberType_Int8) == 0xE9)
    {
        if (!g_PatchSniperBuy13_Main.hasBackup)
        {
            g_PatchSniperBuy13_Main.isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 13] Cave hook already present @ %X, skipping", comissAddr);
        return true;
    }

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

    // cave 地址 = comiss + caveOffset（同模块相对位移，ASLR 安全；优先从 gamedata 读取，默认兜底 0xC5B13）
    int caveOffset = GameConfGetOffset(g_hGameConf, "SniperBuy13_CaveOffset");
    if (caveOffset <= 0)
    {
        caveOffset = 0xC5B13;
    }
    Address caveAddr = comissAddr + view_as<Address>(caveOffset);

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

    // 先写 cave（25B），再挂 jmp-in，避免 jmp 指向 int3 的窗口
    // cave: ecx/xmm1 已死（后均重载），eax/xmm0/r14 不碰，flags 由 comiss 重建
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

    g_PatchSniperBuy13_Cave.Init("SniperBuy13_Cave", caveAddr, 25);
    if (!g_PatchSniperBuy13_Cave.Apply(caveBytes))
    {
        return false;
    }

    // 挂 jmp-in：E9 <cave> + 2 NOP（盖掉 7B comiss）；失败则回滚 cave，不留半态
    int jmpRel = view_as<int>(caveAddr - (comissAddr + view_as<Address>(5)));
    int jmpBytes[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    for (int b = 0; b < 4; b++)
    {
        jmpBytes[1 + b] = (jmpRel >> (b * 8)) & 0xFF;
    }

    g_PatchSniperBuy13_Main.Init("SniperBuy13_JmpIn", comissAddr, 7);
    if (!g_PatchSniperBuy13_Main.Apply(jmpBytes))
    {
        g_PatchSniperBuy13_Cave.Restore();
        return false;
    }
    return true;
}
