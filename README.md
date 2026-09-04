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
    - 32 位（11 字节）：`mov eax, <Zone*> + 6x NOP`
### 模块 3：CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
- **危险度结算与沉降契约 (Settlement Contract)**：
  - 原生引擎危险度衰减速率为 `1.0 / 120.0s`（`nav_area.cpp:4139`），采用惰性读衰减（Lazy Decay）机制，内存裸值陈旧。
  - 本模块严格遵守沉降契约：读取裸值后先结算自然流逝时间差 `effDanger = raw - (curtime - timestamp) / 120.0`。
- **开局 80% 软衰减 (20% 残留记忆)**：
  - 每回合开始（`round_start`）时，全图遍历 `TheNavAreas` 容器，对所有区域危险度执行 80% 衰减（`effDanger * 0.2`），并将更新时间戳重置为当前游戏时间。
  - 既清除了上一回合交火留下的严重路线偏见，又保留了 20% 残余记忆，使 Bot 在新回合开局寻路时具备动态且富于变化的战术权衡。

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

### 模块 5：战斗态规避机动与掩体换弹 (Combat Overhaul: 100% Dodge & Reload Hide)
- **闪避判定 100% 强制 (AttackState Dodge Forced)**：
  - 原生引擎在 `AttackState::OnEnter` 中根据 `dodgeChance = 80.0f * profile->GetSkill()` 掷骰判定是否闪避（`cs_bot_attack.cpp:115-130`），普通与低难度 Bot 极少闪避，对枪如同呆立木桩。
  - 本模块通过 DHook 拦截 `AttackState::OnEnter` 的 Post 阶段，直接将 `m_shouldDodge` 字段强制写为 `1`（32位 +50，64位 +58），确保全员交火时必进 `Dodge()` 规避机动，后续动作灵敏度交由 `Dodge()` 内部按 skill 分化。
- **换弹进掩体 100% 强制 (Reload HideChance NOP)**：
  - 原生引擎在 `cs_bot_weapon.cpp:1186-1210` 中，换弹时依据 `hideChance = 25.0f + 100.0f * profile->GetSkill()` 掷骰看脸决定是否寻找掩体。
  - 本模块将掷骰失败跳过掩体的条件短跳转指令（32位 2字节 `76 ??`，64位 2字节 `73 ??`）NOP 穿透为 `90 90`，抹去开阔地看脸原地换弹的弊端，只要满足前置条件（附近有敌且 5s 内见敌）换弹必进掩体！
- **残血强制后撤 (Low-Health Forced Retreat via PinnedDown)**：
  - 原生引擎在 `AttackState::OnUpdate` 撤退段中受 `isPinnedDown`（交火持续 7~10 秒）与自带 3~15 秒冷却约束，导致残血 Bot 往往死战至死。
  - 本模块通过 DHook 拦截 `AttackState::OnUpdate` 的 Pre 阶段，当检测到 Bot 血量 $< 35\text{ HP}$ 时，直接将 `m_pinnedDownTimestamp` 写为 `0.0`（使 `isPinnedDown` 恒真）并清空撤退计时器冷却；下一帧引擎原生立即触发 `TryToRetreat()` 寻找就近掩体撤退并转入 `HideState`，显著提升竞技残局生存率！

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
        ├── events.inc                游戏事件监听与多模块生命周期同步分发 (Events Hub)
        ├── commands.inc              控制台指令、在线遥测与模块调试工具
        ├── module1_defense.inc       [模块 1] CT 守点概率重构 (70-10*M) 与防前冲 (Anti-Rush)
        ├── module2_bombsite.inc      [模块 2] T 运包时钟士气映射与开局包点随机锁定 (Bombsite & Plant Clock)
        ├── module3_danger.inc        [模块 3] CNavArea 危险度开局软衰减 (NavArea Danger Soft-Decay)
        ├── module4_hunt.inc          [模块 4] HuntState 搜敌机制重构 (45秒老家破除、时间戳认领与随机覆盖 NOP)
        ├── module5_combat.inc        [模块 5] 战斗决策重构 (AttackState 闪避 100% 强制与换弹进掩体 100% 强制)
        └── module6_tmorale.inc       [模块 6] T 阵营士气接战策略与守包听觉出击梯度 (TMorale)
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
| `sm_botroutefix_status` | 所有人 | 查看当前系统运行架构 (32/64位)、Gamedata 状态及各模块补丁生效情况 |

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

