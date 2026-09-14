#if defined _botroutefix_module12_campgate_included
    #endinput
#endif
#define _botroutefix_module12_campgate_included

//========================================================================================
// [模块 12] 狙击 camp 概率重构 (60-10*Morale)：原生骰子 NOP 全进 + OnEnter 重骰门
//
// 业务背景与引擎逆向分析：
//
// 1. 原生机制：cs_bot_idle.cpp 有 6 处狙击 camp 分支（v91/v92/Steam64 三版反编译逐一
//    确认；源码另有 VIP-CT 一处，v91/v92 老版本不存在，属版本差）：
//      进攻 camp（offenseSniperCampChance = 10.0）：RandomFloat(0,100) < 10.0 → Hide → camp
//      防守 camp（defenseSniperCampChance = 75.0）：RandomFloat(0,100) <= 75.0 → Hide → camp
//    六处共同点：门禁含 IsSniper()，命中后一律 SetTask(MOVE_TO_SNIPER_SPOT) 再 Hide()。
//    六处二进制形状各异（jbe/jb/ja 短长不一、跳向 skip 还是跳向 camp 也不同），
//    逐处公式重写需 18 个 cave，64 位又无处落 cave（最长连续 CC 仅 21B），不可行。
//
// 2. 等价重构方案（与原生逐条对照，差异见 §4）：
//      a) 六处骰子 jcc 全部旁路（指向 skip 的 NOP 掉，指向 camp 的改无条件 jmp），
//         狙击手必进 Hide，task 照常置 19；
//      b) 复用模块 15 的 HideState::OnEnter post 钩：task == 19 的 Hide 即 camp 分支，
//         在此按 60.0f - 10.0f * Morale 重骰（SP 侧 GetRandomFloat，每 bot 独立）：
//           士气 -3(大劣): 90% 留守蹲点
//           士气  0(均势): 60% 留守蹲点
//           士气 +3(大优): 30% 留守蹲点（其余 70% 转 Hunt 前压）
//         ★ 短路契约: 重定向去 Hunt 的 Bot 返回 true，模块 15 立即 short-circuit
//         跳过蹲守时长缩放，避免对即将切态的 Bot 进行无效计算；留守者返回 false 正常缩放；
//      c) 未通过重骰：递延一帧 SDKCall CCSBot::Hunt()（= 原生前压语义）+ 同回调内把
//         disposition 写回 ENGAGE_AND_INVESTIGATE 做保险（分支在 Hide 返回后会写
//         OPPORTUNITY_FIRE；HuntState::OnEnter 本体已置 ENGAGE+SEEK，回调内再写一次
//         即双保险，时序恒在其后）。
//         注意：绝不能在 OnEnter-post 钩内同步调 Hunt()——SetState 先调 OnEnter 后写
//         m_state（statemachine.cpp:48-54），内层置的 hunt 会被外层 Hide 覆盖。
//         一切状态切换必须递延到帧外（RequestFrame），这是本模块第一铁律。
//
// 3. 与原生的差异清单（用户已确认接受）：
//      - fail 路径统一递延走 Hunt()，而非各分支原生 fallthrough（运包/守点/继续 Hunt）。
//        高士气 fail→Hunt 正是 spec 要的前压；掉包不捡的角落由模块 9 花名册兜底
//        （事件驱动，Hunt 态也可被选举去 Fetch）。
//      - task 终值为 SEEK_AND_DESTROY(0)（HuntState::OnEnter 置的，非残留 19）。
//      - PrintIfWatched("Sniping!") 会先刷一条再转 Hunt（仅 watched 调试可见）。
//      - 原生进攻 `<` / 防守 `<=` 的 1-ulp 差分被统一为严格 `<`（浮点相等零测集）。
//
// 4. Fail-safe：Hunt SDKCall 缺失或任一 NOP 失败 → 全部回滚 = 原生行为。
//    六处 jcc 逐一预检 + 幂等 + 还原；E9 转换的两处额外校验跳目标确为 camp tail
//    （32 位 fld30 = D9 05，64 位 task19 = C7 87…13），防跳反。
//========================================================================================

// 狙击手留守蹲点概率公式宏定义 (Chance = BASE - STEP * Morale)
#define CAMP_CHANCE_BASE        60.0    // 基础留守概率 (士气 0 时 60%)
#define CAMP_CHANCE_STEP        10.0    // 每级士气增减步长 (百分比)
#define CAMP_CHANCE_MIN         0.0     // 留守概率下限
#define CAMP_CHANCE_MAX         100.0   // 留守概率上限
#define BOT_TASK_MOVE_TO_SNIPER_SPOT 19 // 引擎原生任务枚举：MOVE_TO_SNIPER_SPOT (狙击手前往架枪点)

MemoryPatch g_PatchCamp12_Sites[6];
static int g_iOffset_Task12 = -1;
static int g_iOffset_Disp12 = -1;
static bool g_bModule12_Armed = false;
static int g_iTotalCampRedirected = 0;

// 站点表：0=O1 1=O2 2=O3 3=D1 4=D2 5=D3；O2/O3 为 ja/jb-to-camp，需 E9 转换，其余 NOP
static const char g_sCamp12_SigNames[6][32] =
{
    "Camp12_O1", "Camp12_O2", "Camp12_O3",
    "Camp12_D1", "Camp12_D2", "Camp12_D3"
};
static const char g_sCamp12_OffNames[6][32] =
{
    "Camp12_O1_Jcc", "Camp12_O2_Jcc", "Camp12_O3_Jcc",
    "Camp12_D1_Jcc", "Camp12_D2_Jcc", "Camp12_D3_Jcc"
};

/**
 * 初始化并激活模块 12（Hunt SDKCall 先行，6/6 NOP 全成或全败）
 *
 * @return              是否成功激活
 */
bool InitModule12_CampGate()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 12] Error: Gamedata is not loaded, cannot initialize camp gate!");
        return false;
    }

    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [Module 12] Error: CCSBot_Hunt SDKCall not ready, aborting (native dice untouched)!");
        return false;
    }

    g_iOffset_Task12 = GameConfGetOffset(g_hGameConf, "CCSBot_Task_Offset");
    g_iOffset_Disp12 = GameConfGetOffset(g_hGameConf, "CCSBot_Disposition_Offset");
    if (g_iOffset_Task12 <= 0 || g_iOffset_Disp12 <= 0)
    {
        LogError("[BotRouteFix] [Module 12] Error: Task/Disposition offsets missing!");
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        if (!PrepCamp12_Site(i))
        {
            LogError("[BotRouteFix] [Module 12] Failed at site %d, rolling back to native dice!", i);
            for (int j = 0; j < 6; j++)
            {
                g_PatchCamp12_Sites[j].Restore();
            }
            g_bModule12_Armed = false;
            return false;
        }
    }

    g_bModule12_Armed = true;
    LogMessage("[BotRouteFix] [Module 12] Sniper camp gate (60-10*M) armed: 6/6 dice bypassed [%s]",
        g_bIsWin64 ? "64-bit" : "32-bit");
    return true;
}

/**
 * 还原模块 12（6 处 jcc 写回 + 解除武装；钩子本身归模块 15 管）
 */
void RestoreModule12_CampGate()
{
    for (int i = 0; i < 6; i++)
    {
        g_PatchCamp12_Sites[i].Restore();
    }
    g_bModule12_Armed = false;
    LogMessage("[BotRouteFix] [Module 12] Dice bypasses restored, gate disarmed.");
}

/**
 * 动态启用或停用模块 12
 */
void EnableModule12_CampGate(bool enable)
{
    if (enable)
    {
        if (!g_bModule12_Armed)
        {
            InitModule12_CampGate();
        }
    }
    else
    {
        RestoreModule12_CampGate();
    }
}

/**
 * 查询模块 12 门禁是否武装
 */
bool IsCamp12Armed()
{
    return g_bModule12_Armed;
}

/**
 * 累积重定向次数（骰子未过转 Hunt，自加载起单调递增）
 */
int GetTotalCampRedirected()
{
    return g_iTotalCampRedirected;
}

/**
 * 计算给定士气值对应的理论留守概率 (百分比 0.0 ~ 100.0)
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              留守概率 (0.0 ~ 100.0)
 */
float CalculateCampChance(int morale)
{
    float chance = CAMP_CHANCE_BASE - CAMP_CHANCE_STEP * float(morale);
    if (chance > CAMP_CHANCE_MAX)
    {
        chance = CAMP_CHANCE_MAX;
    }
    if (chance < CAMP_CHANCE_MIN)
    {
        chance = CAMP_CHANCE_MIN;
    }
    return chance;
}

//========================================================================================
// 单站点旁路：O1/D1/D2/D3 = NOP（指向 skip）；O2/O3 = E9 转换（指向 camp）
//========================================================================================

static bool PrepCamp12_Site(int siteIndex)
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, g_sCamp12_SigNames[siteIndex]);
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 12] Failed to locate signature for %s!", g_sCamp12_SigNames[siteIndex]);
        return false;
    }

    int jccOffset = GameConfGetOffset(g_hGameConf, g_sCamp12_OffNames[siteIndex]);
    if (jccOffset == -1)
    {
        LogError("[BotRouteFix] [Module 12] Failed to read %s!", g_sCamp12_OffNames[siteIndex]);
        return false;
    }

    Address jccAddr = sigAddr + view_as<Address>(jccOffset);
    bool isConvert = (siteIndex == 1 || siteIndex == 2); // O2/O3：ja/jb-to-camp 改 E9

    char patchName[32];
    Format(patchName, sizeof(patchName), "Camp12_S%d", siteIndex);

    int b0 = LoadFromAddress(jccAddr, NumberType_Int8);
    int b1 = LoadFromAddress(jccAddr + view_as<Address>(1), NumberType_Int8);

    // 幂等性检测必须先于 opcode 断言（已旁路的字节不再是原助记符）；
    // 有备份=自家补丁，保持旗标（状态行如实 ACTIVE）；无备份=外来已旁路，记 false
    if ((!isConvert && b0 == 0x90) || (isConvert && b0 == 0xE9))
    {
        if (!g_PatchCamp12_Sites[siteIndex].hasBackup)
        {
            g_PatchCamp12_Sites[siteIndex].isPatched = false;
        }
        LogMessage("[BotRouteFix] [Module 12] Site %d already bypassed @ %X, skipping", siteIndex, jccAddr);
        return true;
    }

    if (!isConvert)
    {
        // NOP 路径：断言 jcc 助记符与长度（32: O1=0F86/D1=72/D2,D3=0F82；64: O1=0F83/D1=77/D2,D3=0F87）
        int expect0, expect1 = -1, patchSize;
        if (g_bIsWin64)
        {
            if (siteIndex == 0)      { expect0 = 0x0F; expect1 = 0x83; patchSize = 6; }
            else if (siteIndex == 3) { expect0 = 0x77; patchSize = 2; }
            else                     { expect0 = 0x0F; expect1 = 0x87; patchSize = 6; }
        }
        else
        {
            if (siteIndex == 0)      { expect0 = 0x0F; expect1 = 0x86; patchSize = 6; }
            else if (siteIndex == 3) { expect0 = 0x72; patchSize = 2; }
            else                     { expect0 = 0x0F; expect1 = 0x82; patchSize = 6; }
        }

        if (b0 != expect0 || (expect1 != -1 && b1 != expect1))
        {
            LogError("[BotRouteFix] [Module 12] Site %d opcode mismatch (expected %02X %02X, got %02X %02X), aborted!",
                siteIndex, expect0, expect1, b0, b1);
            return false;
        }

        // fallthrough 锚：NOP 站的 fallthrough 必须是 camp-inline（签名止于 jcc 的站点需补查；
        // O1-64 的锚已在签名内）。锚值经 IDA 验：32-O1 fallthrough=D9 05 / D1=8B / D2,D3=8B；
        // 64-D1 fallthrough=48 / D2,D3=48。
        if (!g_bIsWin64)
        {
            if (siteIndex == 0)
            {
                if (LoadFromAddress(jccAddr + view_as<Address>(6), NumberType_Int8) != 0xD9
                    || LoadFromAddress(jccAddr + view_as<Address>(7), NumberType_Int8) != 0x05)
                {
                    LogError("[BotRouteFix] [Module 12] Site 0 fallthrough anchor mismatch, aborted!");
                    return false;
                }
            }
            else if (siteIndex == 3)
            {
                if (LoadFromAddress(jccAddr + view_as<Address>(2), NumberType_Int8) != 0x8B)
                {
                    LogError("[BotRouteFix] [Module 12] Site 3 fallthrough anchor mismatch, aborted!");
                    return false;
                }
            }
            else
            {
                if (LoadFromAddress(jccAddr + view_as<Address>(6), NumberType_Int8) != 0x8B)
                {
                    LogError("[BotRouteFix] [Module 12] Site %d fallthrough anchor mismatch, aborted!", siteIndex);
                    return false;
                }
            }
        }
        else if (siteIndex >= 3)
        {
            int anchorOff = (siteIndex == 3) ? 2 : 6;
            if (LoadFromAddress(jccAddr + view_as<Address>(anchorOff), NumberType_Int8) != 0x48)
            {
                LogError("[BotRouteFix] [Module 12] Site %d fallthrough anchor mismatch, aborted!", siteIndex);
                return false;
            }
        }

        // fallthrough 即 camp-inline 的站点（O1/D1/D2/D3 全是），签名本身已含 camp 字节，自锚定
        g_PatchCamp12_Sites[siteIndex].Init(patchName, jccAddr, patchSize);
        return g_PatchCamp12_Sites[siteIndex].NopOut();
    }
    else
    {
        // E9 转换路径：断言 ja/jb-far（32: 0F87；64: 0F82），读原 rel，跳目标必须确为 camp tail
        int expect1 = g_bIsWin64 ? 0x82 : 0x87;
        if (b0 != 0x0F || b1 != expect1)
        {
            LogError("[BotRouteFix] [Module 12] Site %d opcode mismatch (expected 0F %02X, got %02X %02X), aborted!",
                siteIndex, expect1, b0, b1);
            return false;
        }

        int origRel = LoadFromAddress(jccAddr + view_as<Address>(2), NumberType_Int32);
        Address campTail = jccAddr + view_as<Address>(6 + origRel);

        // 语义锚点：camp tail 首指令（32: fld30=D9 05；64: task19=C7 87…13）
        if (g_bIsWin64)
        {
            if (LoadFromAddress(campTail, NumberType_Int8) != 0xC7
                || LoadFromAddress(campTail + view_as<Address>(1), NumberType_Int8) != 0x87)
            {
                LogError("[BotRouteFix] [Module 12] Site %d camp-tail anchor mismatch, aborted (not inverting blind)!", siteIndex);
                return false;
            }
        }
        else
        {
            if (LoadFromAddress(campTail, NumberType_Int8) != 0xD9
                || LoadFromAddress(campTail + view_as<Address>(1), NumberType_Int8) != 0x05)
            {
                LogError("[BotRouteFix] [Module 12] Site %d camp-tail anchor mismatch, aborted (not inverting blind)!", siteIndex);
                return false;
            }
        }

        // E9 与原 jcc 同起点：E9(5B) rel' = origRel + 1；末字节 NOP 对齐 6B
        g_PatchCamp12_Sites[siteIndex].Init(patchName, jccAddr, 6);
        int newRel = origRel + 1;
        int jmpBytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
        for (int b = 0; b < 4; b++)
        {
            jmpBytes[1 + b] = (newRel >> (b * 8)) & 0xFF;
        }
        return g_PatchCamp12_Sites[siteIndex].Apply(jmpBytes);
    }
}

//========================================================================================
// CAMP GATE：挂在 HideState::OnEnter post（模块 15 同钩），task 19 即重骰
// 返回 true = 已重定向去 Hunt（调用方跳过 M15 缩放）；false = 放行
//========================================================================================

bool Module12_CampGate(int client)
{
    if (!g_bModule12_Armed || g_hSDKCall_Hunt == null)
    {
        return false;
    }

    if (!IsBot(client, true))
    {
        return false;
    }

    if (g_iOffset_Task12 <= 0 || g_iOffset_Disp12 <= 0)
    {
        return false;
    }

    // 只有 camp 分支会以 task 19 进 Hide，其余 Hide 原样放行
    int task = GetEntData(client, g_iOffset_Task12, 4);
    if (task != BOT_TASK_MOVE_TO_SNIPER_SPOT)
    {
        return false;
    }

    int morale = GetBotMorale(client);
    float chance = CalculateCampChance(morale);
    if (GetRandomFloat(0.0, 100.0) < chance)
    {
        return false; // 留守：Hide 照常进行
    }

    // 前压：只递延，绝不同步切状态（见头部铁律）；计数挪到帧回调里做
    RequestFrame(Module12_FrameHunt, GetClientUserId(client));
    // DebugLog("[BotRouteFix] [Module 12] Bot %N camp dice failed (morale %d, %.0f%%), Hunt deferred", client, morale, chance);
    return true;
}

/**
 * 递延一帧转 Hunt（帧外切状态才安全）+ disposition 双保险
 */
public void Module12_FrameHunt(any data)
{
    int client = GetClientOfUserId(view_as<int>(data));
    if (!IsBot(client, true))
    {
        return;
    }

    BotHunt(client);
    if (g_iOffset_Disp12 > 0)
    {
        SetEntData(client, g_iOffset_Disp12, 0, 4, true); // ENGAGE_AND_INVESTIGATE
    }
    g_iTotalCampRedirected++;
}
