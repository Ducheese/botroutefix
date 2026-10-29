#if defined _botroutefix_module_manager_included
    #endinput
#endif
#define _botroutefix_module_manager_included

//========================================================================================
// MODULE RULES / 模块规则调度系统
//
// 编码规则：
//   - "all" / "*" / "1-15"        : 1~15 全开 (默认基线)
//   - "none" / "off" / "0" / ""   : 1~15 全关
//   - 排除模式 (以 '-' 开头项)    : "all, -5, -13" 或 "-5, -13" (以全开为基线，排除指定模块)
//   - 指定模式 (纯正整数或区间)   : "1, 2, 5" 或 "1-4, 7, 9" (以全关为基线，仅开启指定模块)
//   - 支持逗号、分号、空格混合分隔，区间支持 '-' 或 '~'
//========================================================================================

#define TOTAL_TACTICAL_MODULES      15

// 全局 ConVar 句柄与当前激活状态缓存 (索引 1..15 对应模块 1..15)
ConVar g_cvModules = null;
// 注意：此数组仅为规则意图缓存（仅记录 ConVar 规则期望该模块开启或关闭，供遥测标注区分 "disabled by rule" 与 "activation failed"），
// 严禁作为各模块实际运行时业务判据！真实运行态恒以各模块私有的 g_bModuleX_Active / 运行时函数为唯一真理源。
bool g_bModuleConfigActive[TOTAL_TACTICAL_MODULES + 1];

/**
 * 查询指定模块是否被模块规则启用
 *
 * @param modIdx        模块编号 (1..15)
 * @return              true 为规则启用，false 为规则禁用
 */
bool IsModuleEnabledByRule(int modIdx)
{
    if (modIdx < 1 || modIdx > TOTAL_TACTICAL_MODULES)
    {
        return false;
    }
    return g_bModuleConfigActive[modIdx];
}

/**
 * 严格纯数字校验（拒绝 "abc"、"5abc"、"1a" 等弱类型穿透）
 *
 * StringToInt 的行为等价于 strtol：遇到首个非数字即停止并返回已解析部分，
 * 无数字时返回 0。若不先做纯数字断言，"abc-5" 会被解成区间 [0..5] 并开启 1~5 号。
 *
 * @param s             待校验字符串
 * @param from          起始下标（用于跳过已剥离的前导负号）
 * @return              从 from 起是否全部为 0~9 且非空
 */
static bool IsDigitsOnly(const char[] s, int from = 0)
{
    if (s[from] == '\0')
    {
        return false;
    }
    for (int i = from; s[i] != '\0'; i++)
    {
        if (s[i] < '0' || s[i] > '9')
        {
            return false;
        }
    }
    return true;
}

/**
 * 归一化「符号与数字被空格隔开」的写法： "- 5" -> "-5"，"1- 4" -> "1-4"
 *
 * 分词器按空格切分，若原样保留 "- 5"，会被拆成 "-" 与 "5"；
 * "-" 被 ParseTokenRange 拒绝后，"5" 变成正向白名单项，语义从「关闭 5」反转成「仅开启 5」。
 *
 * 折叠规则：
 *   - 符号【后】的空白：后面紧跟数字才吞掉（"- 5" -> "-5"）；
 *   - 符号【前】的空白：仅当左侧紧邻的是数字才吞掉（"1 - 4" -> "1-4"）。
 *   第二条限定保证了 "all -5" 不受影响——"all" 不是数字，空白保留，
 *   仍是 "all" 与 "-5" 两个 Token，不会粘成非法 Token "all-5"。
 *
 * @param src           原始规则字符串
 * @param dst           归一化输出缓冲
 * @param maxlen        输出缓冲长度
 */
static void NormalizeSignSpace(const char[] src, char[] dst, int maxlen)
{
    int j = 0;
    for (int i = 0; src[i] != '\0' && j < maxlen - 1; i++)
    {
        char c = src[i];

        if (c == '-' || c == '~')
        {
            // 前向探测：符号后（跳过空白）是否为数字
            int k = i + 1;
            while (src[k] == ' ' || src[k] == '\t')
            {
                k++;
            }
            bool hasForwardDigit = (src[k] >= '0' && src[k] <= '9');

            // 反向探测：已写入部分最后一个非空白字符是否为数字
            int t = j - 1;
            while (t >= 0 && (dst[t] == ' ' || dst[t] == '\t'))
            {
                t--;
            }
            bool hasBackwardDigit = (t >= 0 && dst[t] >= '0' && dst[t] <= '9');

            if (hasBackwardDigit)
            {
                // 去掉符号前紧邻的空白，使 "1 - 4" 归一为 "1-4"
                while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\t'))
                {
                    j--;
                }
            }

            dst[j++] = c;

            if (hasForwardDigit)
            {
                i = k - 1;   // 跳过符号后的空白，循环 i++ 后正好落在数字上
            }
            continue;
        }

        dst[j++] = c;
    }
    dst[j] = '\0';
}

/**
 * 提取 Token 中的数字区间 (例如 "5", "-5", "1-4", "-5-8")
 *
 * @param token         输入 Token 字符串
 * @param isNegative    输出是否为负号排除项
 * @param modStart      输出起始模块编号
 * @param modEnd        输出结束模块编号
 * @return              是否成功解析出有效数字
 */
static bool ParseTokenRange(const char[] token, bool &isNegative, int &modStart, int &modEnd)
{
    if (token[0] == '\0')
    {
        return false;
    }

    int startIdx = 0;
    if (token[0] == '-')
    {
        isNegative = true;
        startIdx = 1;
    }
    else
    {
        isNegative = false;
        startIdx = 0;
    }

    if (token[startIdx] == '\0')
    {
        return false;
    }

    // 检索范围连接符 ('-' 或 '~')
    int dashPos = -1;
    int len = strlen(token);
    for (int i = startIdx; i < len; i++)
    {
        if (token[i] == '-' || token[i] == '~')
        {
            dashPos = i;
            break;
        }
    }

    if (dashPos != -1)
    {
        if (token[dashPos + 1] == '\0')
        {
            // 连字符在末尾（例如 "1-" 或 "5~"），属于残缺未完成 Token
            return false;
        }

        char sStart[16], sEnd[16];
        int partLen = dashPos - startIdx;
        if (partLen <= 0 || partLen >= sizeof(sStart))
        {
            return false;
        }
        strcopy(sStart, partLen + 1, token[startIdx]);
        strcopy(sEnd, sizeof(sEnd), token[dashPos + 1]);

        // 纯数字断言：拒绝 "abc-5" / "5-abc" 这类被 strtol 弱类型穿透的写法
        if (!IsDigitsOnly(sStart) || !IsDigitsOnly(sEnd))
        {
            return false;
        }

        modStart = StringToInt(sStart);
        modEnd = StringToInt(sEnd);
    }
    else
    {
        // 纯数字断言：拒绝 "5abc" / "abc" 等（含前导负号时从 startIdx 起校验）
        if (!IsDigitsOnly(token, startIdx))
        {
            return false;
        }

        modStart = StringToInt(token[startIdx]);
        modEnd = modStart;
    }

    // 纠正逆序区间
    if (modStart > modEnd)
    {
        int tmp = modStart;
        modStart = modEnd;
        modEnd = tmp;
    }

    return (modEnd > 0);
}

/**
 * 解析模块规则字符串，输出每个模块的目标开关状态 (1..15)
 *
 * @param rawConfig     输入规则字符串 (例如 "all", "all, -5, -13", "1-4, 7, 9", "-5")
 * @param moduleStates  输出 1..15 的布尔数组
 */
void ParseModulesRule(const char[] rawConfig, bool moduleStates[TOTAL_TACTICAL_MODULES + 1])
{
    // 先压紧「符号 + 空格 + 数字」（"- 5" -> "-5"），再分词；
    // 否则分词器会把 "-" 与 "5" 拆开，导致语义由「关闭 5」反转为「仅开启 5」。
    char config[128];
    NormalizeSignSpace(rawConfig, config, sizeof(config));
    TrimString(config);

    // 1. 全关快捷词
    if (config[0] == '\0' || StrEqual(config, "none", false) || StrEqual(config, "off", false) || StrEqual(config, "0"))
    {
        for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
        {
            moduleStates[i] = false;
        }
        return;
    }

    // 2. 全开快捷词
    if (StrEqual(config, "all", false) || StrEqual(config, "*"))
    {
        for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
        {
            moduleStates[i] = true;
        }
        return;
    }

    // 3. 第一趟探测 (Pass 1)：分析 Token 构成，科学裁决基线模式
    // 逻辑契约：
    // a) 显式包含 "all" / "*" -> 排除模式（基线全开 true）；
    // b) 显式包含正向白名单项（如 "1-5, -3" 或 "1, 2"）-> 指定模式（基线全关 false，仅打开指定项并排除负号项）；
    // c) 纯负号排除项（如 "-5, -13"）-> 排除模式（基线全开 true，排除指定项）。
    bool hasAllToken = false;
    bool hasNoneToken = false;
    bool hasPositiveToken = false;
    bool hasNegativeToken = false;
    int len = strlen(config);
    char token[32];
    int tokenIdx = 0;

    for (int i = 0; i <= len; i++)
    {
        char c = config[i];
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\0')
        {
            if (tokenIdx > 0)
            {
                token[tokenIdx] = '\0';
                bool isNeg = false;
                int s = 0, e = 0;
                if (StrEqual(token, "all", false) || StrEqual(token, "*"))
                {
                    hasAllToken = true;
                }
                else if (StrEqual(token, "none", false) || StrEqual(token, "off", false) || StrEqual(token, "0"))
                {
                    hasNoneToken = true;
                }
                else if (ParseTokenRange(token, isNeg, s, e))
                {
                    // 只有与 [1..TOTAL_TACTICAL_MODULES] 存在交集才算有效模块 Token。
                    // 否则 "99" / "20-30" 会被误认为合法正向配置绕过硬回退，
                    // 基线按白名单定为全关，而 Pass 2 又因越界跳过 —— 15 个模块被静默全灭。
                    if (e >= 1 && s <= TOTAL_TACTICAL_MODULES)
                    {
                        if (isNeg)
                        {
                            hasNegativeToken = true;
                        }
                        else
                        {
                            hasPositiveToken = true;
                        }
                    }
                }
                tokenIdx = 0;
            }
        }
        else
        {
            if (tokenIdx < sizeof(token) - 1)
            {
                token[tokenIdx++] = c;
            }
        }
    }

    // 无法识别任何有效 Token（如拼写错误 "al", "abc", "-", "1-" 等）
    // 为防止误将全部 15 个战术模块静默关闭，打印 LogError 告警并安全回退至全开 "all"
    if (!hasAllToken && !hasNoneToken && !hasPositiveToken && !hasNegativeToken)
    {
        LogError("[BotRouteFix] [ModuleRules] Unrecognized module rule '%s', falling back to 'all'.", config);
        for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
        {
            moduleStates[i] = true;
        }
        return;
    }

    bool isBaselineActive = false;
    if (hasAllToken)
    {
        isBaselineActive = true;
    }
    else if (hasPositiveToken)
    {
        isBaselineActive = false;
    }
    else if (hasNegativeToken)
    {
        isBaselineActive = true;
    }

    // 初始化基线
    for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
    {
        moduleStates[i] = isBaselineActive;
    }

    // 4. 第二趟处理 (Pass 2)：执行每个 Token 的开闭修饰
    tokenIdx = 0;
    for (int i = 0; i <= len; i++)
    {
        char c = config[i];
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\0')
        {
            if (tokenIdx > 0)
            {
                token[tokenIdx] = '\0';

                if (StrEqual(token, "all", false) || StrEqual(token, "*"))
                {
                    for (int m = 1; m <= TOTAL_TACTICAL_MODULES; m++)
                    {
                        moduleStates[m] = true;
                    }
                }
                else if (StrEqual(token, "none", false) || StrEqual(token, "off", false) || StrEqual(token, "0"))
                {
                    for (int m = 1; m <= TOTAL_TACTICAL_MODULES; m++)
                    {
                        moduleStates[m] = false;
                    }
                }
                else
                {
                    bool isNegative = false;
                    int modStart = 0;
                    int modEnd = 0;

                    if (ParseTokenRange(token, isNegative, modStart, modEnd))
                    {
                        if (modEnd < 1 || modStart > TOTAL_TACTICAL_MODULES)
                        {
                            LogMessage("[BotRouteFix] [ModuleRules] Warning: token '%s' references out-of-range module (1..%d), skipping.",
                                token, TOTAL_TACTICAL_MODULES);
                        }
                        else
                        {
                            // 容错 Clamp 截断至有效模块区间 [1..TOTAL_TACTICAL_MODULES]
                            int clampStart = (modStart < 1) ? 1 : modStart;
                            int clampEnd   = (modEnd > TOTAL_TACTICAL_MODULES) ? TOTAL_TACTICAL_MODULES : modEnd;

                            bool targetState = !isNegative;
                            for (int m = clampStart; m <= clampEnd; m++)
                            {
                                moduleStates[m] = targetState;
                            }
                        }
                    }
                }

                tokenIdx = 0;
            }
        }
        else
        {
            if (tokenIdx < sizeof(token) - 1)
            {
                token[tokenIdx++] = c;
            }
        }
    }
}

/**
 * 刷新并分发应用当前所有模块的开启/关闭状态
 */
void ApplyModulesConfig()
{
    char rule[128];
    if (g_cvModules != null)
    {
        g_cvModules.GetString(rule, sizeof(rule));
    }
    else
    {
        strcopy(rule, sizeof(rule), "all");
    }

    bool active[TOTAL_TACTICAL_MODULES + 1];
    ParseModulesRule(rule, active);

    // 缓存目标状态
    for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
    {
        g_bModuleConfigActive[i] = active[i];
    }

    // 日志去重：启动时 OnPluginStart 与 OnConfigsExecuted 会连续调用同一份规则，
    // 仅在规则真正变化时打印三行摘要，避免换图时重复刷屏。
    // 用显式标志位而非字符串初值判"首次"，否则规则被显式设为空串（语义=全关）时
    // 会因 "" == "" 被误判为"未变化"，导致 15 个模块静默关闭且无任何日志。
    static char s_lastRule[128];
    static bool s_bRuleLogged = false;

    if (!s_bRuleLogged || !StrEqual(s_lastRule, rule, false))
    {
        s_bRuleLogged = true;
        strcopy(s_lastRule, sizeof(s_lastRule), rule);

        char activeList[128], inactiveList[128];
        activeList[0] = '\0';
        inactiveList[0] = '\0';
        for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
        {
            char num[8];
            Format(num, sizeof(num), "%d ", i);
            if (active[i])
            {
                StrCat(activeList, sizeof(activeList), num);
            }
            else
            {
                StrCat(inactiveList, sizeof(inactiveList), num);
            }
        }

        LogMessage("[BotRouteFix] [ModuleRules] Applying module rule: '%s'", rule);
        LogMessage("[BotRouteFix] [ModuleRules] Active modules  : [ %s]", activeList[0] ? activeList : "NONE ");
        LogMessage("[BotRouteFix] [ModuleRules] Inactive modules: [ %s]", inactiveList[0] ? inactiveList : "NONE ");
    }

    // 逐模块调用底层通断封装
    EnableModule1_Defense(active[1]);
    EnableModule2_Bombsite(active[2]);
    EnableModule3_Danger(active[3]);
    EnableModule4_Hunt(active[4]);
    EnableModule5_Combat(active[5]);
    EnableModule6_TMorale(active[6]);
    EnableModule7_Escort(active[7]);
    EnableModule8_Queue(active[8]);
    EnableModule9_Loot(active[9]);
    EnableModule10_Defuse(active[10]);
    EnableModule11_Sniper(active[11]);
    EnableModule12_CampGate(active[12]);
    EnableModule13_SniperBuy(active[13]);
    EnableModule14_DefendRush(active[14]);
    EnableModule15_HideDuration(active[15]);

    // 协同同步 M12/M15 借钩宿主挂载状态
    SyncHideDurationDetourState();
}

/**
 * 初始化模块规则 ConVar 与初始分发
 */
void InitModuleManager()
{
    g_cvModules = CreateConVar("sm_botroutefix_modules", "all",
        "Configure active tactical modules ('all', 'none', whitelist '1,2,5-8', or blacklist 'all,-5,-13')",
        FCVAR_NOTIFY);

    for (int i = 1; i <= TOTAL_TACTICAL_MODULES; i++)
    {
        g_bModuleConfigActive[i] = true;
    }
}
