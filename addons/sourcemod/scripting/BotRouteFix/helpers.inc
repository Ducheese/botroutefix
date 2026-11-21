#if defined _botroutefix_helpers_included
    #endinput
#endif
#define _botroutefix_helpers_included

//========================================================================================
// BOT, ENTITY & MEMORY HELPER FUNCTIONS
//========================================================================================

/**
 * 物理级指针合法性门禁（纳秒级 O(1) 快速哨兵过滤）
 *
 * 统一拦截标准空指针 0x0 与 -1 错误哨兵，杜绝整数溢出回绕至 Page 0。
 *
 * @param addr          待判定的内存地址
 * @return              true = 非空且非 -1 的物理候选地址
 */
stock bool IsValidAddress(Address addr)
{
    return addr != Address_Null && addr != view_as<Address>(-1);
}

/**
 * 校验实体是否为有效在线玩家
 *
 * @param client        客户端索引
 * @param checkAlive    是否必须处于存活状态
 * @return              是否有效
 */
stock bool IsValidClient(int client, bool checkAlive = false)
{
    if (client <= 0 || client > MaxClients || !IsClientInGame(client))
    {
        return false;
    }

    if (checkAlive && !IsPlayerAlive(client))
    {
        return false;
    }

    return true;
}

/**
 * 校验实体是否为有效 CSBot
 *
 * @param client        客户端索引
 * @param checkAlive    是否必须处于存活状态
 * @return              是否为有效 Bot
 */
stock bool IsBot(int client, bool checkAlive = false)
{
    return IsValidClient(client, checkAlive) && IsFakeClient(client);
}

/**
 * 校验实体是否为指定阵营的有效 CSBot
 *
 * @param client        客户端索引
 * @param team          阵营索引 (2: T, 3: CT)
 * @param checkAlive    是否必须处于存活状态
 * @return              是否为指定阵营的有效 Bot
 */
stock bool IsTeamBot(int client, int team, bool checkAlive = false)
{
    return IsBot(client, checkAlive) && GetClientTeam(client) == team;
}

/**
 * 校验实体是否为真实人类玩家 (非 Bot)
 *
 * @param client        客户端索引
 * @param checkAlive    是否必须处于存活状态
 * @return              是否为真实人类玩家
 */
stock bool IsHumanPlayer(int client, bool checkAlive = false)
{
    return IsValidClient(client, checkAlive) && !IsFakeClient(client);
}

/**
 * 获取指定 Bot 的当前士气值 (Morale)
 * 士气值范围在 [-3, +3]：
 *   -3: TERRIBLE, -2: BAD, -1: NEGATIVE, 0: NEUTRAL
 *   +1: POSITIVE, +2: GOOD, +3: EXCELLENT
 *
 * @param client        客户端索引
 * @return              士气值 (若无效或未就绪返回默认 +1 POSITIVE)
 */
stock int GetBotMorale(int client)
{
    if (!IsBot(client, false) || g_iOffset_Morale <= 0)
    {
        return 1; // 引擎默认开局士气为 +1 (POSITIVE)
    }

    Address pBot = GetEntityAddress(client);
    if (!IsValidAddress(pBot))
    {
        return 1;
    }

    return LoadFromAddress(pBot + view_as<Address>(g_iOffset_Morale), NumberType_Int32);
}

/**
 * 获取指定阵营存活 Bot 的平均士气值 (若无存活 Bot 则兜底为默认 +1)
 *
 * @param team          阵营索引 (2: T, 3: CT)
 * @return              平均士气值
 */
stock int GetTeamAverageBotMorale(int team)
{
    int sum = 0;
    int count = 0;

    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsTeamBot(i, team, true))
        {
            sum += GetBotMorale(i);
            count++;
        }
    }

    if (count > 0)
    {
        return sum / count;
    }

    return 1; // 默认 +1 (POSITIVE)
}

/**
 * 获取持包者的等效士气值 (Effective Carrier Morale)
 * - 若持包者为 Bot：直接读取其自身士气；
 * - 若持包者为人类玩家：由于人类无士气属性，为保证战术逻辑自洽，统一取当前存活 T 阵营 Bot 的平均士气；
 * - 若全队无存活 Bot，则兜底为默认 +1 (POSITIVE)。
 *
 * @param carrier       持包者客户端索引
 * @return              等效士气值 [-3, +3]
 */
stock int GetEffectiveCarrierMorale(int carrier)
{
    if (IsBot(carrier, false))
    {
        return GetBotMorale(carrier);
    }
    return GetTeamAverageBotMorale(2);
}

/**
 * 获取士气值对应的文字枚举标签
 *
 * @param morale        士气数值
 * @param buffer        目标输出缓冲区
 * @param maxlen        缓冲区最大长度
 */
stock void GetMoraleName(int morale, char[] buffer, int maxlen)
{
    switch (morale)
    {
        case -3: strcopy(buffer, maxlen, "TERRIBLE");
        case -2: strcopy(buffer, maxlen, "BAD");
        case -1: strcopy(buffer, maxlen, "NEGATIVE");
        case 0:  strcopy(buffer, maxlen, "NEUTRAL");
        case 1:  strcopy(buffer, maxlen, "POSITIVE");
        case 2:  strcopy(buffer, maxlen, "GOOD");
        case 3:  strcopy(buffer, maxlen, "EXCELLENT");
        default: Format(buffer, maxlen, "%d", morale);
    }
}

/**
 * 获取当前手持或携带 C4 的 T 阵营存活玩家实体索引
 *
 * @return              玩家客户端索引 (若无返回 0)
 */
stock int GetC4Carrier()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsValidClient(i, true) && GetClientTeam(i) == 2)
        {
            int c4 = GetPlayerWeaponSlot(i, 4);
            if (c4 != -1 && IsValidEntity(c4))
            {
                return i;
            }
        }
    }
    return 0;
}

/**
 * 全库统一的实体地址反解 Bot 客户端索引（收敛 M6 内联循环 / M11 Sniper_ResolveClient /
 * M16 Escape_ResolveClient 三处同构实现；调用方判 `== 0` 或 `!IsBot(x)` 均安全）。
 *
 * 只收 Bot（IsBot 含存活检查）；空指针与未命中一律返回 0（0 即非法索引）。
 * 失败语义：返回 0，调用方按"未命中"放行原生，不触碰事项状态。
 *
 * @param pBot          CCSBot 实体地址
 * @return              Bot 客户端索引；0 = 空指针/未命中
 */
stock int ResolveBotClient(Address pBot)
{
    if (pBot == Address_Null)
    {
        return 0;
    }

    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsBot(i, true))
        {
            if (GetEntityAddress(i) == pBot)
            {
                return i;
            }
        }
    }
    return 0;
}
