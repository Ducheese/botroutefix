#if defined _botroutefix_visuals_included
    #endinput
#endif
#define _botroutefix_visuals_included

//========================================================================================
// VISUALS & TELEMETRY DEBUG OVERLAY SUBSYSTEM
//
// 游戏内视觉特效呈现子系统 (Visuals & TempEnts Engine)：
// 受 sm_botroutefix_debug 单一 ConVar 总控约束：
//   - debug == 0 时：两组 Timer 首行早退，0 计算、0 内存开销、0 网络流量静默；
//   - debug == 1 时：利用 CS:S 原生 TempEnts (TE_SetupBeam*) 将 8 大模块实时战术决策
//     以激光线、光环与 3D 警戒盒直接画进游戏世界。
//
// 视觉设计（与下方实现逐条对应）：
//   - M2 (Bombsite): 包匪胸膛连目标包点，黄色激光线（粗 3.0）；下包延时未到期时不画任何光环，
//     到期起在包匪脚底叠加红色扩散光环（包点上不画）；
//   - M4 (Hunt): 各阵营 Bot 统一红色激光线（粗 3.0）连认领的 NavArea 中心，末端不画光环；
//     包匪在 HuntState 时同样画线，不互斥；认领心跳超过 0.3s 即熄线；
//   - M5 (Retreat): 撤退原因三色分流（残血：红色，一打多：紫色，遇狙：青色），
//     用 TE_SetupBeamFollow 拖尾跟随人物，0.6s 接力续 0.8s；
//   - M6 (Disposition): 腰部静止环不区分阵营（绿色 OPPORTUNITY_FIRE vs 粉色 ENGAGE_AND_INVESTIGATE）；
//   - M7 (Escort): 保镖胸膛连包匪胸膛，绿色光索（粗 3.0）；
//   - M8 (Queue): 加过 danger 的 NavArea 显示红色 3D 警戒盒，持续至少 1 分钟；死锁打破时电光冲击波；
//   - M9 (Loot): 抢包花名册 T 胸膛连掉落 C4，黄色波浪光束（粗 3.0，振幅 12.0）；
//   - M10 (Defuse): 首席拆包 CT 胸膛连已安放 C4，蓝色波浪光束（粗 3.0，振幅 12.0）。
//========================================================================================

static int g_iLaserBeam = -1;
static int g_iHalo = -1;

/**
 * 预缓存原生激光与光环精灵纹理
 */
void Visuals_OnMapStart()
{
    g_iLaserBeam = PrecacheModel("materials/sprites/laserbeam.vmt", true);
    g_iHalo      = PrecacheModel("materials/sprites/halo01.vmt", true);
}

/**
 * 初始化可视化定时器 (快速 0.2s 连线与慢速 1.0s 网格)
 */
void Visuals_Init()
{
    CreateTimer(0.2, Timer_RenderVisualsFast, _, TIMER_REPEAT);
    CreateTimer(1.0, Timer_RenderVisualsSlow, _, TIMER_REPEAT);
}

//========================================================================================
// 基础图元绘制封装
//========================================================================================

static void DrawBeam(const float start[3], const float end[3], const int color[4], float width = 2.5, float amplitude = 0.0, float life = 0.25)
{
    TE_SetupBeamPoints(start, end, g_iLaserBeam, g_iHalo, 0, 0, life, width, width, 0, amplitude, color, 0);
    TE_SendToAll();
}

static void DrawRing(const float center[3], float startRadius, float endRadius, const int color[4], float width = 3.0, float life = 0.25, int speed = 10)
{
    TE_SetupBeamRingPoint(center, startRadius, endRadius, g_iLaserBeam, g_iHalo, 0, 15, life, width, 0.0, color, speed, 0);
    TE_SendToAll();
}

static void GetBotChestPosition(int client, float chest[3])
{
    GetClientEyePosition(client, chest);
    chest[2] -= 12.0;
}

static void NavArea_GetCenter(Address pArea, float center[3])
{
    if (pArea == Address_Null)
    {
        return;
    }
    int offsetCenter = g_bIsWin64 ? 48 : 44;
    center[0] = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetCenter),     NumberType_Int32));
    center[1] = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetCenter + 4), NumberType_Int32));
    center[2] = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetCenter + 8), NumberType_Int32));
}

static void NavArea_GetCorners(Address pArea, float c0[3], float c1[3], float c2[3], float c3[3])
{
    if (pArea == Address_Null)
    {
        return;
    }
    int offsetNw  = g_bIsWin64 ? 8  : 4;
    int offsetSe  = g_bIsWin64 ? 20 : 16;
    int offsetNeZ = g_bIsWin64 ? 40 : 36;
    int offsetSwZ = g_bIsWin64 ? 44 : 40;

    float nwX = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetNw),     NumberType_Int32));
    float nwY = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetNw + 4), NumberType_Int32));
    float nwZ = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetNw + 8), NumberType_Int32));

    float seX = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetSe),     NumberType_Int32));
    float seY = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetSe + 4), NumberType_Int32));
    float seZ = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetSe + 8), NumberType_Int32));

    float neZ = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetNeZ),    NumberType_Int32));
    float swZ = view_as<float>(LoadFromAddress(pArea + view_as<Address>(offsetSwZ),    NumberType_Int32));

    c0[0] = nwX; c0[1] = nwY; c0[2] = nwZ;
    c1[0] = seX; c1[1] = nwY; c1[2] = neZ;
    c2[0] = seX; c2[1] = seY; c2[2] = seZ;
    c3[0] = nwX; c3[1] = seY; c3[2] = swZ;
}

static void DrawNavAreaBox(Address pArea, const int color[4], float height = 36.0, float life = 1.05)
{
    float c0[3], c1[3], c2[3], c3[3];
    NavArea_GetCorners(pArea, c0, c1, c2, c3);

    float t0[3], t1[3], t2[3], t3[3];
    t0[0] = c0[0]; t0[1] = c0[1]; t0[2] = c0[2] + height;
    t1[0] = c1[0]; t1[1] = c1[1]; t1[2] = c1[2] + height;
    t2[0] = c2[0]; t2[1] = c2[1]; t2[2] = c2[2] + height;
    t3[0] = c3[0]; t3[1] = c3[1]; t3[2] = c3[2] + height;

    // 底面矩形边框
    DrawBeam(c0, c1, color, 3.0, 0.0, life);
    DrawBeam(c1, c2, color, 3.0, 0.0, life);
    DrawBeam(c2, c3, color, 3.0, 0.0, life);
    DrawBeam(c3, c0, color, 3.0, 0.0, life);

    // 四角垂直立柱
    DrawBeam(c0, t0, color, 3.0, 0.0, life);
    DrawBeam(c1, t1, color, 3.0, 0.0, life);
    DrawBeam(c2, t2, color, 3.0, 0.0, life);
    DrawBeam(c3, t3, color, 3.0, 0.0, life);

    // 顶面矩形边框
    DrawBeam(t0, t1, color, 2.0, 0.0, life);
    DrawBeam(t1, t2, color, 2.0, 0.0, life);
    DrawBeam(t2, t3, color, 2.0, 0.0, life);
    DrawBeam(t3, t0, color, 2.0, 0.0, life);

    // 地面中心脉冲光环
    float center[3];
    NavArea_GetCenter(pArea, center);
    DrawRing(center, 20.0, 80.0, color, 3.0, life, 15);
}

//========================================================================================
// 定时渲染主循环
//========================================================================================

/**
 * 快速循环 (每 0.2s 刷新一次人物连线与高频光效)
 */
public Action Timer_RenderVisualsFast(Handle timer)
{
    if (g_cvDebug == null || !g_cvDebug.BoolValue)
    {
        return Plugin_Continue;
    }

    if (g_iLaserBeam <= 0 || g_iHalo <= 0)
    {
        return Plugin_Continue;
    }

    //------------------------------------------------------------------------------------
    // [M2] 包匪下包指引线与就绪光环
    //------------------------------------------------------------------------------------
    if (IsModule2_Active())
    {
        int carrier = GetC4Carrier();
        if (carrier > 0 && carrier <= MaxClients && IsClientInGame(carrier) && IsPlayerAlive(carrier))
        {
            float targetPos[3];
            if (Module2_GetTargetBombsitePosition(targetPos))
            {
                float carrierChest[3];
                GetBotChestPosition(carrier, carrierChest);

                // 包匪胸膛连目标包点，黄色 (包点不画光环)
                static const int colorM2Beam[4] = {255, 170, 0, 220}; // 黄色
                DrawBeam(carrierChest, targetPos, colorM2Beam, 3.0, 0.0, 0.25);

                float earliest = Module2_GetEarliestPlantTime();
                float curtime = GetGameTime();
                if (curtime >= earliest && earliest > 0.0)
                {
                    // 下包延时到期：光圈显示在【包匪脚下】！跟随包匪开始高频闪烁纯正猩红扩散光圈
                    float carrierFeet[3];
                    GetClientAbsOrigin(carrier, carrierFeet);
                    carrierFeet[2] += 5.0; // 紧贴脚底地面

                    static const int colorM2Ring[4] = {255, 0, 0, 255}; // 纯正猩红
                    DrawRing(carrierFeet, 20.0, 80.0, colorM2Ring, 3.5, 0.25, 12);
                }
                // 延时未到期前：包匪脚底绝对不闪光圈，保持 100% 静默纯净
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M4] HuntState 认领 NavArea 直连激光线 (统一用红色线)
    //------------------------------------------------------------------------------------
    if (IsModule4_Active())
    {
        static const int colorM4_Red[4] = {255, 40, 40, 200}; // 红色激光线

        for (int i = 1; i <= MaxClients; i++)
        {
            if (!IsClientInGame(i) || !IsPlayerAlive(i) || !IsFakeClient(i))
            {
                continue;
            }

            Address pArea = Module4_GetHuntArea(i);
            if (pArea != Address_Null)
            {
                float areaCenter[3], botChest[3];
                NavArea_GetCenter(pArea, areaCenter);
                GetBotChestPosition(i, botChest);

                DrawBeam(botChest, areaCenter, colorM4_Red, 3.0, 0.0, 0.25);
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M5] 战斗后撤三色标识 (不画贴地光圈，用 TE_SetupBeamFollow 拖尾)
    //------------------------------------------------------------------------------------
    if (IsModule5_Active())
    {
        static const int colorM5_LowHp[4]       = {255, 30, 30, 220};     // 残血：红色
        static const int colorM5_Outnumbered[4] = {180, 0, 255, 220};    // 一打多：紫色
        static const int colorM5_Sniper[4]      = {0, 220, 255, 220};    // 遇狙：青色

        static float s_flNextBeamFollow[MAXPLAYERS + 1];
        float curtime = GetGameTime();

        for (int i = 1; i <= MaxClients; i++)
        {
            if (!IsClientInGame(i) || !IsPlayerAlive(i) || !IsFakeClient(i))
            {
                s_flNextBeamFollow[i] = 0.0;
                continue;
            }

            int reason = Module5_GetRetreatReason(i);
            if (reason != RETREAT_REASON_NONE)
            {
                // 每 0.6 秒续一道 0.8 秒拖尾，跟随人物
                if (curtime >= s_flNextBeamFollow[i])
                {
                    s_flNextBeamFollow[i] = curtime + 0.6;

                    switch (reason)
                    {
                        case RETREAT_REASON_LOW_HP:
                        {
                            TE_SetupBeamFollow(i, g_iLaserBeam, g_iHalo, 0.8, 6.0, 0.5, 10, colorM5_LowHp);
                            TE_SendToAll();
                        }
                        case RETREAT_REASON_OUTNUMBERED:
                        {
                            TE_SetupBeamFollow(i, g_iLaserBeam, g_iHalo, 0.8, 6.0, 0.5, 10, colorM5_Outnumbered);
                            TE_SendToAll();
                        }
                        case RETREAT_REASON_SNIPER:
                        {
                            TE_SetupBeamFollow(i, g_iLaserBeam, g_iHalo, 0.8, 6.0, 0.5, 10, colorM5_Sniper);
                            TE_SendToAll();
                        }
                    }
                }
            }
            else
            {
                s_flNextBeamFollow[i] = 0.0;
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M6] 接战策略腰部静止环 (不区分阵营，互补色: 绿色 OPPORTUNITY_FIRE vs 粉色 INVESTIGATE)
    //------------------------------------------------------------------------------------
    if (IsModule6_Active())
    {
        static const int colorM6_OppFire[4]     = {0, 255, 128, 220};   // OPPORTUNITY_FIRE: 绿色腰环 (架枪/固守)
        static const int colorM6_Investigate[4] = {255, 60, 180, 220};  // ENGAGE_AND_INVESTIGATE: 粉色腰环 (探查/出击)

        for (int i = 1; i <= MaxClients; i++)
        {
            if (!IsClientInGame(i) || !IsPlayerAlive(i) || !IsFakeClient(i))
            {
                continue;
            }

            int disp = Module6_GetBotDisposition(i);
            if (disp == 1) // OPPORTUNITY_FIRE
            {
                float pos[3];
                GetClientAbsOrigin(i, pos);
                pos[2] += 28.0; // 腰部高度
                DrawRing(pos, 26.0, 28.0, colorM6_OppFire, 3.0, 0.25, 0); // speed = 0 静止紧身
            }
            else if (disp == 0) // ENGAGE_AND_INVESTIGATE
            {
                float pos[3];
                GetClientAbsOrigin(i, pos);
                pos[2] += 28.0; // 腰部高度
                DrawRing(pos, 26.0, 28.0, colorM6_Investigate, 3.0, 0.25, 0); // speed = 0 静止紧身
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M7] 保镖连包匪的绿色光索
    //------------------------------------------------------------------------------------
    if (IsModule7_Active())
    {
        int carrier = Escort_GetCarrier();
        if (carrier > 0 && carrier <= MaxClients && IsClientInGame(carrier) && IsPlayerAlive(carrier))
        {
            int bodyguards[MAXPLAYERS + 1];
            int count = Module7_GetBodyguards(bodyguards, sizeof(bodyguards));

            if (count > 0)
            {
                float carrierChest[3];
                GetBotChestPosition(carrier, carrierChest);

                static const int colorM7[4] = {60, 255, 80, 220}; // 绿色光索
                for (int j = 0; j < count; j++)
                {
                    int guard = bodyguards[j];
                    float guardChest[3];
                    GetBotChestPosition(guard, guardChest);
                    DrawBeam(guardChest, carrierChest, colorM7, 3.0, 0.0, 0.25);
                }
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M8] 死锁打破电光冲击波 (跟随人物)
    //------------------------------------------------------------------------------------
    if (IsModule8_Active())
    {
        static const int colorM8Deadlock[4] = {150, 220, 255, 255}; // 白蓝色电光

        for (int i = 1; i <= MaxClients; i++)
        {
            if (!IsClientInGame(i) || !IsPlayerAlive(i))
            {
                continue;
            }

            if (Module8_IsDeadlockBroken(i))
            {
                float origin[3];
                GetClientAbsOrigin(i, origin);

                float top[3];
                top[0] = origin[0]; top[1] = origin[1]; top[2] = origin[2] + 90.0;
                DrawBeam(origin, top, colorM8Deadlock, 4.0, 2.0, 0.25);

                origin[2] += 5.0;
                DrawRing(origin, 20.0, 130.0, colorM8Deadlock, 5.0, 0.25, 15);
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M9] 捡包花名册 T 连掉落 C4
    //------------------------------------------------------------------------------------
    if (IsModule9_Active())
    {
        int roster[LOOT_ROSTER_SIZE];
        float c4Pos[3];
        int count = Module9_GetFetchRoster(roster, c4Pos);

        if (count > 0)
        {
            // C4 不画光环，花名册 T 胸膛射出波浪光束连 C4
            static const int colorM9Beam[4] = {255, 215, 0, 230};  // 黄色波浪光束

            for (int j = 0; j < count; j++)
            {
                int bot = roster[j];
                float botChest[3];
                GetBotChestPosition(bot, botChest);
                DrawBeam(botChest, c4Pos, colorM9Beam, 3.0, 12.0, 0.25);
            }
        }
    }

    //------------------------------------------------------------------------------------
    // [M10] 首席拆包 CT 直连安放 C4 激光
    //------------------------------------------------------------------------------------
    if (IsModule10_Active())
    {
        float c4Pos[3];
        int chief = Module10_GetChiefDefuser(c4Pos);

        if (chief > 0)
        {
            // C4 不画光环，粗 3.0、振幅 12.0，与 M9 一致
            static const int colorM10Beam[4] = {30, 120, 255, 255}; // 蓝色波浪光束

            float chiefChest[3];
            GetBotChestPosition(chief, chiefChest);
            DrawBeam(chiefChest, c4Pos, colorM10Beam, 3.0, 12.0, 0.25);
        }
    }

    return Plugin_Continue;
}

/**
 * 慢速循环 (每 1.0s 刷新一次持续 60s 的堵路 3D 警戒框)
 */
public Action Timer_RenderVisualsSlow(Handle timer)
{
    if (g_cvDebug == null || !g_cvDebug.BoolValue)
    {
        return Plugin_Continue;
    }

    if (g_iLaserBeam <= 0 || g_iHalo <= 0)
    {
        return Plugin_Continue;
    }

    //------------------------------------------------------------------------------------
    // [M8] 堵路 NavArea 红色 3D 警戒框 (持续至少 1 分钟)
    //------------------------------------------------------------------------------------
    if (IsModule8_Active())
    {
        Address areas[MAX_BLOCKED_AREAS];
        float expires[MAX_BLOCKED_AREAS];
        int count = Module8_GetBlockedAreas(areas, expires);

        static const int colorBlockedArea[4] = {255, 30, 30, 220}; // 红色
        for (int i = 0; i < count; i++)
        {
            DrawNavAreaBox(areas[i], colorBlockedArea, 36.0, 1.05);
        }
    }

    return Plugin_Continue;
}
