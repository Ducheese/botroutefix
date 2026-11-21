#if defined _botroutefix_module12_snipercamp_included
    #endinput
#endif
#define _botroutefix_module12_snipercamp_included

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
//
// 事项契约:
//   - 唯一事项 = camp 门禁（ITEM_M12_SNIPERCAMP）；六处骰子旁路是该事项的后端链，全成或全败。
//   - 事实上的 Hook 宿主归 M15；本模块借用其 OnEnter Post，不单独注册 Detour。
//   - 本模块不写代码段以外的对局状态；重定向计数只在事项武装时累加。
//   - 名册优先（与 M9 两手抓）：task 19 的 Hide 若命中 M9 抢包名册，跳过士气重骰，
//     直接走递延 Hunt 通道——M9 那一脚是一次性的，Idle 狙击手露营掷骰随时能把人
//     再拐回 task 19，这边不拒收就前功尽弃；M9 无名册时本条零行为变化。
//========================================================================================

//========================================================================================
// MODULE 12 DEFINES & GLOBALS
//========================================================================================

// 狙击手留守蹲点概率公式宏定义 (Chance = BASE - STEP * Morale)
#define SNIPERCAMP_CHANCE_BASE        60.0    // 基础留守概率 (士气 0 时 60%)
#define SNIPERCAMP_CHANCE_STEP        10.0    // 每级士气增减步长 (百分比)
#define SNIPERCAMP_CHANCE_MIN         0.0     // 留守概率下限
#define SNIPERCAMP_CHANCE_MAX         100.0   // 留守概率上限
static MemoryPatch g_PatchSniperCampSites[6];
static int g_SniperCampPatchBytes[6][6];    // 预先构造的旁路字节（Prep 构造 / Apply 原样写入）
// Task/Disposition 上收 global.inc 共享（OnPluginStart 一次解析），本文件只用不存
static bool g_bModule12_Active = false; // 模块规则开启态（Enable 幂等缓存）
static int g_iTotalSniperCampRedirected = 0;

// 站点表：0=O1 1=O2 2=O3 3=D1 4=D2 5=D3；O2/O3 为 ja/jb-to-camp，需 E9 转换，其余 NOP
static const char g_sSniperCamp12_SigNames[6][32] =
{
    "SniperCamp12_O1", "SniperCamp12_O2", "SniperCamp12_O3",
    "SniperCamp12_D1", "SniperCamp12_D2", "SniperCamp12_D3"
};
static const char g_sSniperCamp12_OffNames[6][32] =
{
    "SniperCamp12_O1_Jcc", "SniperCamp12_O2_Jcc", "SniperCamp12_O3_Jcc",
    "SniperCamp12_D1_Jcc", "SniperCamp12_D2_Jcc", "SniperCamp12_D3_Jcc"
};

//========================================================================================
// MODULE 12 ITEM BACKENDS (called by Init)
//========================================================================================

/**
 * 事项后端：camp 门禁（ITEM_M12_SNIPERCAMP）
 *
 * 校验 Hunt SDKCall、task/disposition 偏移，并逐站点定位/校验/备份/构造旁路字节；
 * 不写入任何代码段（写入由 Apply 执行）。任一后端失败返回 false，Init 标记事项 FAILED。
 *
 * @return              true = 六站点与门禁偏移全部就绪
 */
static bool PrepModule12_SniperCampBackend()
{
    if (g_hGameConf == null)
    {
        LogError("[BotRouteFix] [Module 12] Gamedata missing; camp item FAILED.");
        return false;
    }

    if (g_iOffset_Morale <= 0)
    {
        LogError("[BotRouteFix] [Module 12] Morale offset missing; camp item FAILED.");
        return false;
    }

    // Task/Disposition 由 OnPluginStart 统一解析，此处只做门禁
    if (g_iOffset_Task <= 0 || g_iOffset_Disposition <= 0)
    {
        LogError("[BotRouteFix] [Module 12] Task/Disposition offsets missing; camp item FAILED.");
        return false;
    }

    if (g_hSDKCall_Hunt == null)
    {
        LogError("[BotRouteFix] [Module 12] Hunt SDKCall missing; camp item FAILED (native dice untouched).");
        return false;
    }

    LogMessage("[BotRouteFix] [Module 12] Sniper camp offsets verified (task=%d, disp=%d, hunt SDKCall).",
        g_iOffset_Task, g_iOffset_Disposition);

    for (int i = 0; i < 6; i++)
    {
        if (!PrepModule12_Site(i))
        {
            return false;
        }
    }

    return true;
}

/**
 * 事项 M12 camp 门禁的单站点后端准备
 *
 * O1/D1/D2/D3 = NOP（指向 skip）；O2/O3 = E9 转换（指向 camp）。
 * 只定位/校验/备份并把构造好的字节写入 g_SniperCampPatchBytes，不写代码段。
 * 失败语义：任一步失败返回 false，整个 camp 事项 FAILED（六站点全成或全败）。
 *
 * @param siteIndex     0..5
 * @return              true = 该站点后端就绪
 */
static bool PrepModule12_Site(int siteIndex)
{
    Address sigAddr = GameConfGetAddress(g_hGameConf, g_sSniperCamp12_SigNames[siteIndex]);
    if (sigAddr == Address_Null)
    {
        LogError("[BotRouteFix] [Module 12] Failed to locate signature for %s!", g_sSniperCamp12_SigNames[siteIndex]);
        return false;
    }

    int jccOffset = GameConfGetOffset(g_hGameConf, g_sSniperCamp12_OffNames[siteIndex]);
    if (jccOffset == -1)
    {
        LogError("[BotRouteFix] [Module 12] Failed to read %s!", g_sSniperCamp12_OffNames[siteIndex]);
        return false;
    }

    Address jccAddr = sigAddr + view_as<Address>(jccOffset);
    bool isConvert = (siteIndex == 1 || siteIndex == 2); // O2/O3：ja/jb-to-camp 改 E9

    char patchName[32];
    Format(patchName, sizeof(patchName), "SniperCamp_S%d", siteIndex);

    // 站点固定宽度：O1/D2/D3 = 6 字节远跳；D1 = 2 字节短跳（两种架构一致）
    int patchSize = (siteIndex == 3) ? 2 : 6;

    int b0 = LoadFromAddress(jccAddr, NumberType_Int8);
    int b1 = LoadFromAddress(jccAddr + view_as<Address>(1), NumberType_Int8);

    // Init 必须在幂等检测之前：同目标重入时它保留原始基线备份
    g_PatchSniperCampSites[siteIndex].Init(patchName, jccAddr, patchSize);

    // 幂等性检测必须先于 opcode 断言（已旁路的字节不再是原助记符）；
    // 有备份=自家补丁，保持基线；无备份=外来已旁路，记 false（无基线可还原）
    bool alreadyBypassed = (!isConvert && b0 == 0x90) || (isConvert && b0 == 0xE9);
    if (alreadyBypassed)
    {
        if (!g_PatchSniperCampSites[siteIndex].hasBackup)
        {
            g_PatchSniperCampSites[siteIndex].isPatched = false;
            LogError("[BotRouteFix] [Module 12] Site %d already bypassed without baseline @ %X; camp item FAILED.",
                siteIndex, jccAddr);
            return false;
        }
        LogMessage("[BotRouteFix] [Module 12] Site %d already bypassed @ %X, skipping", siteIndex, jccAddr);
    }
    else
    {
        if (!PrepModule12_ValidateSite(siteIndex, isConvert, jccAddr, b0, b1))
        {
            return false;
        }

        if (!g_PatchSniperCampSites[siteIndex].BackupOriginal())
        {
            LogError("[BotRouteFix] [Module 12] Site %d baseline backup failed; camp item FAILED.", siteIndex);
            return false;
        }
    }

    // 从备份构造旁路字节：NOP 或 E9 转换（E9 的 rel 由原始 jcc 的 rel + 1 推导）
    if (!isConvert)
    {
        for (int i = 0; i < patchSize; i++)
        {
            g_SniperCampPatchBytes[siteIndex][i] = 0x90;
        }
    }
    else
    {
        int origRel = g_PatchSniperCampSites[siteIndex].originalBytes[2]
            | (g_PatchSniperCampSites[siteIndex].originalBytes[3] << 8)
            | (g_PatchSniperCampSites[siteIndex].originalBytes[4] << 16)
            | (g_PatchSniperCampSites[siteIndex].originalBytes[5] << 24);
        int newRel = origRel + 1;

        g_SniperCampPatchBytes[siteIndex][0] = 0xE9;
        for (int b = 0; b < 4; b++)
        {
            g_SniperCampPatchBytes[siteIndex][1 + b] = (newRel >> (b * 8)) & 0xFF;
        }
        g_SniperCampPatchBytes[siteIndex][5] = 0x90;
    }
    LogMessage("[BotRouteFix] [Module 12] Site %d dice bypass verified @ %X", siteIndex, jccAddr);
    return true;
}

/**
 * 事项 M12 camp 门禁的单站点原始指令校验（只在首次打补丁时执行）
 *
 * 失败语义：模板或语义锚点不匹配返回 false，整个 camp 事项 FAILED，不写入任何字节。
 *
 * @param siteIndex     0..5
 * @param isConvert     true = O2/O3 的 E9 转换路径
 * @param jccAddr       跳转指令地址
 * @param b0            jcc 首字节
 * @param b1            jcc 次字节
 * @return              true = 模板与语义锚点全部匹配
 */
static bool PrepModule12_ValidateSite(int siteIndex, bool isConvert, Address jccAddr, int b0, int b1)
{
    if (!isConvert)
    {
        // NOP 路径：断言 jcc 助记符与长度（32: O1=0F86/D1=72/D2,D3=0F82；64: O1=0F83/D1=77/D2,D3=0F87）
        int expect0, expect1 = -1;
        if (g_bIsWin64)
        {
            if (siteIndex == 0)      { expect0 = 0x0F; expect1 = 0x83; }
            else if (siteIndex == 3) { expect0 = 0x77; }
            else                     { expect0 = 0x0F; expect1 = 0x87; }
        }
        else
        {
            if (siteIndex == 0)      { expect0 = 0x0F; expect1 = 0x86; }
            else if (siteIndex == 3) { expect0 = 0x72; }
            else                     { expect0 = 0x0F; expect1 = 0x82; }
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

        return true;
    }

    // E9 转换路径：断言 ja/jb-far（32: 0F87；64: 0F82），读原 rel，跳目标必须确为 camp tail
    int expect1 = g_bIsWin64 ? 0x82 : 0x87;
    if (b0 != 0x0F || b1 != expect1)
    {
        LogError("[BotRouteFix] [Module 12] Site %d opcode mismatch (expected 0F %02X, got %02X %02X), aborted!",
            siteIndex, expect1, b0, b1);
        return false;
    }

    int origRel = LoadFromAddress(jccAddr + view_as<Address>(2), NumberType_Int32);
    Address sniperCampTail = jccAddr + view_as<Address>(6 + origRel);

    // 语义锚点：camp tail 首指令（32: fld30=D9 05；64: task19=C7 87…13）
    if (g_bIsWin64)
    {
        if (LoadFromAddress(sniperCampTail, NumberType_Int8) != 0xC7
            || LoadFromAddress(sniperCampTail + view_as<Address>(1), NumberType_Int8) != 0x87)
        {
            LogError("[BotRouteFix] [Module 12] Site %d camp-tail anchor mismatch, aborted (not inverting blind)!", siteIndex);
            return false;
        }
    }
    else
    {
        if (LoadFromAddress(sniperCampTail, NumberType_Int8) != 0xD9
            || LoadFromAddress(sniperCampTail + view_as<Address>(1), NumberType_Int8) != 0x05)
        {
            LogError("[BotRouteFix] [Module 12] Site %d camp-tail anchor mismatch, aborted (not inverting blind)!", siteIndex);
            return false;
        }
    }

    return true;
}

//========================================================================================
// MODULE 12 LIFECYCLE
//========================================================================================

/**
 * 初始化模块 12（六站点全成或全败）
 *
 * @return              true = 事项已武装
 */
bool InitModule12_SniperCamp()
{
    if (g_hGameConf == null)
    {
        Item_SetReady(ITEM_M12_SNIPERCAMP, false);
        LogError("[BotRouteFix] [Module 12] Gamedata missing; camp item FAILED.");
        return false;
    }

    // 宿主先行断言：HideState::OnEnter 钩子归 M15 所有（sp 初始化顺序 15 → 12 不可调），
    // M12 只借用；宿主柄缺失说明宿主 Prep 没过，本事项直接 FAILED，不进 Prep 做无用功。
    // 宿主柄是 M15 私有 static，经 Module15_IsHideHostReady 探针查询，不直读。
    if (!Module15_IsHideHostReady())
    {
        Item_SetReady(ITEM_M12_SNIPERCAMP, false);
        LogError("[BotRouteFix] [Module 12] Hide host detour missing; camp item FAILED.");
        return false;
    }

    Item_SetReady(ITEM_M12_SNIPERCAMP, PrepModule12_SniperCampBackend());
    EnableModule12_SniperCamp(true);
    if (!Item_IsArmed(ITEM_M12_SNIPERCAMP))
    {
        LogModuleAllFailed(12);
        return false;
    }

    LogModuleArmed(12, "Sniper camp", "1");
    return true;
}

/**
 * 动态启用或停用模块 12（幂等；停用即写回六处 jcc 并解除武装）
 *
 * @param enable        true = 按事项后端就绪情况武装；false = 还原并解除武装
 */
void EnableModule12_SniperCamp(bool enable)
{
    if (enable == g_bModule12_Active)
    {
        return;
    }

    if (!enable)
    {
        RestoreModule12_SniperCampBackend();
        g_bModule12_Active = false;
        // 借钩宿主：模块 15 若仍在工作，钩子必须保持启用
        SyncHideDurationDetourState();
        return;
    }

    bool armed = ApplyModule12_SniperCampBackend();
    g_bModule12_Active = armed;
    // 借钩宿主：本模块自身无 Detour，须确保模块 15 的 OnEnter 钩子处于启用态
    SyncHideDurationDetourState();
}

/**
 * 还原模块 12（插件卸载：写回六处 jcc 并解除武装；钩子本身归模块 15 管）
 */
void RestoreModule12_SniperCamp()
{
    EnableModule12_SniperCamp(false);
    LogMessage("[BotRouteFix] [Module 12] Sniper camp patches safely restored to original bytes.");
}

//========================================================================================
// MODULE 12 PUBLIC GETTERS, TELEMETRY & FORMULA
//========================================================================================

/**
 * 累积重定向次数（骰子未过转 Hunt；只在事项武装时累加，自加载起单调递增）
 */
int Module12_GetTotalSniperCampRedirected()
{
    return g_iTotalSniperCampRedirected;
}

/**
 * 事项 M12 camp 门禁的留守概率公式：Chance = 60 - 10 * Morale
 *
 * 纯计算，无失败分支；结果钳制在 [0, 100]。
 *
 * @param morale        Bot 士气数值 (-3 ~ +3)
 * @return              留守概率 (0.0 ~ 100.0)
 */
float Module12_CalculateSniperCampChance(int morale)
{
    float chance = SNIPERCAMP_CHANCE_BASE - SNIPERCAMP_CHANCE_STEP * float(morale);
    if (chance > SNIPERCAMP_CHANCE_MAX)
    {
        chance = SNIPERCAMP_CHANCE_MAX;
    }
    if (chance < SNIPERCAMP_CHANCE_MIN)
    {
        chance = SNIPERCAMP_CHANCE_MIN;
    }
    return chance;
}

//========================================================================================
// MODULE 12 DETOUR HOOK ENTRY (BORROWED FROM MODULE 15)
//========================================================================================

/**
 * camp 门禁：挂在 HideState::OnEnter post（模块 15 同钩），task 19 即重骰
 *
 * 调用方（模块 15 宿主）只在需要时调用；本函数自行把关事项武装态。
 *
 * @return              true = 已重定向去 Hunt（调用方跳过 M15 缩放）；false = 放行
 */
bool Module12_SniperCamp(int client)
{
    if (!Item_IsArmed(ITEM_M12_SNIPERCAMP))
    {
        return false;
    }

    if (!IsBot(client, true))
    {
        return false;
    }

    if (g_iOffset_Task <= 0 || g_iOffset_Disposition <= 0)
    {
        return false;
    }

    // 只有 camp 分支会以 task 19 进 Hide，其余 Hide 原样放行
    int task = GetEntData(client, g_iOffset_Task, 4);
    if (task != BOT_TASK_MOVE_TO_SNIPER_SPOT)
    {
        return false;
    }

    // 名册优先（两手抓的第二手）：M9 抢包名册里的人正在捡包（或正被 M9 解蹲），
    // 绝不能留守——走与骰子未过相同的递延 Hunt 通道。M9 无名册时 IsInRoster 恒 false，
    // 本分支零行为变化，不需要在 Prep 里加耦合。
    if (Module9_IsInRoster(client))
    {
        RequestFrame(Module12_FrameHunt, GetClientUserId(client));
        DebugLog("[BotRouteFix] [Module 12] Bot %N in M9 fetch roster, camp bypassed, Hunt deferred", client);
        return true;
    }

    int morale = GetBotMorale(client);
    float chance = Module12_CalculateSniperCampChance(morale);
    if (GetRandomFloat(0.0, 100.0) < chance)
    {
        return false; // 留守：Hide 照常进行
    }

    // 前压：只递延，绝不同步切状态（见头部铁律）；计数挪到帧回调里做
    RequestFrame(Module12_FrameHunt, GetClientUserId(client));
    DebugLog("[BotRouteFix] [Module 12] Bot %N camp dice failed (morale %s%d, %.0f%%), Hunt deferred", client, (morale >= 0) ? "+" : "", morale, chance);
    return true;
}

/**
 * 递延一帧转 Hunt（帧外切状态才安全）+ disposition 双保险
 */
public void Module12_FrameHunt(any data)
{
    // 递延帧可能在停用之后才到，必须二次校验事项武装态
    if (!Item_IsArmed(ITEM_M12_SNIPERCAMP))
    {
        return;
    }

    int client = GetClientOfUserId(view_as<int>(data));
    if (!IsBot(client, true))
    {
        return;
    }

    BotHunt(client);
    if (g_iOffset_Disposition > 0)
    {
        SetEntData(client, g_iOffset_Disposition, 0, 4, true); // ENGAGE_AND_INVESTIGATE
    }
    DebugLog("[BotRouteFix] [Module 12] Bot %N camp redirect executed (deferred Hunt now)", client);
    g_iTotalSniperCampRedirected++;
}

//========================================================================================
// MODULE 12 ITEM APPLY / RESTORE BACKENDS
//========================================================================================

/**
 * 应用 camp 门禁事项：六站点全成或全败
 *
 * 物理写入失败证伪了 Prep 的模板校验，必须同步清 Ready 并回滚已写站点。
 *
 * @return              true = 六处旁路全部驻留
 */
static bool ApplyModule12_SniperCampBackend()
{
    if (!Item_IsReady(ITEM_M12_SNIPERCAMP))
    {
        Item_SetArmed(ITEM_M12_SNIPERCAMP, false);
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        if (!g_PatchSniperCampSites[i].Apply(g_SniperCampPatchBytes[i]))
        {
            for (int j = 0; j < 6; j++)
            {
                g_PatchSniperCampSites[j].Restore();
            }
            Item_SetReady(ITEM_M12_SNIPERCAMP, false);
            Item_SetArmed(ITEM_M12_SNIPERCAMP, false);
            LogError("[BotRouteFix] [Module 12] Site %d write failed; all bypasses rolled back, camp item FAILED.", i);
            return false;
        }
    }

    // 借钩宿主必须在线：没有 OnEnter 钩子，六处骰子已旁路却无人重骰，
    // 会退化成"狙击手必 camp"的错误行为，必须回滚。
    // 先临时置位再同步：宿主开关按 M12 Armed 判定借钩需求，不置位会形成循环依赖。
    Item_SetArmed(ITEM_M12_SNIPERCAMP, true);
    if (!SyncHideDurationDetourState())
    {
        for (int j = 0; j < 6; j++)
        {
            g_PatchSniperCampSites[j].Restore();
        }
        // 宿主缺失属挂钩类失败：只清 Armed，保留 Ready 供下次 Enable 重试
        Item_SetArmed(ITEM_M12_SNIPERCAMP, false);
        LogError("[BotRouteFix] [Module 12] Hook host unavailable; dice bypasses rolled back, camp item unarmed.");
        return false;
    }

    return true;
}

/**
 * 还原 camp 门禁事项：写回六处原始 jcc 并解除武装
 */
static void RestoreModule12_SniperCampBackend()
{
    for (int i = 0; i < 6; i++)
    {
        g_PatchSniperCampSites[i].Restore();
    }
    Item_SetArmed(ITEM_M12_SNIPERCAMP, false);
}


