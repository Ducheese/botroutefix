#if defined _botroutefix_items_included
    #endinput
#endif
#define _botroutefix_items_included

//========================================================================================
// ITEM CAPABILITY REGISTRY / 事项能力表
//
// 契约（2026-11-02 用户裁定）：
//   事项 = 一个用户可见、不可分割的战术行为；它下面的所有机制与后端都只是条件。
//   任一后端缺失 → 整个事项失败，不允许另一半继续执行。
//   模块只有 >=2 个事项时才可能出现 DEGRADED；1 个事项的模块只可能 ACTIVE / FAILED / INACTIVE。
//
// 状态词：
//   INACTIVE = 模块规则关闭（主动不启用）
//   ACTIVE   = 规则开启，且该模块全部事项就绪并已武装
//   DEGRADED = 规则开启，部分事项武装、部分失败（只有多事项模块可能出现）
//   FAILED   = 规则开启，0 个事项武装
//
// 归属原则：谁执行最终改写，谁拥有事项。
//   - M11 只发撤退请求，最终后撤由 M5 执行 → 撤退归 M5，M11 只有走廊 danger。
//   - M15 亲手写 m_duration → 短蹲与缩放同属 M15 一件事。
//   - M6 亲手写下包 OPPO → 下包 OPPO 归 M6，噪声钩子拦截与事件写入同属 M6 一件事（Detour / 偏移 / SDKCall 缺失即 FAILED）。
//   - M9 亲手写抢包 OPPO → 抢包 OPPO 归 M9；M9 名册选举只是本事项门禁的预热（Detour / 偏移 / SDKCall 缺失即 FAILED）。
//   - M10 的选举只是本模块门禁的预热，不改变引擎行为，不计事项。
//   - GUARD_TICKING_BOMB 谁都不写：T 方守包听到枪声必须能出击，一刀切不合理。
//========================================================================================

#define ITEM_M1_ANTI_RUSH       1
#define ITEM_M1_GUARD           2
#define ITEM_M2_LOCK            3
#define ITEM_M2_CLOCK           4
#define ITEM_M3_DECAY           5
#define ITEM_M4_NOP             6
#define ITEM_M4_HUNT            7
#define ITEM_M5_RELOAD          8
#define ITEM_M5_DODGE           9
#define ITEM_M5_RETREAT         10
#define ITEM_M6_NOISE           11
#define ITEM_M7_ESCORT          12
#define ITEM_M8_QUEUE           13
#define ITEM_M8_DEADLOCK        14
#define ITEM_M9_FETCH           15
#define ITEM_M10_DEFUSE         16
#define ITEM_M11_CORRIDOR       17
#define ITEM_M12_SNIPERCAMP     18
#define ITEM_M13_BUY            19
#define ITEM_M14_DEFEND         20
#define ITEM_M15_HIDE           21
#define ITEM_M16_ESCAPE         22

#define ITEM_COUNT              22
#define ITEM_ARRAY_SIZE         (ITEM_COUNT + 1)

enum ItemModuleState
{
    ItemModule_Inactive = 0,
    ItemModule_Active,
    ItemModule_Degraded,
    ItemModule_Failed
};

static int g_iItemModule[ITEM_ARRAY_SIZE];
static char g_sItemName[ITEM_ARRAY_SIZE][48];
static bool g_bItemReady[ITEM_ARRAY_SIZE];
static bool g_bItemArmed[ITEM_ARRAY_SIZE];

/**
 * 初始化事项表（模块归属与显示名）
 */
void InitItems()
{
    g_iItemModule[ITEM_M1_ANTI_RUSH] = 1;
    strcopy(g_sItemName[ITEM_M1_ANTI_RUSH], 48, "M1 anti-rush NOP");
    g_iItemModule[ITEM_M1_GUARD] = 1;
    strcopy(g_sItemName[ITEM_M1_GUARD], 48, "M1 guard chance formula patch");

    g_iItemModule[ITEM_M2_LOCK] = 2;
    strcopy(g_sItemName[ITEM_M2_LOCK], 48, "M2 bombsite lock patch");
    g_iItemModule[ITEM_M2_CLOCK] = 2;
    strcopy(g_sItemName[ITEM_M2_CLOCK], 48, "M2 plant clock write");

    g_iItemModule[ITEM_M3_DECAY] = 3;
    strcopy(g_sItemName[ITEM_M3_DECAY], 48, "M3 nav danger decay writes");

    g_iItemModule[ITEM_M4_NOP] = 4;
    strcopy(g_sItemName[ITEM_M4_NOP], 48, "M4 random-overwrite NOP");
    g_iItemModule[ITEM_M4_HUNT] = 4;
    strcopy(g_sItemName[ITEM_M4_HUNT], 48, "M4 HuntState dhook writes");

    g_iItemModule[ITEM_M5_RELOAD] = 5;
    strcopy(g_sItemName[ITEM_M5_RELOAD], 48, "M5 reload-hide NOP");
    g_iItemModule[ITEM_M5_DODGE] = 5;
    strcopy(g_sItemName[ITEM_M5_DODGE], 48, "M5 dodge dhook write");
    g_iItemModule[ITEM_M5_RETREAT] = 5;
    strcopy(g_sItemName[ITEM_M5_RETREAT], 48, "M5 retreat dhook writes");

    g_iItemModule[ITEM_M6_NOISE] = 6;
    strcopy(g_sItemName[ITEM_M6_NOISE], 48, "M6 noise & OPPO dhook writes");

    g_iItemModule[ITEM_M7_ESCORT] = 7;
    strcopy(g_sItemName[ITEM_M7_ESCORT], 48, "M7 escort meme ret & SDKCalls");

    g_iItemModule[ITEM_M8_QUEUE] = 8;
    strcopy(g_sItemName[ITEM_M8_QUEUE], 48, "M8 queue breaker write");
    g_iItemModule[ITEM_M8_DEADLOCK] = 8;
    strcopy(g_sItemName[ITEM_M8_DEADLOCK], 48, "M8 deadlock Hunt SDKCall");

    g_iItemModule[ITEM_M9_FETCH] = 9;
    strcopy(g_sItemName[ITEM_M9_FETCH], 48, "M9 loose-bomb fetch gate & OPPO dhook");

    g_iItemModule[ITEM_M10_DEFUSE] = 10;
    strcopy(g_sItemName[ITEM_M10_DEFUSE], 48, "M10 defuse dhook gate");

    g_iItemModule[ITEM_M11_CORRIDOR] = 11;
    strcopy(g_sItemName[ITEM_M11_CORRIDOR], 48, "M11 corridor danger writes");

    g_iItemModule[ITEM_M12_SNIPERCAMP] = 12;
    strcopy(g_sItemName[ITEM_M12_SNIPERCAMP], 48, "M12 sniper camp bypass patch & gate");

    g_iItemModule[ITEM_M13_BUY] = 13;
    strcopy(g_sItemName[ITEM_M13_BUY], 48, "M13 sniper buy formula patch");

    g_iItemModule[ITEM_M14_DEFEND] = 14;
    strcopy(g_sItemName[ITEM_M14_DEFEND], 48, "M14 defend-rush formula patch");

    g_iItemModule[ITEM_M15_HIDE] = 15;
    strcopy(g_sItemName[ITEM_M15_HIDE], 48, "M15 hide duration dhook writes");

    g_iItemModule[ITEM_M16_ESCAPE] = 16;
    strcopy(g_sItemName[ITEM_M16_ESCAPE], 48, "M16 escape weapon dhook & SDKCall");
}

/**
 * 设置事项的后端就绪状态（后端是否存在/可用，不等于已武装）
 *
 * 后端失效时自动解除武装，杜绝“后端没了但事项还在跑”。
 */
void Item_SetReady(int item, bool ready)
{
    if (item < 1 || item > ITEM_COUNT)
    {
        return;
    }

    g_bItemReady[item] = ready;
    if (!ready)
    {
        g_bItemArmed[item] = false;
    }
}

/**
 * 查询事项后端是否就绪
 */
bool Item_IsReady(int item)
{
    if (item < 1 || item > ITEM_COUNT)
    {
        return false;
    }
    return g_bItemReady[item];
}

/**
 * 设置事项是否已武装（只有就绪的事项才能武装）
 */
void Item_SetArmed(int item, bool armed)
{
    if (item < 1 || item > ITEM_COUNT)
    {
        return;
    }

    g_bItemArmed[item] = (armed && g_bItemReady[item]);
}

/**
 * 查询事项是否已武装
 */
bool Item_IsArmed(int item)
{
    if (item < 1 || item > ITEM_COUNT)
    {
        return false;
    }
    return g_bItemArmed[item];
}

/**
 * 按模块统计事项总数
 */
int Items_CountForModule(int module)
{
    int count = 0;
    for (int i = 1; i <= ITEM_COUNT; i++)
    {
        if (g_iItemModule[i] == module)
        {
            count++;
        }
    }
    return count;
}

/**
 * 按模块统计后端就绪的事项数
 */
int Items_ReadyForModule(int module)
{
    int count = 0;
    for (int i = 1; i <= ITEM_COUNT; i++)
    {
        if (g_iItemModule[i] == module && g_bItemReady[i])
        {
            count++;
        }
    }
    return count;
}

/**
 * 按模块统计已武装的事项数
 */
int Items_ArmedForModule(int module)
{
    int count = 0;
    for (int i = 1; i <= ITEM_COUNT; i++)
    {
        if (g_iItemModule[i] == module && g_bItemArmed[i])
        {
            count++;
        }
    }
    return count;
}

/**
 * 把事项名拼成人类可读列表（仅失败项，供状态行/日志）
 */
void Items_FailedNamesForModule(int module, char[] buffer, int maxlen)
{
    buffer[0] = '\0';
    for (int i = 1; i <= ITEM_COUNT; i++)
    {
        if (g_iItemModule[i] == module && !g_bItemArmed[i])
        {
            if (buffer[0] != '\0')
            {
                StrCat(buffer, maxlen, ", ");
            }
            StrCat(buffer, maxlen, g_sItemName[i]);
        }
    }
}

/**
 * 计算模块四态
 *
 * @param module        模块编号 1..16
 * @param ruleEnabled   模块规则是否开启
 */
ItemModuleState Items_GetModuleState(int module, bool ruleEnabled)
{
    if (!ruleEnabled)
    {
        return ItemModule_Inactive;
    }

    int total = Items_CountForModule(module);
    int armed = Items_ArmedForModule(module);

    if (total > 0 && armed == total)
    {
        return ItemModule_Active;
    }
    if (armed > 0)
    {
        return ItemModule_Degraded;
    }
    return ItemModule_Failed;
}

/**
 * 四态显示名
 */
void Items_GetStateName(ItemModuleState state, char[] buffer, int maxlen)
{
    switch (state)
    {
        case ItemModule_Inactive: strcopy(buffer, maxlen, "INACTIVE");
        case ItemModule_Active:   strcopy(buffer, maxlen, "ACTIVE");
        case ItemModule_Degraded: strcopy(buffer, maxlen, "DEGRADED");
        case ItemModule_Failed:   strcopy(buffer, maxlen, "FAILED");
    }
}
