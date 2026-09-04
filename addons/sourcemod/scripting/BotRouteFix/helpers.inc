#if defined _botroutefix_helpers_included
    #endinput
#endif
#define _botroutefix_helpers_included

//========================================================================================
// BOT & ENTITY HELPER FUNCTIONS
//========================================================================================

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
 * 获取指定 Bot 的当前士气值 (Morale)
 * 士气值范围在 [-3, +3]：
 *   -3: TERRIBLE, -2: BAD, -1: NEGATIVE, 0: NEUTRAL
 *   +1: POSITIVE, +2: GOOD, +3: EXCELLENT
 *
 * @param client        客户端索引
 * @return              士气值 (若无效或未就绪返回 0)
 */
stock int GetBotMorale(int client)
{
    if (!IsBot(client, false) || g_iOffset_Morale <= 0)
    {
        return 1; // 引擎默认开局士气为 +1 (POSITIVE)
    }

    Address pBot = GetEntityAddress(client);
    if (pBot == Address_Null)
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
        if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == team && IsFakeClient(i))
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
 * 获取当前手持或携带 C4 的 T 阵营玩家实体索引
 *
 * @return              玩家客户端索引 (若无返回 0)
 */
stock int GetC4Carrier()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2)
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
