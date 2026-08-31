# Bot Route Fix (CS:S Bot 寻路与战术决策底层修复插件)

针对 Counter-Strike: Source 官方 Bot AI 底层寻路缺陷与战术决策逻辑的 SourceMod 修复插件。

---

## 模块列表 (Modules)

### 模块 1：CT 守包点与防发疯前冲修复 (CT Bombsite Defense & Anti-Rush Fix)
* **状态**：**已实装并在实机验证成功**
* **逆向根因**：
  1. 在官方源码 `cs_bot_idle.cpp` 的 `IdleState::OnUpdate()` 中，CT 判定是否守包点的公式为：
     $$\text{guardBombsiteChance} = -34.0f \times \text{Morale}$$
     开局默认 $\text{Morale} = 0$，导致守包点概率恒为 **`0.0%`**。底层的条件跳转指令（64位 `jnb` / 32位 `jbe`）开局 100% 必定触发跳过，导致全队 CT 直接掉入保底的 `Hunt()` 冲向匪家送人头；
  2. 在进入守点前，引擎先检查 `TheCSBots()->IsDefenseRushing()`（每回合有 **33.3%** 的纯随机几率判定全队前冲）。一旦触发，直接在顶层执行 `me->Hunt()` 绕过守点逻辑。
* **修复机制**：
  1. 对 `guardBombsiteChance` 跳转打入 **6 字节 NOP (`90 90 90 90 90 90`)** 就地补丁，彻底激活官方原生守包点与掩体架枪体系；
  2. 对 `IsDefenseRushing` 跳转打入 **6 字节 NOP (`90 90 90 90 90 90`)** 就地补丁，彻底消除官方 33.3% 的全员无脑冲锋暴毙局；
  3. 保留个别独狼 Bot（`IsRogue`）的单兵自主性，兼顾战术纪律与战场多样性。

---

### 模块 2：T 包匪开局即刻运包与 50% 随机包点修复 (T C4 Carrier Instant Plant & 50/50 Bombsite Fix)
* **状态**：**已实装**
* **逆向根因**：
  1. 在官方源码 `cs_bot_manager.cpp` 中，每回合开局设定了下包延迟时间戳：
     $$m\_earliestBombPlantTimestamp = \text{curtime} + \text{RandomFloat}(10.0f, 30.0f)$$
     导致开局前 10~30 秒内，`IsTimeToPlantBomb()` 恒为 `false`。带包的 T 队员被禁止前往包点运包，顺流跌落到了最底部的 `me->Hunt()`，带着 C4 跟随大部队冲向 CT 出生点送人头白给；
  2. 运包时官方源码直接调用 `TheCSBots()->GetClosestZone(...)`，导致包匪每一局都死板地前往物理距离最近的包点（例如 `de_dust2` 永远只去 B 包点）。
* **修复机制**：
  1. 对 `IdleState::OnUpdate()` 中检测 `IsTimeToPlantBomb` 的条件跳转（64位 `ja` / 32位 `jb`）打入 **6 字节 NOP (`90 90 90 90 90 90`)** 就地补丁。开局冻结结束，包匪 **0 延迟** 立即启程前往包点运包安包；
  2. 对调用 `GetClosestZone` 处打入机器码补丁重定向为 `GetRandomZone`，使包匪在开局时真正以 **50% / 50%** 的概率在 A 包点与 B 包点之间随机轮换进攻。

---

## 支持架构 (Supported Architectures)

- **32-bit**：Windows non-Steam (v91/v92) `server.dll`
- **64-bit**：Windows Steam x64 (2026 最新构建) `server.dll`

---

## 逆向签名与结构体档案库 (Reverse Engineering Archive - Future Modules)

以下所有签名与成员偏移量均已在 32 位与 64 位官方 `server.dll` 中通过 IDA Pro 实机逆向，并通过 `find_bytes` 验证为 **100% 唯一匹配**，供后续分流、寻路留痕与防卡死模块直接调用。

### 1. 核心函数唯一签名 (Signatures)

| 标识符 | 32-bit 地址 | 64-bit 地址 | 对应 C++ 原型与说明 |
| :--- | :--- | :--- | :--- |
| **`IdleState_GuardBombsiteChance`** | `0x102BBE15` | `0x18036448C` | `IdleState::OnUpdate` 守包点概率跳转拦截点 (已使用) |
| **`IdleState_DefenseRush`** | `0x102BBDED` | `0x18036444C` | `IdleState::OnUpdate` 全队 33.3% 前冲跳转拦截点 (已使用) |
| **`IdleState_C4PlantDelay`** | `0x102BB882` | `0x180363E0D` | `IdleState::OnUpdate` 包匪 10~30s 延迟与选点拦截点 (已使用) |
| **`CCSBot_ComputePath`** | `0x102A2000` | `0x180343550` | `bool CCSBot::ComputePath(const Vector &goal, int route)` (A* 核心寻路) |
| **`HuntState_OnUpdate`** | `0x102BA430` | `0x180362350` | `void HuntState::OnUpdate(CCSBot *me)` (搜敌目标选择) |
| **`CCSBot_MoveToInitialEncounter`** | `0x102A6FF0` | `0x180349CF0` | `bool CCSBot::MoveToInitialEncounter()` (开局交火线推进) |
| **`PathCost_FriendDensity`** | `0x10292CB0` | `0x18032D2B0` | `PathCost::operator()` (50000.0f 队友密度代价读取点) |
| **`CCSBot_Hide`** | `0x102A6D50` | `0x180349940` | `bool CCSBot::Hide(CNavArea *area, ...)` (掩体潜伏与架枪) |
| **`CCSBot_UpdatePathMovement`** | `0x102A5000` | `0x180347330` | `int CCSBot::UpdatePathMovement()` (路径跟踪与礼貌排队) |

### 2. 结构体成员偏移量 (Offsets)

| 结构体与成员 | 32-bit (v91/v92) | 64-bit (Steam x64) | 数据类型与说明 |
| :--- | :--- | :--- | :--- |
| **`CCSBot::m_path`** | `+0x1C28` (**步长 24B**) | `+0x2108` (**步长 32B**) | 寻路节点数组 `ConnectInfo[256]` |
| **`CCSBot::m_pathLength`** | `+0x3428` | `+0x4108` | 当前路径总节点数 (int32) |
| **`CCSBot::m_lastKnownArea`** | `+0x1C14` | `+0x20F0` | Bot 当前所在的 `CNavArea*` |
| **`CCSBot::m_task`** | `+0x1BF8` | `+0x20D0` | 当前任务枚举 `BotTaskType` (1 = PLANT_BOMB, 7 = GUARD_BOMB_ZONE) |
| **`CCSBot::m_isWaitingBehindFriend`** | `+0x345C` | `+0x4150` | 遇到队友挡路的礼貌等待标志位 (bool/int8) |
| **`CCSBot::m_politeTimer`** | `+0x3450` | `+0x4140` | 礼貌等待计时器 `IntervalTimer` |
| **`HuntState::m_huntArea`** | `+0x04` | `+0x08` | 选定的搜敌目标 `CNavArea*` |
| **`CNavArea::m_center`** | `+0x2C` | `+0x30` | 区域三维几何中心 `Vector` (12B) |
| **`CNavArea::m_clearedTimestamp`** | `+0xB4` | `+0xF8` | `float[2]`，最后被 T/CT 清理的时间戳 |
| **`CNavArea::m_danger`** | `+0xC8` | `+0x100` | `float[2]`，阵营动态危险惩罚值 |
| **`CNavArea::m_dangerTimestamp`** | `+0xD0` | `+0x108` | `float[2]`，Danger 最后更新时间戳 |
| **`CNavArea::m_earliestOccupyTime`** | `+0xD4` | `+0x120` | `float[2]`，双方最早到达该区时间 |
