# BotRouteFix（CSBot 决策链与战术寻路重构框架）

针对 Counter-Strike: Source (CS:S) 官方 CSBot 底层决策链缺陷与行为树痛点进行深度重构的 SourceMod 插件。
结合内存字节级补丁（Byte Patching）、DHook 动态拦截（Detours）、SDKCall 引擎能力调用以及事件感知体系，全面提升 Bot 的竞技战术逻辑与路线决策质量。

---

## 模块清单与功能

### 模块 1：CT 守点概率重构与防开局前冲 (Defense & Anti-Rush)
- **防全队前冲 (Anti-Rush NOP)**：
  - 原版引擎每回合开局掷骰 `m_isDefenseRushing = (RandomFloat(0,100) <= 33.3f)`（`cs_bot_manager.cpp:147`）。
  - 在拆弹图（Defuse Scenario）下，一旦触发整队 CT（除独狼外）立即执行 `me->Hunt()` 弃点突击，导致 1/3 的局数完全无人守包点。
  - 本模块将 `IdleState::OnUpdate` 中的 `0F 85 <rel32>` (JNE -> Hunt) 条件跳转 6 字节彻底 NOP 穿透，使 CT 永不误判突击，正常顺延到包点防守与战术决策逻辑。
- **守包点概率公式重写 (guardBombsiteChance)**：
  - 原生公式为 `guardBombsiteChance = -34.0f * me->GetMorale()`（`idle.cpp:413`）。开局顺风或初始士气为 +1 时，概率算出来恒为 -34%，导致 CT 开局守点概率为 0%。
  - 本模块将公式原地汇编重写为 `70.0f - 10.0f * Morale`：
    - 士气 -3（大劣势）：100% 坚守包点
    - 士气 0（均势）：70% 坚守包点
    - 士气 +1（微优）：60% 坚守包点
    - 士气 +3（大胜）：40% 坚守包点（其余 60% 主动全图猎杀抓人）
  - 64 位架构（Steam x64）：26 字节纯 SSE 汇编替换 (`cvtsi2ss xmm6, eax`)。
  - 32 位架构（v91/v92）：55 字节完整 x87 FPU 状态机重写，严格遵守跨调用浮点栈平衡契约。

### 模块 2：T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
- **运包时钟士气线性映射 (IsTimeToPlantBomb)**：
  - 原生引擎开局使用生硬随机值 `m_earliestBombPlantTimestamp = curtime + RandomFloat(10.0f, 30.0f)`（`manager.cpp:138`）。
  - 本模块接入持包者士气线性映射：`delay = 30.0 + 10.0 * Morale`（限制在 0.0s ~ 60.0s）。
    - 士气 -3（大劣势）：0.0s（即刻下包，分秒必争）
    - 士气 0（均势）：30.0s（常规节奏）
    - 士气 +3（大胜）：60.0s（延缓下包，留出充分拉扯与击杀窗口）
  - 冻结时间结束（`round_freeze_end`）瞬间写入 `TheCSBots()->m_earliestBombPlantTimestamp`。
- **开局包点随机锁定与换人动态自愈 (GetClosestZone Patch)**：
  - 原版引擎在运包时硬编码调用 `TheCSBots()->GetClosestZone()`（`idle.cpp:163`），导致 Bot 局局只冲几何最近包点。
  - 本模块在开局冻结结束时，从地图所有可用包点中随机选取一个，将调用序列重写为目标包点指针的直接加载：
    - 64 位（19 字节）：`mov rax, <Zone*> + 9x NOP`
    - 32 位（19 字节）：`mov eax, <Zone*> + 14x NOP`
  - **换人自愈与残局自适应 (Self-Healing on Carrier Change)**：
    - 一旦开局包匪掉包（`bomb_dropped`）、中途阵亡（`player_death`）或由新队友拾取雷包（`item_pickup`），立即毫秒级卸载内存补丁，无缝恢复引擎原生 `GetClosestZone()` 动态寻路；
    - 确保残局接力持包的队员能够根据实时交火站位就近选择最优包点下包，兼顾“开局多路线战术突袭”与“残局就近下包自适应”。

### 模块 3：CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
- **危险度结算与沉降契约 (Settlement Contract)**：
  - 原生引擎危险度衰减速率为 `1.0 / 120.0s`（`nav_area.cpp:4139`），采用惰性读衰减（Lazy Decay）机制，内存裸值陈旧。
  - 本模块严格遵守沉降契约：读取裸值后先结算自然流逝时间差 `effDanger = raw - (curtime - timestamp) / 120.0`。
- **开局 80% 软衰减与底噪截断 (80% Decay & Noise Floor)**：
  - 每回合开始（`round_start`）时，全图遍历 `TheNavAreas` 容器，对所有区域危险度执行 80% 衰减（`effDanger * 0.2`），衰减后若危险度 $< 0.05$ 则截断归零（$0.0$），并将更新时间戳重置为当前游戏时间。
  - 既清除了上一回合交火留下的严重路线偏见与微弱浮点底噪，又保留了 20% 残余记忆，使 Bot 在新回合开局寻路时具备动态且富于变化的战术权衡。

### 模块 4：HuntState 搜敌机制重构 (HuntState Target Claim & Oldest-Scan Restore)
- **破除 45 秒直冲敌方老家 (45s Rush Bypass)**：
  - 原生引擎开局 45 秒内进入 HuntState 时，强制锁定目标区域为敌方出生点（`cs_bot_hunt.cpp:164-175`），导致开局全图撞车扎堆。
  - 本模块在 `HuntState::OnUpdate` 的 Pre 钩子中，动态将 Bot 的 `m_hasVisitedEnemySpawn` 标志置为 `true`，使条件恒假，开局第 0 秒即走全图区域搜索。
- **NOP 随机选点无守卫覆盖 (Random-Override Bypass)**：
  - 原生引擎在最久未清理扫描之后，随机选点循环缺失了 `if (m_huntArea == NULL)` 分支保护，无条件覆盖掉了 oldest-scan 找到的区域。
  - 本模块将随机循环中对 `m_huntArea` 的回写指令 NOP 穿透（32位 3字节 / 64位 4字节），完好保留最久未清理扫描结果，使基于时间戳的搜索具备数学意义。
- **目标区域盖章认领 (Target Area Claiming)**：
  - 在 Post 钩子中读取最终选定的 `m_huntArea`，并将其所属阵营的 `m_clearedTimestamp[teamIdx]` 盖上当前游戏时间 `curtime`（`teamIdx = (team - 1) % 2`）。
  - 刚被选取的区域其 age 瞬间归零，同队其他 Bot 在后续扫描时绝不会二次选中，达成天然、无死锁的战术多路分散搜敌！

### 模块 5：战斗态规避机动、掩体换弹与残血/一打多后撤 (Combat Overhaul: Dodge, Reload Hide & Retreat)
- **闪避判定 100% 强制 (AttackState Dodge Forced)**：
  - 原生引擎在 `AttackState::OnEnter` 中根据 `dodgeChance = 80.0f * profile->GetSkill()` 掷骰判定是否闪避（`cs_bot_attack.cpp:115-130`），普通与低难度 Bot 极少闪避，对枪如同呆立木桩。
  - 本模块通过 DHook 拦截 `AttackState::OnEnter` 的 Post 阶段，直接将 `m_shouldDodge` 字段强制写为 `1`（32位 +50，64位 +58），确保全员交火时必进 `Dodge()` 规避机动，后续动作灵敏度交由 `Dodge()` 内部按 skill 分化。
- **换弹进掩体 100% 强制 (Reload HideChance NOP)**：
  - 原生引擎在 `cs_bot_weapon.cpp:1186-1210` 中，换弹时依据 `hideChance = 25.0f + 100.0f * profile->GetSkill()` 掷骰看脸决定是否寻找掩体。
  - 本模块将掷骰失败跳过掩体的条件短跳转指令（32位 2字节 `76 ??`，64位 2字节 `73 ??`）NOP 穿透为 `90 90`，抹去开阔地看脸原地换弹的弊端，只要满足前置条件（附近有敌且 5s 内见敌）换弹必进掩体！
- **残血强制后撤 (Low-Health Forced Retreat via PinnedDown)**：
  - 原生引擎在 `AttackState::OnUpdate` 撤退段中受 `isPinnedDown`（交火持续 7~10 秒）与自带 3~15 秒冷却约束，导致残血 Bot 往往死战至死。
  - 本模块通过 DHook 拦截 `AttackState::OnUpdate` 的 Pre 阶段，当检测到 Bot 血量 $< 35\text{ HP}$ 时，直接将 `m_pinnedDownTimestamp` 写为 `0.0`（使 `isPinnedDown` 恒真）并清空撤退计时器冷却；下一帧引擎原生撤退段在满足其余门控（有敌人正瞄着自己、掩体可达，**60 秒长节流冷却**）时触发 `TryToRetreat()` 寻找就近掩体撤退并转入 `HideState`，显著提升竞技残局生存率！
- **包匪使命特权防护 (C4 Carrier Mission Shield)**：
  - **保镖在场豁免**：身边有存活队友/保镖（`nearbyFriends > 0`）时，持包者绝对禁止后撤，由保镖拉枪吸引火力，包匪维持推进；
  - **下包任务豁免**：已处于 `PLANT_BOMB` 任务（`Task == 1`）时，下包使命压倒一切，绝对禁止后撤；
  - 彻底根除残血包匪受击抛下保镖掉头逃跑进掩体的恶性折返跑死循环！
- **孤身一打多强制后撤 (Outnumbered Forced Retreat via SDKCall)**：
  - 原生 `CCSBot::IsOutnumbered() = 友 < 敌-1`（`cs_bot.cpp:606-621`，自己不占名额，1v2 起算），计数读 3 秒视觉记忆表而非距离圈；原生只走 c 支骰子/aggr，高侵略 Bot 近乎不撤。
  - 本模块同一 Pre 钩子直调原生判定，确认被包后再读 `m_nearbyFriendCount`：视野内有队友（2v3/2v4）放行交火，仅计数为 0 的真 1v 多才写同一两处内存焊门（1、2 号门焊死，3 号门"有人瞄着"仍归原生）。
  - **完全不走节流阀**：一对多属于高危态，完全脱离节流阀限制，只要遇围随时允许后撤拉开身位。
- **狙击手威慑强撤特权接口 (Combat_RequestSniperRetreat Bypass)**：
  - 供模块 11 远程调用。目击 Bot 遭遇敌方狙击手后，由模块 11 调用此接口将 `g_bSniperRetreatWanted[client]` 置为 true；
  - 在随后的 `AttackState::OnUpdate` Pre 阶段，**完全不走节流阀**与 `< 35 HP` 残血门控，直接写零 `m_pinnedDownTimestamp` 并清空 `m_retreatTimer`，引导 Bot 立即寻找掩体脱战；
  - **全员一视同仁后撤**：遭遇狙击手威胁时，所有人无论谁（包括持包者，无论身边是否有保镖），全员统一撤退寻找掩体拉开身位，杜绝保镖退入掩体而包匪独自暴露在狙击线下的白给现象；包匪护卫抗线豁免仅在常规交火残血与一对多中生效。

### 模块 6：T 阵营士气接战策略与守包听觉出击梯度 (Morale Disposition & Audio Interception)
- **下包前 T 包匪行为 (Pre-Plant Carrier Morale Disposition)**：
  - 原生引擎任何战区的枪声广播（2000码）都会诱使全图 Bot 产生 30%~60% 几率调头去凑热闹，导致包匪放弃运包下包。
  - 本模块在 `round_freeze_end` 与 `item_pickup`（获得 C4）时，依据持包者士气单次赋值：
    - 低士气（Morale $\le$ -1）：赋予 `OPPORTUNITY_FIRE` (1)，底层 `HeardInterestingNoise()` 自动对声音免疫，心无旁骛直奔包点下包；
    - 高士气（Morale $\ge$ 0）：保持原生 `ENGAGE_AND_INVESTIGATE` (0)。
- **下包后 T 全队守点梯度 (Post-Plant Defense & Audio Interception)**：
  - 下包瞬间（`bomb_planted`）全队 T 按士气分层：
    - 低士气（Morale $\le$ -1）：设为 `OPPORTUNITY_FIRE` (1) 并清空声音时间戳，HideState 门控自动阻断出击，死守掩体架枪；
    - 高士气（Morale $\ge$ 0）：保持 `ENGAGE_AND_INVESTIGATE` (0) 允许视情出击。
  - 出击拦截（DHook Pre on `CCSBot::InvestigateNoise`）：
    - 仅拦截守包掩体任务（Task 4: `GUARD_TICKING_BOMB`）；
    - 针对保持 ENGAGE 的非负士气 T，按士气阶梯约束最大可调查声音距离：
      - 士气 0 $\rightarrow$ 最大 500.0 码
      - 士气 +1 $\rightarrow$ 最大 1000.0 码
      - 士气 +2 $\rightarrow$ 最大 2000.0 码
      - 士气 +3 $\rightarrow$ 最大 3000.0 码
    - 超距或线性概率衰减（$1.0 - \text{dist} / \text{maxDist}$）未命中时，清空声音时间戳并直接 `MRES_Supercede` 拦截状态切换，原地保持架枪，彻底瓦解战火黑洞效应！

### 模块 7：T 包匪贴身保镖花名册 (Carrier Escort Roster)
- **编制随士气浮动 (Morale-Scaled Roster)**：
  - 原生引擎没有包匪专职护送概念，持包者与单打独斗无异。
  - 本模块按持包者士气配给保镖：`保镖数 = 3 - Morale`（钳制在 0~6 名）。士气 -3（绝境）：6 名抱团强推；士气 +1（常规）：2 名随行；士气 +3（大优）：0 名，全员放飞控图。
- **欧氏 3D 选人与差量刷新 (Euclidean Pick & Diff Refresh)**：
   - 保镖人选按包匪坐标欧氏 3D 平方距离（含 Z，跨楼层天然排后）取最近者；用 A* 真实距离精度更高，但在 10v10 开局每回合 O(N) 次寻路是可观卡顿，故以欧氏换零开销（同层排序精确，跨楼层自动靠后）。
  - 名单只做差量更新：换包匪才解散重配；保镖阵亡/掉线/转队只除名并按实时士气多退少补，不抖散整条跟随链。
- **包离手即解散 (Release on Loose)**：
  - 掉包、下包、拆包、爆炸、包匪阵亡/掉线时全体解散（发 `StopFollowing` 恢复自由身，可就近捡包或转入守点）；回合开始静默清零。
  - 冻结期内不接管（跟随会打断买枪），中途捡包递延一帧等槽位归属落定。

### 模块 8：门口礼貌排队打碎与死锁强切 Hunt (Polite Queue Breaker & Deadlock Hunt Transition)
- **原生排队过久与顽固对冲死锁 (Over-Polite Waiting & Deadlock)**：
  - 门口/路口被队友挡路时，原生礼貌等待要 2~5 秒（`5 - 3 * aggression`）才散，且散开后可能一头再扎回同一个堵点。
  - 开局多名 Bot（如 de_dust2 CT 出生点前往 A/B 区）路线交叉迎面对冲，互相判定阻挡而陷入“礼貌互让”；即便在开阔区域，由于双方守点任务目标未变，重算路径后依然迎面相撞，极易形成长达十数秒甚至整回合的顽固死锁。
- **Post 钩子打碎与 10 秒死锁强切 (Break on Post Hook & Hunt Transition)**：
  - 本模块在 `CCSBot::UpdatePathMovement` 的 Post 阶段介入（与引擎评估等待同一位置，无 timer 轮询）：单次等待超过 2.0 秒即清等待旗；被堵 node 与下一个 node 各钉 `+0.8` 危险度（封顶 2.5，沉降契约照旧），A* 立即绕开，下次重算不走回头路；随后手工碎路径逼立即重算。
  - **顽固死锁判定与强切**：累计打碎达 5 次（`QUEUE_SHATTER_LIMIT 5`，每次卡 2 秒，累计约 10 秒死锁），判定为顽固任务锁死；通过 `RequestFrame` 递延一帧安全调用 `CCSBot::Hunt` 强切至 `HuntState`（任务被引擎重置为 `SEEK_AND_DESTROY`），彻底洗掉僵尸执念任务并全局重新自主选路奔赴战场，秒级化解迎面对冲死锁。
  - 回合重置、模块停用/卸载时自动清零计数；原生 2~5 秒机制保留在后兜底，双向交接。
  - 危险度下标与模块 3 同约定：`teamID % 2`（T=0/CT=1），切勿与模块 4 的 `(team-1)%2` 混淆。

### 模块 9：掉包 Top3 抢包花名册 (Loose-Bomb Fetch Roster)
- **原生全员哄抢 (Swarm on Loose Bomb)**：
  - 原生 `CCSBot::NoticeLooseBomb()` 只要地上有包即返回 true（`cs_bot.cpp:428`），`Idle` 与 `Hunt` 两条路闻到就 `FetchBomb()`，掉包即全员送死。且虽然原生调用方在外层有 T 阵营检查，但查询函数本体未作阵营约束。
- **Pre 门控 + 事件花名册 (Pre Gate & Event Roster)**：
  - 本模块 Pre 钩上述查询函数：CT 一律回 false 实施纵深防御兜底（视力 `CanSeeLooseBomb` 与捡包听觉不受影响）；T 非名册成员直接回 false 并计入拦截（遥测 `gated`）；名册为空时全部放行回原生（fail-open）。
  - 掉包事件递延一帧（等 C4 归属落定，当场查恒为"人手里"）后，以无主 `weapon_c4` 坐标为圆心、无损实测（`BotNavWalkDistance`：备份移动状态→解 repath 节流→`ComputePath` 实测→逐段累加→全量还原，调用方零副作用）真实地面行走距离取最近 3 存活 T，不可达跳过；测距未就绪则无名单直接放行（不拿欧氏混分）。成员阵亡/掉线按掉包点真实距离回补，全灭则解散回原生（防"包烂地上没人捡"死锁）；有人捡包/回合开始/换图即清。

### 模块 10：CT 首席拆包员 (Chief Defuser)
- **原生全员撞包 (Swarm on Planted Bomb)**：
  - 下包后在首人读条前，`IsDoingScenario()` 为真的 CT 全员拿 `DEFUSE_BOMB` 往雷包上撞，撞墙窗口等于全员旅行时间（`cs_bot_idle.cpp:250-281`）。
- **Scenario 门控 + 首席选举 (Scenario Gate & Chief Election)**：
  - 本模块 Pre 钩 `CCSBot::IsDoingScenario()`：下包后非首席一律回 false，掉进原生"不让拆就守包点"分支（`GUARD_BOMB_ZONE` + 包点掩体 + 自由开火），掩护组由引擎自建；首席直通原生拿 `DEFUSE_BOMB`，读上条后引擎再把众人翻成 `GUARD_BOMB_DEFUSER`。字面"提前转守拆包员"不可行（`HideState` 在无人读条时每拍弹回，会自旋），故掩护组拿 ZONE 等价任务。
   - 首席选举：下包瞬间对存活 CT 无损实测到 `planted_c4` 的真实地面行走距离（同 M9 测量器），最低者 − 持钳 500（钳子 5 秒读条差的等效让分）当选；寻路不可达跳过不参评，全员不可达/测距未就绪则无首席、全员直通回原生（不断谁的 scenario 逻辑）；首席阵亡/掉线按缓存包坐标立即重选；拆掉/炸掉/开局/换图即清。
  - 与真人玩家的关系：选举不数真人玩家（`IsBot` 限定），出动尊重原生 `bot_defer_to_human`——默认开且 CT 有活人时首席坐板凳、bot 全员掩护；真人玩家死光原生翻 true，首席自动上，无需重选；defer 置 0 则全程按设计走，真人玩家抢拆时首席在原生让路逻辑下搁置，不打架。

### 模块 11：狙击手威慑与走廊加压 (Sniper Deterrence & Corridor Danger)
- **原生缺陷 (Sniper Peeking & Retreat Lockout)**：
  - 当 Bot 遭遇敌方狙击手（`AWP` / `Scout` / `G3SG1` / `SG550`）时，原生仅通过 Chatter 报点（`SpottedSniper`），本体仍沿原生路径继续前推送命。
  - 即使想要后撤，也受限于 `AttackState` 的 `< 35 HP` 残血门控、交火 7~10 秒 `isPinnedDown` 门控以及 3~15 秒撤退冷却限制，无法在满血或半血时及时避险。
- **触发契约与武器豁免 (Trigger & Weapon Exemption)**：
  - 挂载 `BotChatterInterface::SpottedSniper` Post 钩子，纯事件驱动，0 每帧扫描，0 模拟视觉与 Raycast 开销。
  - 自身对狙豁免：若目击 Bot 自身主武器为狙击枪（`Sniper_IsSniper` 双重校验当前手持与 Slot 0 主武器），则放行原生逻辑，允许对位架枪反打。
  - 原生上游节流保护：依托引擎底层 `BotChatterInterface` 内置的 Chatter 报点冷却，天然规避密集触发，杜绝短时间频繁重算与重复走廊钉刺。
- **核心战术豁免 (Tactical Retreat Exemptions)**：
  - **下包后 CT 见狙禁撤**：C4 成功安放后（`g_bBombPlanted`），CT 阵营 Bots 见狙击手绝对不可后撤逃跑，必须争分夺秒回防进点拆包，杜绝因掩体寻路掉头跑回老家送掉对局。
   - **T 进包点后见狙禁撤**：T 阵营 Bots 进入包点范围后（`m_bInBombZone` 为真），见狙击手绝对不可后撤逃跑，必须坚守包点掩体、压制点内敌人或强行下包，杜绝拿包进点后因受击掉头缩回包点外通道。
   - **贴脸不撤（265 units 内）**：距狙小于 265 只钉走廊（团队情报照给）不请求撤退——近距离跑路等于送背，碎步停步与钉刺保留。
- **双轨目标定位与逆序回溯 (Dual-Path Threat Resolution)**：
  - **路径 1（优先交火目标）**：直接读取 `m_enemy` 句柄（$O(1)$ 查找），若已锁定且为主武器持狙的敌方，即刻确立目标。
  - **路径 2（瞬时感知队列回溯）**：若目击 Bot 尚未进入交火态（`m_enemy == 0xFFFFFFFF`），直接深入引擎内部感知队列 `m_enemyQueue`（由 `CCSBot::FindMostDangerousThreat` 每帧感知刷新），从 `m_enemyQueueIndex` 逆序回溯最近 20 帧的历史威胁实体，提取瞬间闪现的狙击手。
  - **Slot 0 主武器双重校验**：不仅检查狙击手当前手持武器（`m_hActiveWeapon`），还穿透检查 Slot 0（主武器槽），杜绝敌方狙击手因切手枪、切刀或掏雷导致的威慑漏判。
- **走廊加压与碎步打碎 (Corridor Danger & Route Shatter)**：
  - 定位到狙击手后，调用引擎底层寻路函数 `CCSBot::ComputePath(spotter, sniperPos, 0)` 计算目击者直面狙击手的视线走廊。
  - 遍历沿途最多前 20 步 (SNIPER_MAX_STEPS) NavArea 节点（从脚底起步），严格遵守 `1.0 / 120.0s` 自然衰减沉降契约，各钉刺 `+0.8f` 危险度（封顶 `2.5f`）并同步刷新时间戳，迫使后续全队寻路 A* 天然规避该交火走廊。
  - 碎步打碎冲锋：清空 `m_pathLength = 0`、`m_pathIndex = 0`、`m_isStopping = 0`、`m_pathLadder = NULL`，强行截断当前面向狙击手的送命路线。
- **脱战特权通道 (Forced Disengage Bypass)**：
  - 联动模块 5 调用 `Combat_RequestSniperRetreat(spotter)`，开启单拍后撤特权通道。
  - 穿透原生 `< 35 HP` 残血门控、清空 `m_pinnedDownTimestamp` 与 `m_retreatTimer` 冷却，使目击 Bot 在下一拍 `AttackState::OnUpdate` 时必走 `TryToRetreat()` 躲入掩体，实现“见狙立缩、封锁走廊、绕路迂回”的高拟真竞技战术行为！

### 模块 12：狙击手 Camp 门禁与高士气前压 (Sniper Camp Gate & Morale Rush)
- **原生狙击蹲点缺陷 (Static Sniper Camp Dice)**：
  - 原生引擎在 `cs_bot_idle.cpp` 中散落 6 处狙击手蹲守决策分支（进攻端固定 10.0% 概率，防守端固定 75.0% 概率）。
  - 无论战局顺逆、士气高低，概率恒定写死，且原生分支在未通过掷骰时，往往会落入消极防守分支（如原地踱步守点），无法实现高士气狙击手的主动前压搜敌。
- **旁路补丁 + DHook 借钩重骰 (Bypass Patches & OnEnter Gate)**：
  - **前端 6 处指令旁路**：原生 6 处分支中，4 处为跳过 Hide 的条件分支（O1, D1, D2, D3），打入 4 组 NOP 穿透；2 处为跳向非 Hide 备用逻辑的分支（O2, O3），打入 2 组 `E9` JMP 强制跳转，确保全员狙击手必进 `Hide()` 并带上专属任务 `MOVE_TO_SNIPER_SPOT (Task 19)`。
  - **后端借钩重骰与短路契约**：借用模块 15 挂载的 `HideState::OnEnter` Post Detour 归口拦截所有 `Task == 19` 的 Bot，按全新士气公式实时掷骰：
    $$\text{CampChance} = 60.0\% - 10.0\% \times \text{Morale}$$
    - 士气 -3（大劣势）：90% 留守掩体架枪蹲点
    - 士气 0（均势）：60% 留守蹲点
    - 士气 +3（大胜）：30% 留守蹲点（其余 70% 判定为前压）
  - 若掷骰未中决定前压，模块 12 返回 `true`，触发模块 15 短路跳过蹲守时长缩放。
- **递延一帧切状态 (Safe Deferred State Transition)**：
  - 若重骰判定不留守，利用 `RequestFrame` 递延至下一帧调用底层 `CCSBot::Hunt()`，并强制将作战姿态重写为 `ENGAGE_AND_INVESTIGATE`。
  - 彻底规避 Source 引擎状态机在 `OnEnter` 内同步切状态被外层覆盖的经典缺陷，完美实现高士气狙击手带头前冲搜敌。

### 模块 13：随机买狙击枪意愿概率 (Sniper Rifle Buy Chance)
- **原生买狙硬编码 (Static 50% Buy Chance)**：
  - 原生引擎在 `BuyState::OnUpdate`（`cs_bot_buy.cpp:522-524`）中，通过 `const float sniperRifleChance = 50.0f` 与随机数比对，决定狙击枪（`SNIPER_RIFLE`）是否列入随机主武器候选名单。
  - 劣势局极度需要长枪破局时买狙意愿不足，顺风局又频繁买狙拉低进攻节奏。
- **士气映射与双架构机器码注入 (Morale-Scaled Buy Chance)**：
  - 接入动态士气买狙公式：
    $$\text{wantSniper} = \text{RandomFloat}(0, 100) < 50.0\% - 10.0\% \times \text{Morale}$$
    - 士气 -3（大劣势）：80% 想买狙（劣势多起狙破局）
    - 士气 0（均势）：50% 想买狙（契合原生基线）
    - 士气 +3（大胜）：20% 想买狙（优势多起步枪快速突进）
  - **32 位架构（v91/v92）**：66 字节整窗汇编原地重写。巧妙复用压栈参数空间作为 `fild` 转换便笺区，精确平衡 x87 浮点栈并维持严格小于语义。
  - **64 位架构（Steam x64）**：由于 `50.0f` 是共享常量不可直改，采用 7 字节（`E9 jmp + 2 NOP`）跳转至 DLL 尾部空白对齐区（25 字节 Code Cave），在 Cave 内借用空闲寄存器 `ecx`/`xmm1` 提取 `me->GetMorale()`、计算 `cvtsi2ss` 并比较后跳回，零 SourcePawn 运行开销。

### 模块 14：开局前沿架枪概率重构 (Defend Rush Chance)
- **原生前沿架枪缺陷 (Native DefendRush Imbalance)**：
  - 原生引擎在 `IdleState::OnUpdate`（`cs_bot_idle.cpp:203/444/678/757`，四处一字不差）中，通过 `defendRushChance = -17.0f * (me->GetMorale() - 2)` 决定开局是否前往初始交火点（Initial Encounter Spot）架枪。
  - 原生公式在士气 $+2 / +3$ 时计算结果 $\le 0\%$，导致架枪机制完全坏死、全员无脑前冲送命；而在士气 $-3$ 时架枪率高达 85%，冲锋率仅 15%。
- **代数变形与内存单字节/常量双改写 (Algebraic Formula Rewrite)**：
  - 将公式重构为更均衡的竞技梯度：
    $$\text{defendRushChance} = 40.0\% - 10.0\% \times \text{Morale} \equiv -10.0 \times (\text{Morale} - 4)$$
    - 士气 -3（大劣势）：70% 前沿架枪（30% 直接前冲，优于原生 15%）
    - 士气 0（均势）：40% 前沿架枪（60% 直接前冲）
    - 士气 +3（大胜）：10% 前沿架枪（90% 直接前冲，保留极小部分侧翼警戒）
  - **内存补丁组合拳**：
    - 32 位：修改 1 处共享 `.rdata` 浮点（`-17.0f` $\rightarrow$ `-10.0f`）+ 4 处 `lea eax, [esi-2]` 立即数改为 `[esi-4]`（`FE` $\rightarrow$ `FC`）；
    - 64 位：修改 1 处共享 `.rdata` 浮点（`-17.0f` $\rightarrow$ `-10.0f`）+ 4 处 `sub eax, 2` 立即数改为 `sub eax, 4`（`02` $\rightarrow$ `04`）。
  - **动态常量寻址与共池断言**：浮点地址从 Site 1 指令操作数动态提取（64 位 RIP 相对寻址），并断言 Site 2~4 指向同一物理常量池，自带 Fail-Safe 全量回滚。T 阵营 C4 携带者原生已在前置逻辑分流，无任何被误伤风险。

### 模块 15：Hide 蹲守时长士气缩放 (Hide Duration Morale Scaling)
- **全局时长单 Choke 点拦截 (Universal Single-Choke Detour)**：
  - 原生引擎中所有躲藏与架枪行为（前沿架枪、守包蹲点、狙击点位卡点）最终均汇聚至 `HideState::OnEnter`（`cs_bot_hide.cpp:22-31`）。原生在此处将无限蹲守（`-1.0f`）归一化为 `RandomFloat(30.0f, 60.0f)`。
- **Post Detour 士气动态伸缩 (Morale Duration Scaling)**：
  - 挂载 `HideState::OnEnter` 的 **Post Detour** 钩子，从结构体成员变量（32位 `+0x1C`，64位 `+0x24`）读取归一化后的 `m_duration`，按士气线性缩放后直接写回内存：
    $$\text{m\_duration} \mathrel{*}= (1.0 - 0.15 \times \text{Morale})$$
    - 士气 -3（大劣势）：$1.45\times$ 时长（蹲守拉长 45%，沉着冷静稳守反击）
    - 士气 0（均势）：$1.00\times$ 时长（维持原生节奏）
    - 士气 +3（大胜）：$0.55\times$ 时长（蹲守缩短 45%，快速破架转入进攻）
  - **时序完美吻合**：引擎原生在 `OnEnter` 阶段只写入变量，真正的倒计时计时器是在 Bot 跑到位后（`range < 20.0f`，`cs_bot_hide.cpp:458`）才在 `OnUpdate` 中由 `m_hideTimer.Start(m_duration)` 触发，因此 Post Detour 的修改 100% 及时且全局生效。
  - 系数区间 $[0.55, 1.45]$ 严格恒正，零浮点下溢，且遇脚步声或受击时引擎事件自打断机制完好保留。
- **承载模块 12 宿主钩与短路保护 (Host Detour for Module 12 & Short-Circuit)**：
  - 模块 15 的 `HideState_OnEnter_Post` 钩子作为单一归口，在缩放时长前首先调用 `Module12_CampGate(client)`；
  - 若模块 12 判定狙击手高士气前压重定向至 `Hunt`，则立即短路返回 `MRES_Ignored`，跳过无意义的时长缩放计算与内存覆写。

---

## 目录结构

```
addons\sourcemod\
├── gamedata\
│   └── botroutefix.gamedata.txt      签名、地址与内存偏移声明
├── plugins\
│   └── botroutefix.smx               编译产物
└── scripting\
    ├── botroutefix.sp                主插件入口（全局生命周期协调与模块加载）
    └── BotRouteFix\                  模块化分层实现
        ├── global.inc                全局常量、宏定义、环境架构识别与公共状态
        ├── memory.inc                底层内存安全补丁抽象与字节序列写入管理 (MemoryPatch)
        ├── helpers.inc               Bot 实体判定、Morale 属性与通用内存数据读取
        ├── sdkcalls.inc              引擎级虚函数/静态函数底层调用封装 (SDKCall Hub)
        ├── nav_measure.inc             真实地面行走距离无损测量子系统 (NavMeasure，M9/M10 共用)
        ├── events.inc                游戏事件监听与多模块生命周期同步分发 (Events Hub)
        ├── commands.inc              控制台指令、在线遥测与模块调试工具
        ├── module1_defense.inc       [模块 1] CT 守点概率重构 (70-10*M) 与防前冲 (Anti-Rush)
        ├── module2_bombsite.inc      [模块 2] T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
        ├── module3_danger.inc        [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
        ├── module4_hunt.inc          [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
        ├── module5_combat.inc        [模块 5] 战斗决策重构 (AttackState 闪避 100% 强制、换弹进掩体 100% 强制、残血/一打多强制后撤)
        ├── module6_tmorale.inc       [模块 6] T 阵营士气接战策略与守包听觉出击梯度 (TMorale)
        ├── module7_escort.inc        [模块 7] T 包匪贴身保镖花名册 (Carrier Escort Roster)
        ├── module8_queue.inc         [模块 8] 门口礼貌排队打碎与死锁强切 Hunt (Polite Queue Breaker & Deadlock Hunt Transition)
        ├── module9_loot.inc          [模块 9] 掉包 Top3 抢包花名册 (Loose-Bomb Fetch Roster)
        ├── module10_defuse.inc       [模块 10] CT 首席拆包员 (Chief Defuser)
        ├── module11_sniper.inc       [模块 11] 狙击手威慑与走廊加压 (Sniper Deterrence & Corridor Danger)
        ├── module12_campgate.inc     [模块 12] 狙击 camp 概率重构 (60-10*M, OnEnter 重骰门)
        ├── module13_sniperbuy.inc    [模块 13] 随机买狙概率重构 (50-10*M, 汇编补丁)
        ├── module14_defendrush.inc   [模块 14] 开局前沿架枪概率重构 (40-10*M, defendRushChance)
        └── module15_hideduration.inc [模块 15] Hide 蹲守时长士气缩放 (m_duration *= 1-0.15*M)
```

---

## 控制台指令与参数

### ConVar
| ConVar | 默认值 | 说明 |
| :--- | :--- | :--- |
| `sm_botroutefix_version` | `"2.0.0"` | 插件版本（FCVAR_NOTIFY） |
| `sm_botroutefix_enable`  | `"1"`     | 是否启用 BotRouteFix 战术重构 |
| `sm_botroutefix_debug`   | `"0"`     | 是否开启详细调试日志与遥测输出 |

### 控制台指令
| 指令 | 权限 | 说明 |
| :--- | :--- | :--- |
| `sm_botroutefix_status` | 所有人 | 15 模块健康行（ACTIVE/INACTIVE，缺签名显示待机原因）+ 累积计数器（认领/闪避/后撤/拦截/打碎/选举/首席/狙击撤退/狙击前压重定向/蹲守时长缩放，单调递增、换图不清零）+ debug 专属 `[Live]` 段（活包匪士气、glBomb 状态、T/CT 平均士气与守点率，需 `sm_botroutefix_debug 1`） |

---

## 架构备忘与已知技术债务 (Technical Debt & Notes)

### 引擎全局单例指针跨签名解析 (Singleton Cross-Signature Resolution)
- **涉及的核心单例**：
  1. `TheCSBots`（`CCSBotManager*` 引擎全局单例指针）：用于管理全队 Rush 状态、全图包点列表 `m_zones` 以及运包时钟 `m_earliestBombPlantTimestamp`。
  2. `TheNavAreas`（`CUtlVector<CNavArea*>` 引擎全局导航网格容器）：用于全图 Nav 节点遍历与危险度衰减。
- **现状与跨模块借用链路**：
  - **模块 2 借用模块 1 签名**：模块 2（[module2_bombsite.inc](addons/sourcemod/scripting/BotRouteFix/module2_bombsite.inc)）为了获取 `TheCSBots` 指针，借用了模块 1 的特征码 `"IdleState_DefenseRush"`，从中提取 `mov eax, [TheCSBots]`（32位）或 `mov rax, cs:TheCSBots`（64位 RIP 相对偏移）。
  - **模块 3 借用模块 4 签名与暴力扫码**：模块 3（[module3_danger.inc](addons/sourcemod/scripting/BotRouteFix/module3_danger.inc)）为了获取 `TheNavAreas` 容器，64 位下借用了 `"CCSBotManager_GetRandomArea"`；32 位下则借用了模块 4 的特征码 `"HuntState_NavArrayAnchor"`（实为 `HuntState::OnUpdate`），并在代码段内向后暴力循环扫描 0x700 字节以命中 `8B 0D <Base> 8B 0C 91`。
- **架构坏味道与潜在风险**：
  - **隐式跨模块依赖**：模块 2 隐式依赖模块 1 的 Gamedata 条目，模块 3 隐式依赖模块 4 的 Gamedata 条目。若未来单独拆分或修改某一模块签名，可能导致其他业务模块无法定位单例。
  - **32 位扫码脆弱性**：代码段暴力扫描受编译器机器码排布与版本迭代影响，具有隐性脆弱点。
- **现状保留决策与考量**：
  - 目前该套解析机制在 CS:S 现行全平台版本（v91/v92 x86 及 Steam x64）中经过长时间测试，运行稳定可靠。
  - 单例指针仅在插件载入时解析一次并缓存在 [global.inc](addons/sourcemod/scripting/BotRouteFix/global.inc) 的全局变量（`g_pTheCSBotsPtr`、`g_pTheNavAreasBase`、`g_pTheNavAreasCount`）中，运行时各模块只读消费，性能开销为 0。
  - 经评估，为了保持当前版本测试基准的绝对稳定，避免引入无必要的重构扰动与时序风险，**暂不作代码重构，特在此归档备忘**。未来若需彻底解耦，可建立独立的 `singletons.inc` 基础设施层在 `OnPluginStart` 首步统一解析，并为 Gamedata 分离出纯净的独立定位锚点。

### 模块 1 与模块 2 核心安全熔断与残留锁启动机制 (Fuse-Protected Stale Lock & Zero-Write Prevention)
- **背景与崩服隐患**：在模块 1（防前冲补丁）与模块 2（开局包点锁定）中，若旧版本会话异常退出、崩溃残留，或管理员在带锁/已打补丁状态下热重载插件，目标内存代码段可能已处于被修改状态。若此时盲目将其判定为已打补丁并允许后续 `Restore()` 执行，因缺失原始未修改时的基线备份，会将未初始化的全零（`0x00`）写回服务端可执行代码段，直接导致非法指令段错误崩溃。
- **现行熔断方案**：
  1. `memory.inc` 注入核心熔断：`Restore()` 强制校验 `if (!this.hasBackup)`，无合法基线备份时坚决拒绝写回代码段，输出 `LogError` 告警并安全拦截。
  2. 模块 1 与模块 2 启动幂等分支置 `isPatched = false; g_bBombsiteLocked = false;`。
- **运行时行为与代价**：
  - 在残留锁状态下启动时，补丁依然可通过裸写机制正常生效，但在卸载或还原时，由于无原始基线可写，`Restore()` 会被熔断拦截（输出一条防护日志），避免全零写入。
  - 此行为保证了绝对零崩服，且代码段状态在换图或重启服务器后自动彻底恢复，行为完全安全可控。

### 模块 3 换图网格重构与边界校验 (Module 3 Map Reload & Container Re-Resolution)
- 地图切换时，引擎将销毁旧图 NavArea 并重建导航网格。模块 3 在 `OnMapStart` 彻底重置危险度软衰减标记（`g_bDecayedThisRound`），并在首回合重新解析 `TheNavAreas` 容器首地址与计数。
- 在 64 位环境下，解析出区域总数后增加防越界安全校验（`0 < areaCount <= 100,000`），若遇异常网格数据则优雅告警并跳过软衰减，坚决杜绝野指针越界遍历引发崩溃。

### 模块 4 降级认领模式与多版本补丁自愈 (Module 4 Flush-Only Fallback & Alt Patch Offset)
- 模块 4 核心逻辑建立在最久未清理（oldest-scan）保护与时间戳认领机制之上。在 64 位或未预期编译变种下，若随机覆盖 NOP 补丁匹配失败，模块 4 自动降级为纯净的“仅认领刷新模式 (flush-only mode)”，核心多路分流依然有效且绝不崩溃。
- 32 位环境下，不同引擎构建（如 v91 与 v92）在 `HuntState::OnUpdate` 内部存在约 3 字节的代码位移，Gamedata 现已提供主备双偏移配置（`HuntState_RandomBypass_PatchOffset` 与 `_Alt`），模块 4 在启动时自动嗅探匹配，实现自愈式无缝热适配。

### 模块 12 借用模块 15 单 Detour 协作契约 (Module 12 Detour Borrowing Contract)
- 模块 12（狙击 Camp 门禁）与模块 15（Hide 蹲守时长缩放）在底层均需要拦截 `HideState::OnEnter`。为杜绝在同一虚函数/函数签名上挂载双重 Detour 导致的钩子递归重入、执行顺序不确定及调用栈消耗，模块 12 不单独注册 Detour，而是直接借用模块 15 的 Post Detour 作为执行宿主。
- 协作契约：在模块 15 Detour 入口首先调用 `Module12_CampGate(client)`；若模块 12 判定为高士气前压并递延重定向至 `Hunt`，则返回 `true`，模块 15 随即短路返回，跳过无意义的时长缩放计算，各模块职责清晰解耦。

### 掉线自愈与残局热重载说明 (Disconnect & Mid-Round Hot-Reload Parity)
- **掉线机制**：`OnClientDisconnect` 直调三家花名册——模块 7（包匪掉线全放、保镖掉线除名递补）、模块 9（队员掉线按掉包点回补）、模块 10（首席掉线按包坐标接力）。开局持包者意外掉线时，引擎原生还会将 C4 强制掉落触发 `bomb_dropped`，模块 2 藉此换人自愈并恢复原生就近选点，双保险。
- **残局热重载状态同步（已显式移除）**：旧版曾在 `OnPluginStart` 中使用 `SyncMidRoundState` 探查当前残局下包与地面 C4 状态。模块化重构时已正式移除该复杂检测。若管理员在下包残局中途热重载插件，新插件实例不会在中途补发决策状态，将在下一回合 `round_start` 时完全自然自愈。

### 模块启动刚性容错策略 (Fail-Fast Startup Contract)
- 主插件入口（`botroutefix.sp`）对全部 15 个业务模块的 `InitModule*()` 实施严格的刚性链式返回值检查。
- 若因 Gamedata 签名失效、偏移未命中或二进制模板不匹配导致任何一个核心模块初始化失败，插件将立即调用 `SetFailState` 终止启动并报错，坚决杜绝“带病运行”导致的隐蔽战术逻辑割裂。例外：模块 8/10/11 在签名缺失时优雅待机（`INACTIVE (Pending Signature)`，不阻断全库启动）；模块 9 名册为空时行为回退原生（fail-open）；无损测距（NavMeasure）偏移缺失时整体待机，模块 9/10 自动降级为 fail-open/无首席而非杀插件。
