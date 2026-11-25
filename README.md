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
- **后撤档位三档制 (Retreat Tiering: Hard / Soft / Pass)**：
  - 所有后撤触发（残血、孤身一打多、狙击威慑）共用同一套"身份 × 阶段"档位框架，裁决权唯一收敛在本模块：
    - **硬豁免**：不施加强制后撤，直接放行交火（使命动作的临界态；引擎原生撤退判定仍保留）；
    - **软豁免**：照常撤，但经模块 15 的 `Module15_RequestShortHide` 把蹲守压到 1 秒（跑进掩体、到点即起）；
    - **放行**：照常撤，Hide 时长交由模块 15 按士气缩放。
  - **下包前（T 有使命）**：
    - 包匪：`Task == PLANT_BOMB && m_bInBombZone`（持包者已进包点触区，下一拍下包）→ 硬；其余情况 → 软；
    - 捡包名册（模块 9）成员：软；保镖（模块 7 名册）成员：软；
    - 其他 T：残血 / 一打多源 → 放行；**狙击威胁源 → 软**（下包前所有 T 一律短蹲，包点内包匪除外——已被上面的硬豁免拦下）。
  - **下包后（CT 有使命）**：
    - `Task == DEFUSE_BOMB` → 硬；其他 CT → 软（三源同构）；全 T：放行。
    - 该判定跟随原生任务语义：模块 10 开启时仅 Top-3 拆包名册成员（及尚未进入 750 码半径的远端 CT）持有 `DEFUSE_BOMB`；模块 10 关闭/FAILED（钩子不可用）时全员原生持有，硬豁免自动覆盖所有 CT，不会出现"无名册即无硬豁免"的真空（例外：`bot_defer_to_human` 开启且队内有活人时，bot 可能拿不到该任务，回落软豁免）。
  - 注：`m_bInBombZone` 仅在持包者进入 bomb target 触发区时置位（`func_bomb_target.cpp:50-52`），天然等价"下一拍下包"；CT 无 C4 该标记恒 false，故 CT 改用拆包任务判定。
  - 消除包点内下包临界态的折返跑死循环；包点外的包匪仍保留短蹲后撤的保命机动。
- **孤身一打多强制后撤 (Outnumbered Forced Retreat via SDKCall)**：
  - 原生 `CCSBot::IsOutnumbered() = 友 < 敌-1`（`cs_bot.cpp:606-621`，自己不占名额，1v2 起算），计数读 3 秒视觉记忆表而非距离圈；原生只走 c 支骰子/aggr，高侵略 Bot 近乎不撤。
  - 本模块同一 Pre 钩子直调原生判定，确认被包后再读 `m_nearbyFriendCount`：视野内有队友（2v3/2v4）放行交火，仅计数为 0 的真 1v 多才写同一两处内存焊门（1、2 号门焊死，3 号门"有人瞄着"仍归原生）。
  - **完全不走节流阀**：一对多属于高危态，完全脱离节流阀限制；档位仍按上面的矩阵裁决。
- **狙击手威慑联动接口 (Module5_RequestSniperRetreat + TTL)**：
  - 供模块 11 调用；请求带 2 秒 TTL，Bot 未在窗口内进入 `AttackState` 则自动作废，杜绝跨状态残影误触发；
  - 命中请求时完全穿透 `< 35 HP` 残血门控与节流阀，赋予单拍立即后撤特权；最终是硬豁免/短蹲/放行由本模块按矩阵统一裁决，模块 11 不再自行判任务与包点豁免。

### 模块 6：噪声出击统一梯度 (Noise Interception Gradient)
- **纯 DHook 入口拦截（Pre on `CCSBot::InvestigateNoise`）**：
  - 原生引擎任何战区的枪声广播（2000码）都会诱使 Bot 产生 30%~60% 几率调头去凑热闹；但 Defuse/Plant/Move 等关键任务在引擎层根本不调本函数，进钩子的只有巡逻推进与掩体架枪两类 Bot——因此不判任务、不判阵营、不判下包状态，进来的 Bot 一律按实时士气走同一套梯度（下包前/后、T/CT 全员一致）。
  - 低士气（Morale <= -1）：直接拦截（等效强制 OPPO），一律不调查；
  - 非负士气（Morale >= 0）：按士气阶梯约束最大可调查声音距离：
    - 士气 0 -> 最大 500.0 码
    - 士气 +1 -> 最大 1000.0 码
    - 士气 +2 -> 最大 2000.0 码
    - 士气 +3 及以上 -> 最大 3000.0 码
  - 超距或线性概率衰减（1.0 - dist / maxDist）未命中时，清空声音时间戳并直接 `MRES_Supercede` 拦截，彻底瓦解战火黑洞效应！命中则放行。
- **下包 OPPO 守卫（Plant Bomb OPPO Guard）**：
  - 对 M7 开路先锋、`plantDelay` 到期后（`GetGameTime() >= earliest`，或时间戳不可用）的 C4 持包者，以及 `task == PLANT_BOMB`（含到期前提前进点情形），通过 `CCSBot::SetState` Post 与 `AttackState::OnUpdate` Pre 双面守卫补写 `DISP_OPPORTUNITY`（对标 CT 原生 `DEFUSE_BOMB` 的 OPPO）；到期前顺风控图阶段保留原生听声与伏击，到期后与先锋对称统一按角色兜底，不再因 `FOLLOW`、`MOVE_TO_LAST_KNOWN_ENEMY_POSITION` 等 Task 切换脱保并落入 33.3% Ambush Hide 蹲坑 3~15 秒断档；
  - 两项守卫边界：真正埋包动作（`PlantBombState`）的 `SELF_DEFENSE (2)` 原样保留；`GUARD_TICKING_BOMB` 坚决不碰（T 守包人保持听声出击梯度）；
  - 遥测更新为 `total blocked, allowed, oppo-guarded` 进 status M06 行。

### 模块 7：T 包匪贴身保镖花名册 (Carrier Escort Roster)
- **编制随士气浮动 (Morale-Scaled Roster)**：
  - 原生引擎没有包匪专职护送概念，持包者与单打独斗无异。
  - 本模块按持包者士气配给保镖：`保镖数 = 3 - Morale`（钳制在 0~6 名）。士气 -3（绝境）：6 名抱团强推；士气 +1（常规）：2 名随行；士气 +3（大优）：0 名，全员放飞控图。
- **欧氏 3D 选人与差量刷新 (Euclidean Pick & Diff Refresh)**：
   - 保镖人选按包匪坐标欧氏 3D 平方距离（含 Z，跨楼层天然排后）取最近者；用 A* 真实距离精度更高，但在 10v10 开局每回合 O(N) 次寻路是可观卡顿，故以欧氏换零开销（同层排序精确，跨楼层自动靠后）。
  - 名单只做差量更新：换包匪才解散重配；保镖阵亡/掉线立即除名并按实时士气多退少补，转队在下一次 `Escort_Refresh()` 时除名，不抖散整条跟随链。
- **包离手即解散 (Release on Loose)**：
  - 掉包、下包、拆包、爆炸、包匪阵亡/掉线时全体解散（发 `StopFollowing` 恢复自由身，可就近捡包或转入守点）；回合开始静默清零。
- **开局 meme 传染源切断 (FollowMeme Ret)**：
  - 原生 `cs_bot_move_to.cpp:227` 包匪出门即广播 `GoingToPlantTheBomb`，`BotFollowMeme::Interpret`（`cs_bot_chatter.cpp:170`）对 1000 码内全员 `receiver->Follow(sender)`，无人数上限——M7 刚配平的名册瞬间被冲成跟包长龙。
  - 本模块在 `BotFollowMeme::Interpret` 入口写 ret（64 位 1 字节 `C3` / 32 位 3 字节 `C2 08 00`，thiscall 被调方清栈、裸 `C3` 会坏栈），meme 全员盲从链物理灭活；该 meme 唯一挂载点就是开局广播，无线电与其他 meme 不受影响；M7 自己的 `BotFollow` SDKCall 走直调，不受影响。
  - 补丁归属护送事项：签名缺失/入口 opcode 不符/写入失败 → 事项 FAILED（名册无补丁守不住，不降级）；停用/卸载原样还原。
  - `item_pickup` 在冻结期被忽略；模块启用/热重载时会立即按当前持包者配一次保镖（可能打断买枪），中途捡包递延一帧等槽位归属落定。

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
  - 原生 `CCSBot::NoticeLooseBomb()` 仅在拆弹场景且存在无主 C4 时返回 true（`cs_bot.cpp:428`），`Idle` 与 `Hunt` 两条路闻到就 `FetchBomb()`，掉包即全员送死。且虽然原生调用方在外层有 T 阵营检查，但查询函数本体未作阵营约束。
- **Pre 门控 + 事件花名册 (Pre Gate & Event Roster)**：
  - 本模块 Pre 钩上述查询函数：CT 一律回 false 实施纵深防御兜底（视力 `CanSeeLooseBomb` 与捡包听觉不受影响）；T 非名册成员直接回 false 并计入拦截（遥测 `gated`）；运行时无候选（全员不可达/无人存活）则放行，这是运行时无对象，不是后端 fail-open。
  - 掉包事件递延一帧（等 C4 归属落定，当场查恒为"人手里"）后，以无主 `weapon_c4` 坐标为圆心、无损实测（`BotNavWalkDistance`：备份移动状态→解 repath 节流→`ComputePath` 实测→逐段累加→全量还原，调用方零副作用）真实地面行走距离取最近 3 存活 T，不可达跳过；测距未就绪时 M9 事项 FAILED，钩子不挂载，行为回原生。成员阵亡/掉线按掉包点真实距离回补，全灭则解散回原生（防"包烂地上没人捡"死锁）；有人捡包/回合开始/换图即清。
  - **蹲守者解蹲 (Camp Unstick)**：当选/回补时 task==19（`MOVE_TO_SNIPER_SPOT`，蹲守承诺态）的强制 `Hunt()` 一次——蹲坑 Hide 里到不了 `NoticeLooseBomb`（Idle 的 loose 检查排在狙击手守点/架枪掷骰之后，M12 再把 task-19 的 Hide 留守），不踢名册就是摆设；Hunt 落 `SEEK_AND_DESTROY`，M12 不认，交接干净。`Hunt` SDKCall 缺失则跳过（名册照转被动门禁）。残血等其他 task 的长蹲 task 看不出来（Hide 不改 task），靠 Hide 到期自愈， observed 了再补 M15 蹲守时长表。
  - **名册抢包手 OPPO 守卫（Fetch OPPO Guard）**：M9 限制了抢包人手，必须保证人手质量；由 `CCSBot::SetState` Post、`AttackState::OnUpdate` Pre 与选举/递补入口直接按 `Module9_IsInRoster` 身份守护（直接内存覆写 `m_disposition` 为 `DISP_OPPORTUNITY`，不经 `CCSBot::SetDisposition`，完整保留 `m_ignoreEnemiesTimer` 撤退/换弹掩护计时，无需排除 `HideState`），杜绝抢包手在途、交火脱视或非退避藏身途中听声分心与蹲伏伏击；遥测 `%d/%d roster | total %d elected, %d gated, %d oppo-guarded` 进 status M09 行。

### 模块 10：CT Top-3 拆包花名册 (Defuse Roster)
- **原生全员涌包 (Swarm Before Defuse Begins)**：
  - 下包后在首人读条前，`IsDoingScenario()` 为真的 CT 全员拿 `DEFUSE_BOMB` 沿最速路线往同一雷包坐标推进（直到第一个人读上条后其余才在 `MoveTo:170` / `Idle:257` 翻成 `GUARD_BOMB_DEFUSER`）。
- **Scenario 门控 + Top-3 花名册 (Scenario Gate & Top-3 Roster)**：
  - 本模块 Pre 钩 `CCSBot::IsDoingScenario()`：下包后近端（距包 `<= 750` 码）的非名册成员一律回 false，掉进原生"不让拆就守包点"分支（`GUARD_BOMB_ZONE` + 包点 750 码内真掩体 + `OPPORTUNITY_FIRE`），掩护组由引擎自建；远端（距包 `> 750` 码）非名册 CT 与名册内 Top-3 成员直通原生（`> 750` 码必须放行，否则 `HideState:147` 无法唤醒他处蹲守 CT，且 `GUARD_BOMB_ZONE` 到期后会在 `HideState:284` 按 `GetClosestZone(myOrigin)` 锁死在原半区继续蹲）。名册第 1 人读上条后其余 2 人由引擎原生转为 `GUARD_BOMB_DEFUSER`，而前锋受阻时 2/3 号无需等待即可直接接力拆包。
  - 花名册选举：下包瞬间对存活 CT 无损实测到 `planted_c4` 的真实地面行走距离（同 M9 测量器），按 `walk - 500*(持钳)`（钳子 5 秒读条差的等效让分）取前 3 名存活可达 CT bot；不可达跳过，全员不可达 / 测距未就绪则空名册全员直通回原生（不断谁的 scenario 逻辑）；名册成员阵亡/掉线按缓存包坐标立即就近递补，刚补入的原 `GUARD_BOMB_ZONE` 成员会在下一帧由 `HideState:147` 原生唤醒转入 `Idle()->DEFUSE_BOMB`（名册全灭同样解散回原生）；拆掉/炸掉/开局/换图即清。
  - 与真人玩家的关系：选举只计存活 CT bot（`IsTeamBot` 限定），出动尊重原生 `bot_defer_to_human`——默认开且 CT 有活人时名册成员原生同样返回 false 坐板凳、真人玩家死光后原生自动翻 true 立即接管，无需重选；defer 置 0 时若真人玩家抢拆，名册成员也会在原生让路逻辑下搁置，不打架。

### 模块 11：狙击手威慑与走廊加压 (Sniper Deterrence & Corridor Danger)
- **原生缺陷 (Sniper Peeking & Retreat Lockout)**：
  - 当 Bot 遭遇敌方狙击手（`AWP` / `Scout` / `G3SG1` / `SG550`）时，原生仅通过 Chatter 报点（`SpottedSniper`），本体仍沿原生路径继续前推送命。
  - 即使想要后撤，也受限于 `AttackState` 的 `< 35 HP` 残血门控、交火 7~10 秒 `isPinnedDown` 门控以及 3~15 秒撤退冷却限制，无法在满血或半血时及时避险。
- **触发契约与武器豁免 (Trigger & Weapon Exemption)**：
  - 挂载 `BotChatterInterface::SpottedSniper` Post 钩子，纯事件驱动，0 每帧扫描，0 模拟视觉与 Raycast 开销。
  - 自身对狙豁免：若目击 Bot 自身主武器为狙击枪（`Sniper_IsSniper` 双重校验当前手持与 Slot 0 主武器），则放行原生逻辑，允许对位架枪反打。
  - 原生上游节流保护：`CCSBot::Update` 的 `m_sawEnemySniperTimer` 提供 20 秒/每 bot 节流（`CanSeeSniper → SpottedSniper`），天然规避密集触发；`BotChatterInterface` 另有接收者侧的 60 秒警告去重。
- **职责边界与贴脸无效化 (Responsibility Boundary & Close-Range Nullification)**：
  - 本模块只负责"走廊 danger + 撤退请求"，撤退档位（硬豁免 / 短蹲 / 放行）统一由模块 5 按身份与阶段裁决，本模块不判任务与包点豁免。
  - **贴脸无效化（265 units 内）**：近距离狙击枪退化为步枪，只构成点威胁、不构成走廊威胁——不钉 danger、不碎路径、不发撤退请求，直接放行，威胁模型交回常规交火（模块 5 残血/一打多）处理。两点重叠没有走廊可钉，钉了只会污染遭遇区；该分支不计入 `requests` 遥测，避免 status 混入大量放行样本。
- **双轨目标定位与逆序回溯 (Dual-Path Threat Resolution)**：
  - **路径 1（优先交火目标）**：直接读取 `m_enemy` 句柄（$O(1)$ 查找），若已锁定且为主武器持狙的敌方，即刻确立目标。
  - **路径 2（瞬时感知队列回溯）**：若目击 Bot 尚未进入交火态（`m_enemy == 0xFFFFFFFF`），直接深入引擎内部感知队列 `m_enemyQueue`（由 `CCSBot::FindMostDangerousThreat` 每帧感知刷新），从 `m_enemyQueueIndex` 逆序回溯最近 20 帧的历史威胁实体，提取瞬间闪现的狙击手。
  - **狙击手身份判定（引擎真值优先）**：以 `CCSBot::IsSniper` SDKCall 为准（主武器槽 + `WEAPONTYPE_SNIPER_RIFLE` 类型判定，自定义狙击枪只要类型正确即被识别）；签名缺失时回退名册硬编码（AWP/Scout/G3SG1/SG550，手持 + Slot 0 双重校验，防切枪漏判）。找"哪个狙击手"仍走双轨（`m_enemy` 优先 + 威胁队列逆序回溯），判定"是不是狙击手"才走本条。
- **走廊加压 (Corridor Danger)**：
  - 定位到狙击手后，通过 `BotNavProbePathAreas()` 临时探测目击者直面狙击手的视线走廊：内部仍是 `CCSBot::ComputePath`，但先快照路径数组、路径下标、梯子指针、阻挡旗、当前/最后已知区域、目标坐标、repath 时间戳与区域进入时间，读取走廊 NavArea 后还原；**爬梯中直接跳过探测**（梯子子状态不在快照范围）。spot 遭遇点、初始遭遇区等次级状态未纳入快照，属低频可自愈的残留。
  - 该设计是必需而非优化：`CCSBot::ComputePath` 本体先 `DestroyPath()` 再写入新路径（`cs_bot_pathfind.cpp:1877-1891`），直接调用会当场篡改 spotter 当前行进路径；硬豁免目标（包匪下包临界、CT 拆包任务）不会经 M5 后撤重建路径，会被带向狙击手。
  - 遍历沿途最多前 20 步 (SNIPER_MAX_STEPS) NavArea 节点（从 Bot 当前/最后已知 NavArea 起步），严格遵守 `1.0 / 120.0s` 自然衰减沉降契约，各钉刺 `+0.8f` 危险度（封顶 `2.5f`）并同步刷新时间戳，迫使后续全队寻路 A* 天然规避该交火走廊。
- **撤退请求 (Retreat Request)**：
  - 只调用 `Module5_RequestSniperRetreat(spotter)`（带 2 秒 TTL），不碎路径、不同步切换状态。
  - 真正撤退时 `Hide` 内部会自行 `DestroyPath` / `ComputePath`；硬豁免目标（包匪下包临界、CT 拆包任务者）不会被本模块打断使命动作，是否真撤与撤多久由模块 5 统一裁决。
  - **归属**：撤退事项归 M5；M11 只统计已发出的 `requests`。走廊事项因 nav 缺失 FAILED 时，只要 Detour 仍在，请求照发，M5 自己决定退不退。

### 模块 12：狙击手 Camp 门禁与高士气前压 (Sniper Camp & Morale Rush)
- **原生狙击蹲点缺陷 (Static Sniper Camp Dice)**：
  - 原生引擎在 `cs_bot_idle.cpp` 中散落 6 处狙击手蹲守决策分支（进攻端固定 10.0% 概率，防守端固定 75.0% 概率）。
  - 无论战局顺逆、士气高低，概率恒定写死，且原生分支在未通过掷骰时，往往会落入消极防守分支（如原地踱步守点），无法实现高士气狙击手的主动前压搜敌。
- **旁路补丁 + DHook 借钩重骰 (Bypass Patches & OnEnter Gate)**：
  - **前端 6 处指令旁路**：原生 6 处分支中，4 处为跳过 Hide 的条件分支（O1, D1, D2, D3），打入 4 组 NOP 穿透；2 处为跳向非 Hide 备用逻辑的分支（O2, O3），打入 2 组 `E9` JMP 强制跳转，确保全员狙击手必进 `Hide()` 并带上专属任务 `MOVE_TO_SNIPER_SPOT (Task 19)`。
  - **后端借钩重骰与短路契约**：借用模块 15 挂载的 `HideState::OnEnter` Post Detour 归口拦截所有 `Task == 19` 的 Bot，按全新士气公式实时掷骰：
    $$\text{SniperCampChance} = 60.0\% - 10.0\% \times \text{Morale}$$
    - 士气 -3（大劣势）：90% 留守掩体架枪蹲点
    - 士气 0（均势）：60% 留守蹲点
    - 士气 +3（大胜）：30% 留守蹲点（其余 70% 判定为前压）
  - 若掷骰未中决定前压，模块 12 返回 `true`，触发模块 15 短路跳过蹲守时长缩放。
  - **名册优先（与 M9 两手抓）**：task-19 的 Hide 若命中 M9 抢包名册，跳过士气重骰直接递延 Hunt——M9 选举时的强制 Hunt 是一次性的，Idle 狙击手露营掷骰随时能把人再拐回 task-19，这边不拒收就前功尽弃；M9 无名册时本条零行为变化。
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
- **模块 5 紧急短蹲入口 (M5 Soft-Exemption Short Hide)**：
  - 提供 `Module15_RequestShortHide(client)`：一次性请求 + 2 秒 TTL，Post 中优先消费，将 `m_duration` 写为 1 秒并跳过士气缩放（单独计数）。
  - 语义：后撤动作完整保留（跑向掩体、`IgnoreEnemies` 照给），抵达藏身点后蹲 1 秒即起，供模块 5 的软豁免身份（包匪/捡包名册/保镖/CT 掩护组等）使用。请求只绑定 client + 2 秒窗口：窗口内任意 `Hide` 都会消费它；超时未消费则作废，不跨窗口污染后续 Hide。
  - **短蹲与缩放同属 M15 一件事**：M15 亲手写 `m_duration`，所以 M15 规则关闭或事项 FAILED（含 morale 偏移缺失）时，短蹲与缩放一起停，M5 的软豁免退化为普通后撤。M12 借钩只让宿主 Detour 保持挂载，不恢复 M15 的短蹲/缩放代码路径。
  - 无待消费请求时，换弹掩体的 `Hide(*spot, 0.0f)` 保持原生 0 秒；若 2 秒窗口内存在请求，换弹/守包等任意 Hide 会被压成 1 秒（已知的弱身份绑定）。士气缩放对 0.0 亦为 no-op。
- **下包前先锋与到期包匪 `Hide(-1)` 短蹲 (Pre+Post Infinite Hide Short Hide)**：
  - 利用 SourceMod / CS:S 主循环单线程且 `HideState::OnEnter` 内不递归状态切换的特性，`HideState::OnEnter` Pre 捕获原始 `m_duration < 0.0`（`-1.0f` 无限期掩体占坑重寻，原生随后会在函数体将其归一化为 `RandomFloat(30.0, 60.0)`）并做瞬时标记；Post 入口第一时间读取并清零该标记，经 M12 狙击重骰与 M5 短蹲优先消费后，在 `!g_bBombPlanted` 期间若命中与 M6 下包 OPPO 守卫同口径的身份判定（M7 开路先锋 `Module7_GetPointman()`、`plantDelay` 到期后 `GetGameTime() >= earliest` 的 C4 携带者，或提前踩点触发 `task == PLANT_BOMB` 的兜底者），则跳过士气缩放并把 `m_duration` 直接写成 1 秒。有限时长蹲（`0.0s` 换弹、`1.0s` 躲雷、`3~15s` 撤退与埋伏）以及下包后（`g_bBombPlanted == true`）Guard 守包 `Hide(-1)` 与 `plantDelay` 到期前的顺风控图期均不拦截，按正常士气缩放或短蹲执行。
- **承载模块 12 宿主钩与短路保护 (Host Detour for Module 12 & Short-Circuit)**：
  - 模块 15 的 `HideState_OnEnter_Post` 钩子作为单一归口，在缩放时长前首先调用 `Module12_SniperCamp(client)`；
  - 若模块 12 判定狙击手高士气前压重定向至 `Hunt`，则立即短路返回 `MRES_Ignored`，跳过无意义的时长缩放计算与内存覆写。

---

## 事项契约与四态 (Item Contract & Four States)

> 2026-11-02 用户裁定。**这是模块状态、失败处理与跨模块边界的唯一契约。**
> 事项 = 一个用户可见、不可分割的战术行为；它下面的所有机制、所有后端都只是这个事项的条件。
> **任一后端缺失 → 整个事项失败，不允许另一半继续执行。**

### 22 件事项

| 模块 | 数量 | 事项（底层物理机制标签，对齐 `items.inc`） |
| :--- | :---: | :--- |
| M1 | 2 | `M1 anti-rush NOP` / `M1 guard chance formula patch` |
| M2 | 2 | `M2 bombsite lock patch` / `M2 plant clock write` |
| M3 | 1 | `M3 nav danger decay writes` |
| M4 | 2 | `M4 random-overwrite NOP` / `M4 HuntState dhook writes` |
| M5 | 3 | `M5 reload-hide NOP` / `M5 dodge dhook write` / `M5 retreat dhook writes` |
| M6 | 1 | `M6 noise & OPPO dhook writes` |
| M7 | 1 | `M7 escort meme ret & SDKCalls` |
| M8 | 2 | `M8 queue breaker write` / `M8 deadlock Hunt SDKCall` |
| M9 | 1 | `M9 loose-bomb fetch gate & OPPO dhook` |
| M10 | 1 | `M10 defuse dhook gate` |
| M11 | 1 | `M11 corridor danger writes` |
| M12 | 1 | `M12 sniper camp bypass patch & gate` |
| M13 | 1 | `M13 sniper buy formula patch` |
| M14 | 1 | `M14 defend-rush formula patch` |
| M15 | 1 | `M15 hide duration dhook writes` |
| M16 | 1 | `M16 escape weapon dhook & SDKCall` |

### 四态

| 状态 | 含义 |
| :--- | :--- |
| `INACTIVE` | 模块规则关闭（主动不启用） |
| `ACTIVE` | 规则开启，全部事项已武装 |
| `DEGRADED` | 规则开启，部分事项武装、部分失败；**只有多事项模块可能出现** |
| `FAILED` | 规则开启，0 个事项武装 |

因此 M3 / M6 / M7 / M9 / M10 / M11 / M12 / M13 / M14 / M15 只可能 `ACTIVE` / `FAILED` / `INACTIVE`；
`DEGRADED` 只属于 M1 / M2 / M4 / M5 / M8。

### 归属与边界

- **谁执行最终改写，谁拥有事项。**
  - M11 只发撤退请求，最终后撤由 M5 执行 → 撤退归 M5，M11 只有走廊 danger。
  - M15 亲手写 `m_duration` → 短蹲与缩放同属 M15 一件事；M15 关闭/FAILED 时短蹲一起停。
  - M6 亲手写下包 OPPO → 下包 OPPO 归 M6，噪声钩子拦截与事件写入同属 M6 一件事（Detour / 偏移 / SDKCall 缺失即 FAILED）。
  - M9 亲手写抢包 OPPO → 抢包 OPPO 归 M9；M9 名册选举只是本事项门禁的预热（Detour / 偏移 / SDKCall 缺失即 FAILED）。
  - M10 的选举只是本模块门禁的预热，不改变引擎行为，不计事项。
  - GUARD_TICKING_BOMB 谁都不写：T 方守包听到枪声必须能出击，一刀切不合理。
- **跨模块 getter / 请求不参与本模块状态**（用户第 3 条）：M5 读 M7/M9 getter、M11 给 M5 发请求、M5 给 M15 发短蹲请求，提供方缺席只让局部不生效，不改变消费方状态。
- **M11 特例**：走廊事项因 nav 缺失 FAILED 时，只要 Detour 仍在，撤退请求仍照发；是否真退由 M5 自己的后端决定。M11 的遥测只统计 `requests`，真正 `retreats` 由 M5 统计。
- **共享钩子**：M12 借用 M15 的 `HideState::OnEnter` Host Detour。宿主可用性是 M12 事项的后端条件；宿主不可用 → M12 FAILED，且 6 处补丁不得单独驻留。M15 关闭但 M12 开启时宿主保持挂载，M15 自己的代码路径由事项武装态关闭。
- **禁止 fail-open**：后端缺失时对应事项整段不执行，不再出现“缺了还跑一半、状态还报 ACTIVE”。规则关闭后的原生行为与运行时无对象（没下包/没有可达 CT/没看到狙击手）是正常 no-op，不算 fail-open。

### 后端包含 morale

`g_iOffset_Morale` 是 M2 运包时钟、M6 噪声与 OPPO 守卫、M7 保镖人数、M12 门禁重骰、M13 买狙公式、M15 时长缩放的必需后端。缺失时对应事项 FAILED，**不允许 `GetBotMorale()` 静默返回 0 继续按均势跑**。

### 调试可视化

可视化按**数据源**而不是模块 active 决定是否绘制。M2 的下包延时光圈读引擎原生 `m_earliestBombPlantTimestamp`，M6 的接战腰环读缓存偏移 `m_disposition`——即使模块关闭/FAILED 也应显示；M2 的包点指引线在锁事项武装时连锁定包点，未锁定时连原生最近包点（M2 关闭也能看原生选点）。M4 的 Hunt 选区线走只读观察：规则关闭但后端就绪时仅 Post 钩子保持挂载做只读记录（Pre 无观测逻辑不启用），不写内存、不计数，画的是原生选区；后端缺失时钩子彻底不挂载。卸载时先彻底脱钩再释放 Detour 句柄。依赖模块自己写入状态的可视化（M5 后撤原因、M7 保镖、M8 堵路、M9 名册、M10 拆包名册）才随事项停运。

---

## 模块代码习惯 (Module Code Conventions)

> M1–M4 重构定稿。M5–M15 迁移时照此执行；本节是风格契约，不是建议。

### 分区横幅

每个模块文件按固定顺序分区（M1 按事项拆成 `ITEM n BACKEND`，其余用集合式 `ITEM BACKENDS`）：

```text
MODULE X DEFINES & GLOBALS
MODULE X ITEM BACKENDS [(called by Init [/ MapStart])]
MODULE X LIFECYCLE
MODULE X PUBLIC GETTERS [& TELEMETRY / & FORMULA]
MODULE X EVENT HANDLERS
MODULE X ... PRIMITIVES / DETOUR & HOOKS / ITEM APPLY / RESTORE BACKENDS
```

调用顺序从上往下可读：`Prep → Mount → Lifecycle → Public → Events → Hooks → Apply/Restore → Validation`。被调用者尽量出现在调用者附近（如 M4 的 `MountHuntStateDetour` 紧跟 `PrepModule4_HuntBackend`），而不是扔在文件尾。原语靠近其调用者即可，位置不限（新模块按模板放在钩子之前）。

- **引擎级常量上收 `global.inc`**：任务枚举等引擎原值只在 `global.inc` 的 DEFINES 区定义一次并注明共用方（如任务常量唯一出处，M5/M6/M12/M16 共用）；模块只消费不定义，更不得直接复用他模块文件的 `#define`（include 顺序不可靠，换序即 `undefined symbol`）。模块私有阈值留在本文件。

### 函数命名

| 角色 | 命名 | 例 |
| :--- | :--- | :--- |
| 生命周期 | `Init / Enable / Restore + ModuleX_<动词>` | `InitModule3_Danger` / `EnableModule4_Hunt` / `RestoreModule2_Bombsite` |
| 事项后端 | `Prep / Apply / Restore + ModuleX_<事项> + Backend`，一律 `static` | `PrepModule1_GuardBackend` / `ApplyModule4_HuntBackend` |
| 事件处理 | `ModuleX_On<Event>` | `Module3_OnRoundStart` / `Module2_OnRoundFreezeEnd` |
| 钩子回调 | `Hook_<目标>_<Pre/Post>`，一律 `public` | `Hook_HuntState_OnUpdate_Pre` / `Hook_AttackState_OnEnter_Post` |
| 公共读取 | `ModuleX_<动词><名词>` | `Module2_IsBombsiteLocked` / `Module4_GetTotalHuntClaims` |
| 内部公式/原语 | 描述性命名，一律 `static` | `CalculatePlantDelay` / `DecayAllNavAreaDanger` |

散查询（`IsModuleX_Active` / `Is...Patched`）不再作为公共接口；状态唯一出口是 `items.inc` + `status` 命令。

### 函数头注释

每个函数必须有头注释，且必须写清三件事：

1. 这个函数属于哪个事项（如“事项 M4 HuntState dhook writes 的唯一后端入口”）；
2. 失败语义（如“失败时该事项 FAILED，另一事项不受影响”）；
3. 确需偏离门禁的特殊情形必须显式标注（如 M3 `OnMapStart` 的“【例外：不随业务态守卫】”）；纯幂等清理依底层契约天然豁免，无需标注。

### 生命周期链（固定模板）

```sourcepawn
Init:
  GameConf 缺失 -> 全部事项 Ready=false, return false;
  PrepModuleX_<A>Backend(); PrepModuleX_<B>Backend();
  Item_SetReady(ITEM_MX_A, aReady); Item_SetReady(ITEM_MX_B, bReady);
  EnableModuleX(true);
  全灭断言: if (!g_bModuleX_Active) { LogModuleAllFailed(X); return false; }
  武装闭环: LogModuleArmed(X, <label>, <detail>);
  return true;

Enable(true):
  只武装 Ready 的事项；失败只清对应事项的 Armed，不回滚另一件；
  g_bModuleX_Active = OR(Armed)；部分成功打 Partial activation 日志（全败不打）。

Enable(false) / Restore:
  逐事项 Restore 后端 + Item_SetArmed(false)；
  Restore 卸载时销毁模块自有 Detour 句柄并输出标准确认日志。
```

- `g_bModuleX_Active` 只是 `Enable` 幂等短路的内部缓存；业务判据一律 `Item_IsArmed`。
- `Prep` 只回答“能不能用”（定位/校验/备份），`Apply` 回答“这次生不生效”（写补丁/挂钩子）；无常驻资源的事项（如 M3）可以没有 `Apply`。
- 单事项模块不可能 `DEGRADED`（如 M3）；双事项以上才可能（如 M1/M2/M4）。

### Ready / Armed 中心语义（全模块统一标准）

> 不以任何单个模块为标杆；以下定义适用于每一个模块，所有模块均以此为准对齐。

1. **事项 = 独立能力，后端链 = 该事项的全部前置条件。** 任一后端条件不满足，该事项即认定为无法运行；一事项可用、一事项不可用 → 模块 `DEGRADED`；全不可用 → `FAILED`。
2. **`Ready` = 最近一次探测结论，不是环境快照。** 探测只发生在三处：`Init`（全部事项）、环境重建点（如换图的 M3 重解）、重武装前（资源可能在此期间失效如补丁被还原）。
3. **`Prep` 只探测不注入**（定位 / 校验 / 备份）；**`Apply` 只注入不探测**（写补丁 / 挂钩子）以 `Ready` 为前置只写 `Armed`。写入失败若证伪了探测结论（如模板明明校验通过落笔却写不进去）则同步刷新 `Ready=false`；挂钩类失败（句柄有效、可待下次全关全开重试）只清 `Armed`。
4. **`Enable` 只做调度**：重探（如需）→ `Apply` → `Armed` → `Active = OR` → 部分成功打 Partial 日志。`Enable` 内部不得内联注入逻辑。
5. **按回合 / 事件生灭的资源**（如 M2 包点锁）注入点在所属事件里但必须同样以 `Armed` 为门禁等价于 `Apply`；事件里的战术写操作一律按 `Armed` 门控只读 getter 不参与。

### 事件门控

以 `events.inc` 接收方契约为准：

a) 有业务副作用的处理器，函数首行必须自守（不看模块级 `Active`）：
```sourcepawn
if (!Item_IsArmed(ITEM_M2_LOCK)) return;
```
b) 纯幂等清理（关闭态下本就是 no-op：清计数/清名单）可豁免守卫，无需逐处标注；
c) 换图必须执行的底层重建（如 M3 重解导航容器）【不得】加守卫。

可视化/调试读取是跨模块 getter，不参与事项状态（见“调试可视化”）。

### 日志用词

- `[ModuleRules]` 只说规则意图：`Rule-enabled modules` / `Rule-disabled modules`，不说 Active/Inactive。
- 事项失败用 `LogError`，必须带模块号和事项名（如 `[Module 4] ... HuntState item FAILED`）。
- `status` 行：`INACTIVE` 显示 `disabled by rule`；`FAILED` 显示 `items x/y armed (x/y ready) | failed: <事项名>`，不拼宣传性 detail；`DEGRADED` 才追加活跃事项的实时 detail。

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
        ├── items.inc                 22 件事项能力表与四态计算（唯一状态源）
        ├── sdkcalls.inc              引擎级虚函数/静态函数底层调用封装 (SDKCall Hub)
        ├── nav_measure.inc             真实地面行走距离无损测量子系统 (NavMeasure，M9/M10 共用)
        ├── events.inc                游戏事件监听与多模块生命周期同步分发 (Events Hub)
        ├── commands.inc              控制台指令、在线遥测与模块调试工具
        ├── module_manager.inc        模块规则解析与动态通断调度系统 (Module Rules)
        ├── module1_defense.inc       [模块 1] CT 守点概率重构 (70-10*M) 与防前冲 (Anti-Rush)
        ├── module2_bombsite.inc      [模块 2] T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
        ├── module3_danger.inc        [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
        ├── module4_hunt.inc          [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
        ├── module5_combat.inc        [模块 5] 战斗决策重构 (闪避 100%、换弹进掩体 100%、残血/一打多/见狙后撤与三档豁免裁决)
        ├── module6_noise.inc         [模块 6] 噪声出击统一梯度 + 下包 OPPO 守卫 (Noise Interception + Plant OPPO Guard)
        ├── module7_escort.inc        [模块 7] T 包匪贴身保镖花名册 (Carrier Escort Roster)
        ├── module8_queue.inc         [模块 8] 门口礼貌排队打碎与死锁强切 Hunt (Polite Queue Breaker & Deadlock Hunt Transition)
        ├── module9_loosefetch.inc    [模块 9] T 掉包 Top3 抢包花名册 + 抢包 OPPO 守卫 (Loose-Bomb Fetch Roster + Fetch OPPO Guard)
        ├── module10_defuse.inc       [模块 10] CT Top-3 拆包花名册 (Defuse Roster)
        ├── module11_sniper.inc       [模块 11] 狙击手威慑与走廊加压 (Sniper Deterrence & Corridor Danger)
        ├── module12_snipercamp.inc   [模块 12] 狙击 camp 概率重构 (60-10*M, OnEnter 重骰门)
        ├── module13_sniperbuy.inc    [模块 13] 随机买狙概率重构 (50+10*M, 汇编补丁)
        ├── module14_defendrush.inc   [模块 14] 开局前沿架枪概率重构 (40-10*M, defendRushChance)
        ├── module15_hideduration.inc [模块 15] Hide 蹲守时长士气缩放 + 紧急短蹲入口 (m_duration *= 1-0.15*M / RequestShort=1s)
        └── module16_escape.inc       [模块 16] 逃跑切枪纪律 (Escape Knife→Gun on Contact)
```

---

## 控制台指令与参数

### ConVar
| ConVar | 默认值 | 说明 |
| :--- | :--- | :--- |
| `sm_brf_version` | `"2.0.0"` | 插件版本（FCVAR_NOTIFY） |
| `sm_brf_modules` | `"all"`   | 模块规则：灵活配置启用哪些模块（支持 `"all"`、`"none"`、排除模式 `"-5, -13"` 或 `"all, -5"`、指定模式 `"1, 2, 5-8"`） |
| `sm_brf_debug`   | `"0"`     | 是否开启各模块内部逐行调试日志与调试可视化 |

#### 模块规则说明
通过 `sm_brf_modules` 单一参数即可灵活管理 16 个战术模块，支持以下用法：
- **默认全开**：`"all"` 或 `"*"`（1~16 号全部开启，开箱即用推荐）；
- **排除模式（做减法）**：以 `-` 开头，在全开基准上关闭指定项。如 `"-5, -13"`（全开，仅关闭模块 5 和 13），`"all, -8-10"`（全开，排除 8~10 号）；
- **指定模式（做加法）**：输入正整数或区间，仅开启列出的项。如 `"8, 9"`（仅开启模块 8 和 9，其余全关），`"1-4, 7"`；
- **一键全关**：`"none"`、`"off"`、`"0"` 或空串（16 个模块全部停用，底层补丁与钩子全量卸载回原生）；
- **从左到右顺序覆盖**：规则从左到右依次解析执行。例如 `"all, 0"` 会先全开再全关（最终全关）；`"0, 5"` 或 `"none, 5"` 会先执行全关再开启 5 号（最终仅 5 号开启）；反之 `"5, 0"` 或 `"5, none"` 最终结果为全关；
- **非法规则安全回退**：若输入无法识别任何有效模块编号（如拼写错误 `"al"`、`"abc"`、残缺的 `"1-"` 等），系统将记录一条 `LogError` 告警并安全回退为默认全开 `"all"`，彻底杜绝所有模块被静默全关；
- **空格与符号折叠**：符号与数字之间被空格隔开的写法会被自动压紧，`"- 5"` 等同于 `"-5"`，`"1 - 4"` 等同于 `"1-4"`；`"all -5"` 这类合法写法不受影响；
- **数字严格校验**：仅接受纯数字与区间，`"abc-5"`、`"5-abc"`、`"5abc"` 等包含非数字字符的写法一律视为非法并回退 `"all"`；
- **越界处理**：部分越界自动截断到 1~16（`"16-20"` 视为 16，`"0-5"` 视为 1~5）；完全越界（`"99"`、`"20-30"`、`"-99"`）视为无效 Token，若整条规则无任何有效项则回退 `"all"`；
- **容错设计**：支持逗号、分号或空格分隔，区间支持 `-` 与 `~`，输入不区分大小写，变更即刻热生效；规则日志仅在规则实际变化时打印，换图不会重复刷屏。

> 旧总开关 `sm_botroutefix_enable`（历史名）已彻底移除：事件总线不再做任何全局粗暴拦截，
> 模块启停统一收敛为各模块独立业务态（`EnableModuleX`），避免一刀切拦截对局炸弹状态与事件分发。

### 控制台指令
| 指令 | 权限 | 说明 |
| :--- | :--- | :--- |
| `sm_brf_status` | 管理员 (`ADMFLAG_GENERIC`) | 分两段：前半 16 模块启停行（`ACTIVE` 光杆 counts，`DEGRADED` / `FAILED` 点名 failed 事项居中、counts 永远挂尾，`INACTIVE` 显示 `disabled by rule`；静态士气公式在模块名里）；后半 `Live`（与上分隔行等长）：全局炸弹三态 `PLANTED` / `DROPPED` / `CARRIED by <人> (士气/身份)`（不显示包点）+ 士气两行（`T n bots alive (AVG Morale: +x 名)`，avg 只统计存活；`T Morale: [...]` 全员名单不筛存活；偏移丢失时显示 `unavailable`）+ 各模块快照行（`M01 CT Guard` / `M02 锁+延迟五变种` / `M03~M16 计数器与名册` + `M12/M13/M14` per-bot 概率名单；仅 `ACTIVE` / `DEGRADED`；per-bot 现算值不进 live）。累积计数器单调递增、换图不清零 |

### 初始化日志口径（Initialization 契约）

- **横幅定宽 74**：`Initialization Start / Complete`、status 头、分隔线、尾线，`[BotRouteFix] ` 前缀之后一律补齐到 74 列。
- **每模块 = N 个检查点行 + 1 行武装确认**（N 由后端数量决定，不强求一样多）：
  - 补丁写入点：`... verified @ %X`；幂等命中：`... already present @ %X, skipping`；
  - 只读解析点：`Resolved ... @ ...`；偏移/SDKCall 点：`... verified (...)`（无地址可打，如 detour 句柄）；
  - 纯 hook/名册：无检查点行（挂钩失败走事项 FAILED 错误行）；
  - **武装确认统一走 `LogModuleArmed`**：`[Module N] <label> items armed (<detail>) [<arch>]`。
  - `<label>` 必须为 1~2 个英文单词的标准纯短名（首字母大写，严禁带模块编号前缀、公式或长变量，全库标准：`Defense`, `Bombsite`, `Danger decay`, `HuntState`, `Combat`, `Noise`, `Escort`, `Queue`, `Fetch`, `Defuse`, `Corridor`, `Sniper camp`, `Sniper buy`, `Defend rush`, `Hide duration`, `Escape`）；
  - `<detail>`：多事项列名（如 `guard=1, anti-rush=1`），单事项成功时严格传纯数字 `"1"`（全败走 LogModuleAllFailed 拦截，永不传 `"0"` 或三元表达式）。
- **卸载日志统一口径 (Restore 契约)**：各模块 `RestoreModule*()` 在还原完成后必须统一输出 `[BotRouteFix] [Module N] <ShortName> <设施> safely restored.`（纯补丁为 `patches safely restored to original bytes.`，纯钩子为 `hooks safely restored.`，混合为 `hooks and memory patches safely restored.`，纯数据重置为 `state reset (no persistent patch).`），禁止遗漏。
- **变体**：部分武装 → `LogModulePartial`（`Partial activation (...)`）；事项全灭 → `LogModuleAllFailed`（`All items failed; module will report FAILED.`）；失败原因各事项就地 `LogError`。
- **Hook 回调命名、门禁与日志口径**：
  - 命名规范：统一为 `Hook_<目标>_<Pre/Post>[_<消歧后缀>]`，一律 `public MRESReturn`；跨模块多 Detour 冲突以业务后缀消歧（如 `_Plant`, `_Fetch`）；
  - 门禁口径：业务写入必守卫（首行或分支入口均可）；纯观测（如 M4 visual 数组）或跨模块信号（如 M11 侦察向 M5 发撤退请求）走消费方门禁；多租户共享宿主钩子（如 M15 借钩宿主）按各租户业务分支分别守卫，杜绝为字面形式强加首行粗暴短路；
  - 运行期日志：前缀统一 `[BotRouteFix] [Module N]`；`DebugLog` 只记离散事件/状态变化（选举/拾取/安放/重定向/放行/认领），不记每 tick 求值——M4 的 `HuntState::OnUpdate` pre/post 刻意零输出（高频 tick），它的可观测面是 status 的 `total claims` 计数器；`LogError` 只用于不变量破坏（如 M7 非法名册数），普通业务分支一律 `DebugLog`；新增 Hook 回调必须带一行说明决策的 `DebugLog`，不留注释掉的死代码。
- **准备期与运行期的门禁分界**：准备期输出（Prep/Mount/Apply/Restore/Enable 内的句柄创建、签名绑定、基线备份、补丁落子、`verified`/`Resolved` 确认行）走 Init 口径，**无条件输出，不受 `sm_brf_debug` 约束**；运行期离散事件（Hook 回调/事件响应内的逐条决策，如持包人变更、锁落子/还原、士气处置切换）**必须走 `DebugLog`，受 debug 约束**。M2 锁落子/还原与 M6 逐条拦截/放行的 `DebugLog` 属后者（运行期逐 noise 事件），不是准备期，门控正确。
- **各模块检查点清单**：M1 守点/反冲各 1 个写入点；M2 单例指针 + 时钟偏移 + 锁写入点；M3 危险度偏移 + nav 解析 + 状态重置；M4 随机旁路写入点 + 搜寻偏移；M5 reload 写入点 + 闪避偏移 + 后撤后端；M6 士气/噪声/Task/Disp 偏移 + 三 Detour；M7 Follow/StopFollowing SDKCalls + 士气偏移 + FollowMeme ret 补丁；M8 排队偏移 + 死锁 SDKCall；M9 State/Disp 偏移 + NavMeasure + 三 Detour；M10 无（纯 hook/名册）；M11 走廊对抗偏移；M12 蹲守偏移 + 6 个 bypass 点；M13 rewrite/cave 二选一（按架构）；M14 4 站点 + 1 常量池；M15 隐藏时长偏移；M16 EquipKnife 签名 + EquipBestWeapon SDKCall + 1 自有 Detour。

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

### 模块 4 事项级降级与多版本补丁自愈 (Module 4 Item-Level Fallback & Alt Patch Offset)
- 模块 4 有两个独立事项：随机覆盖 NOP 与 HuntState dhook 写入（45s bypass + 认领）。NOP 补丁匹配失败时，只有 NOP 事项 FAILED；dhook 事项仍可工作，模块显示 DEGRADED。不再把“flush-only”当成整个模块的降级。
- 32 位环境下，不同引擎构建（如 v91 与 v92）在 `HuntState::OnUpdate` 内部存在约 3 字节的代码位移，Gamedata 现已提供主备双偏移配置（`HuntState_RandomBypass_PatchOffset` 与 `_Alt`），模块 4 在启动时自动嗅探匹配，实现自愈式无缝热适配。

### 模块 12 借用模块 15 单 Detour 协作契约 (Module 12 Detour Borrowing Contract)
- 模块 12（狙击 Camp 门禁）与模块 15（Hide 蹲守时长缩放）在底层均需要拦截 `HideState::OnEnter`。为杜绝在同一虚函数/函数签名上挂载双重 Detour 导致的钩子递归重入、执行顺序不确定及调用栈消耗，模块 12 不单独注册 Detour，而是直接借用模块 15 的 Post Detour 作为执行宿主。
- 协作契约：在模块 15 Detour 入口首先调用 `Module12_SniperCamp(client)`；若模块 12 判定为高士气前压并递延重定向至 `Hunt`，则返回 `true`，模块 15 随即短路返回，跳过无意义的时长缩放计算，各模块职责清晰解耦。
- **底层启用态协同（`SyncHideDurationDetourState`）**：底层钩子只要任一借钩方（M9 / M12 / M15）规则开启就必须保持启用，只有全部关闭才真正 Disable。模块 12 的 `EnableModule12_SniperCamp()` 与模块 15 的业务态变更都会调用该同步函数。
- **全成或全败**：M12 唯一事项 = 6 处补丁 + 宿主钩子 + Hunt SDK + morale。宿主不可用时不武装 6 处补丁；补丁任一站点失败立即全部回滚 → M12 FAILED，绝不出现“补丁驻留但门禁不工作”的半态。

### 后撤三档制跨模块契约 (Retreat Tier Contract: M5 × M11 × M15)
- **唯一裁决点**：残血 / 孤身一打多 / 狙击威慑三大触发源的"硬豁免 / 短蹲 / 放行"全部由模块 5 的 `AttackState::OnUpdate` Pre 钩子裁决；模块 11 与 15 不感知 M5 的三档撤退身份矩阵（M15 仅在自身武装后，借助 Pre+Post 标记把下包前 M7 先锋、`plantDelay` 到期后的持包者及 `task == PLANT_BOMB` 的 `Hide(-1)` 时长压到 1 秒）。
- **M11 → M5**：模块 11 只钉走廊 danger 并调用 `Module5_RequestSniperRetreat(client)`，请求带 2 秒 TTL；命中后模块 5 穿透残血门控与节流阀，再按矩阵决定档位。
- **M5 → M15**：软豁免时模块 5 调用 `Module15_RequestShortHide(client)`（一次性 + 2 秒 TTL），模块 15 在 `HideState::OnEnter` Post 中优先消费，写 1 秒并跳过士气缩放。
- **M5 → M7/M9（只读）**：`Module7_IsBodyguard()` / `Module9_IsInRoster()` 是名册内的常数级小循环；M5 同一判定还包括一次 `GetC4Carrier()` 的 O(MaxClients) 持包扫描。后撤判定内不做寻路。
- **失效行为**：模块 10 未就绪/空名册时，CT 通常全员原生持有 `DEFUSE_BOMB`，下包后硬豁免自动覆盖所有 CT（这是原生任务语义，不是 M10 fail-open）；M10 自身状态为 FAILED。M15 规则关闭或事项 FAILED（含 morale 缺失）时，短蹲与缩放一起停，M5 软豁免退化为普通后撤（`TryToRetreat` 默认 3~15 秒，而非守包 Hide 的 30~60 秒）。M12 借钩只保持宿主 Detour 挂载，不恢复 M15 的短蹲/缩放代码路径。

### 事项状态机与 DHook 收敛 (Item State Machine & Detour Sync)
- **事项态（唯一状态源）**：`items.inc` 维护 21 件事项的 `Ready`（后端是否可用）与 `Armed`（规则开启且已武装）。状态行只读这张表：`INACTIVE / ACTIVE / DEGRADED / FAILED`。
- **模块内部门**：`g_bModuleN_Active` / `IsModuleN_Active()` 只是各模块自己的执行门（规则意图或“至少一件事项已武装”，按模块语义），不再被状态行当作 ACTIVE/INACTIVE 的依据。
- **底层态**：`MemoryPatch::isPatched` 与各 `Is...Patched()` / `Is...Hooked()` 只描述代码段字节与 Detour 真实挂载情况，是实现细节，不直接拼状态词。
- **状态机收敛**：所有 DHook 的 Enable/Disable 统一走 `SyncDetourEnabled()`（[memory.inc](addons/sourcemod/scripting/BotRouteFix/memory.inc)），先比对 per-hook 启用态再动作，从根上杜绝重复 Disable 触发 `Function not detoured` 致命异常。
- **补丁基线保留**：`MemoryPatch::Init()` 在同一目标重入时保留原始字节备份与生效旗标，避免 `Enable -> Disable -> Enable` 后自家补丁被误判为外来补丁、`Restore()` 被零写入熔断拦截而永久卡死。

### 掉线自愈与残局热重载说明 (Disconnect & Mid-Round Hot-Reload Parity)
- **掉线机制**：`OnClientDisconnect` 直调三家花名册——模块 7（包匪掉线全放、保镖掉线除名递补）、模块 9（队员掉线按掉包点回补）、模块 10（名册成员掉线按包坐标回补）。开局持包者意外掉线时，引擎原生还会将 C4 强制掉落触发 `bomb_dropped`，模块 2 藉此换人自愈并恢复原生就近选点，双保险。
- **残局热重载状态同步（已显式移除）**：旧版曾在 `OnPluginStart` 中使用 `SyncMidRoundState` 探查当前残局下包与地面 C4 状态。模块化重构时已正式移除该复杂检测。若管理员在下包残局中途热重载插件，新插件实例不会在中途补发决策状态，将在下一回合 `round_start` 时完全自然自愈。

### 模块启动与事项解析策略 (Soft Startup & Item Resolution)
- 主插件入口不再对 16 个模块做链式 fail-fast。每个 `InitModule*()` 独立执行：Gamedata 签名失效、偏移未命中或二进制模板不匹配只让对应事项 `Ready=false`，插件继续加载。
- `InitSDKCalls()` 拆成 8 个独立准备函数；缺失的 SDKCall 只让其消费方事项 FAILED（M5 后撤 / M6 下包 OPPO / M7 保镖 / M8 死锁 Hunt / M9 抢包 OPPO / M9/M10/M11 nav / M11/M12 名单 / M12 camp），不再 `SetFailState`。
- 启动阶段：16 个模块的 `InitModule*()` 解析后端后立即按 ConVar 初始值（默认 `all`）武装；`OnPluginStart` 末尾 `ApplyModulesConfig()` 再应用同一默认；`OnConfigsExecuted` 按 cfg 实际规则增量收敛。规则关闭的模块会在启动瞬间短暂武装，cfg 生效后回滚。
- 这是当前**统一**的生命周期：Init/Enable 负责武装，Apply 负责规则收敛。若未来要改成“Init 只解析、Apply 唯一武装入口”，必须 16 个模块一起迁移；不做 M1/M2 单独延迟的半迁移。
- 解析失败不会杀死其他模块；`g_bBombPlanted` 与事件总线始终运行，不随任何模块启停断流。
