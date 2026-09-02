# Bot Route Fix (CS:S Bot 寻路与战术决策底层修复插件)

针对 Counter-Strike: Source 官方 Bot AI 底层寻路缺陷与战术决策逻辑的 SourceMod 修复插件。

---

## 模块列表 (Modules)

### 模块 1：CT 守包点与防发疯前冲底层补丁 (CT Bombsite Defense & Anti-Rush Patches)
* **状态**：**已实装**
* **逆向根因**：
  1. 在官方源码 `cs_bot_idle.cpp` 的 `IdleState::OnUpdate()` 中，CT 判定是否守包点的公式为：
     $$\text{guardBombsiteChance} = -34.0f \times \text{Morale}$$
     开局默认 $\text{Morale} = +1$，计算得 **`-34.0%`**（负数）。底层的条件跳转指令（64位 `jnb` / 32位 `jbe`）开局 100% 必定触发跳过，导致非劣势局 CT 完全不守包点，直接掉入保底的 `Hunt()` 冲向匪家白给送死；
  2. 在进入守点前，引擎先检查 `TheCSBots()->IsDefenseRushing()`（每回合有 **33.3%** 的纯随机几率判定全队前冲）。一旦触发，直接在顶层执行 `me->Hunt()` 绕过守点逻辑。
* **修复机制**：
  1. 对 `guardBombsiteChance` 计算逻辑原地覆写 **26 字节机器码**，将公式重构为：
     $$\text{guardBombsiteChance} = 70.0f - 10.0f \times \text{Morale}$$
     实现平局(0) 70%、优势(+3) 40%、劣势(-3) 100% 留守包点的健康战术梯度，并完整保留后续 RandomFloat 比较与跳转流转；
  2. 对 `IsDefenseRushing` 跳转打入 **6 字节 NOP (`90 90 90 90 90 90`)** 就地补丁，彻底消除官方 33.3% 的全员无脑冲锋暴毙局；
  3. 保留独狼 Bot（`IsRogue`）的单兵自主性，兼顾战术纪律与战场多样性。

---

### 模块 2：T 包匪动态运包时机与 A/B 随机包点锁定 (T C4 Carrier Dynamic Plant & Bombsite Target Lock)
* **状态**：**已实装**
* **逆向根因**：
  1. 在官方源码 `cs_bot_manager.cpp` 中，每回合开局设定了下包延迟时间戳：
     $$m\_earliestBombPlantTimestamp = \text{curtime} + \text{RandomFloat}(10.0f, 30.0f)$$
     导致开局前 10~30 秒内，`IsTimeToPlantBomb()` 恒为 `false`。带包的 T 队员被禁止前往包点运包，顺流跌落到了最底部的 `me->Hunt()`，带着 C4 逛街白给；
  2. 运包时官方源码直接调用 `TheCSBots()->GetClosestZone(...)`，导致包匪 100% 局数都死板地前往物理距离最近的包点（例如 `de_dust2` 永远只去 B 包点）。
* **修复机制**：
  1. 将运包延迟接入士气动态线性映射：
     $$\text{PlantDelay} = 30.0f + 10.0f \times \text{Morale} \quad (0\text{s} \sim 60\text{s})$$
     开局动态写入引擎 `TheCSBots()->m_earliestBombPlantTimestamp` 内存（32位偏移 6712 / 64位偏移 7020）。绝境(-3) 0 秒极速速推下包求生，连胜优势(+3) 放开 60 秒自由控图抓单；
  2. 解决 `GetClosestZone` 缺陷：在 `IdleState` 运包决策点进行汇编级 64位/32位 内存即时数注入（`mov rax, imm64` / `mov eax, imm32` + NOPs），每回合开局 **50% / 50%** 随机预选并锁定目标包点，整回合恒定有效，既实现全图随机战略进攻，又彻底杜绝 180 度横跳折返跑。

---

### 模块 3：开局全图 Danger 软衰减 (Round Danger Soft Decay)
* **状态**：**已实装**
* **逆向根因**：
  官方 A* 寻路算法（`CCSBot::ComputePath`）在阵亡时向 NavArea 叠加的 `m_danger` 跨回合残留衰减缓慢，导致阵亡密集的要道产生永久高阻力封路；
* **修复机制**：
  遍历全局 `CUtlVector<CNavArea*> TheNavAreas`，在 `round_start` 时将全图所有区域的 `m_danger` 软衰减 80%（保留 20% 微弱战场记忆），既防止跨回合永久封路死锁，又为 A* 寻路注入动态扰动以产生多变进攻路线。

---

### 模块 4：礼貌排队 1.2 秒超时打碎与门口掉头 (1.2s Queue Breaker & Anti-Jam Turnaround)
* **状态**：**已实装**
* **逆向根因**：
  在 `cs_bot_pathfind.cpp` 的 `CCSBot::UpdatePathMovement()` 中，当 Bot 在狭窄门口（如 A 门、B 洞狭缝）被队友挡住时，进入 `m_isWaitingBehindFriend = true`，其礼貌等待时长计算公式为：
  $$\text{politeDuration} = 5.0f - 3.0f \times \text{Aggression}$$
  标准 Bot 原地发呆等待高达 **3.5 ~ 5.0 秒**，导致后排队友依次卡死在门后排成一串开火车。
* **修复机制**：
  10Hz 高频定时器实时监控 `m_isWaitingBehindFriend` 与 `m_politeTimer`。一旦 Bot 在原地等待超过 **1.2 秒**（人类犹豫极限），插件立即清空路径（`m_isStopping = 0`, `m_pathLength = 0`, `m_pathLadder = NULL`）并打碎排队。在下一帧 Bot 重新寻路时，结合堵门节点注入的阻力，**Bot 会果断掉头绕道走另一侧**，彻底解决门框堵死与排队开火车。

---

### 模块 5：T 阵营 C4 包匪专属护卫与保镖协同 (T C4 Carrier Escort & Bodyguard System)
* **状态**：**已实装**
* **逆向根因**：
  在官方源码 `cs_bot_update.cpp` 中，自动跟随（AutoFollow）仅对人类玩家生效，对带包的 Bot 队友完全无视；且不带包的 T 队员在下包前只有冲 CT 家（`Hunt`）或冲交火线的逻辑，导致包匪永远单刀赴会、队友全部脱节送命。
* **修复机制**：
  在回合开局冻结结束及 C4 被捡起时，根据士气线性分配：
  $$\text{Bodyguards} = 3 - \text{Morale} \quad (0 \sim 6 \text{人})$$
  挑选 NavMesh 地面行走距离最近的 T 队友调用原生 `CCSBot::Follow` 成为专属贴身保镖，形成绝境(-3) 6 人重装抱团推点、连胜(+3) 全员 0 保镖放飞拉枪线控图的立体战术形态；下包后由引擎底层自动解除跟随并就地守包。

---

### 模块 6：搜敌目标即时抢占与 45 秒冲锋破除 (Hunt Target Claim & 45s Rush Bypass)
* **状态**：**已实装**
* **逆向根因**：
  1. 在 `cs_bot_hunt.cpp` 的 `HuntState::OnUpdate()` 中，官方存在开局 45 秒短路分支：
     $$\text{if (GetElapsedRoundTime() < 45.0f \&\& !HasVisitedEnemySpawn())}$$
     导致开局前 45 秒内，无论 T 还是 CT，引擎无条件直接调用 `GetRandomSpawn(OtherTeam)` 将目标锁死为敌方老家，根本不读取 `clearedTimestamp`，导致双方开局在中路主干道无脑对撞混战；
  2. 45 秒后全图搜点时，Bot 挑中目标后官方源码未更新时间戳，导致全队 3~4 个存活 Bot 算出来的目标完全一模一样（蜂群抱团）。
* **修复机制**：
  1. Detour `HuntState::OnUpdate` **Pre 阶段**：将当前 Bot 的 `m_hasVisitedEnemySpawn`（紧随 `m_isStopping` 的 1 字节布尔位）置为 `true`，彻底破除 45 秒老家锁定，强制从第 0 秒直通 `TheNavAreas` 全图分散搜索；
  2. Detour `HuntState::OnUpdate` **Post 阶段**：Bot 选定搜敌目标 `m_huntArea` 的瞬间，立即将其 `m_clearedTimestamp` 刷新为当前时间戳，驱使后续队友必须选择全图其他不同要道与战区，实现全队兵力 100% 互不重复的立体网状排查。

---

### 模块 7：C4 掉落 Top 3 真实寻路距离捡包与团队火力掩护 (Loose Bomb Top 3 Retriever & Tactical Cover)
* **状态**：**已实装**
* **逆向根因**：
  在 `cs_bot.cpp` 的 `CCSBot::NoticeLooseBomb()` 中，官方写死只要地图上有掉落 C4，对全队所有 T 队员恒返回 `true`，导致包匪倒地后全队所有存活队友全部切换为 `FetchBombState` 像踢足球一样蜂拥哄抢 C4，在狭窄路口被 CT 一颗手雷全部团灭。
* **修复机制**：
  Detour `CCSBot::NoticeLooseBomb` Post 阶段。调用底层原生 `CCSBot::ComputePath` 驱动 NavMesh A* 算法计算每名存活 T 到雷包的真实弯道行走距离并升序排序，**仅对排名前 3 的 T 队员返回 `true` 组成 3 人协同抢包突击组**；对其余队友强制覆写为 `false`，使其继续维持在 `HuntState` 中就地反击、原地架枪掩护，形成完美的“3人突击捡包 + 全队火力掩护”专业战术协同。

---

### 模块 8：CT 首席拆包员与交叉火力架枪体系 (CT Designated Defuser & Crossfire Guard)
* **状态**：**已实装 (v1.0.0)**
* **逆向根因**：
  在官方源码 `cs_bot_idle.cpp` 中，`TheCSBots()->GetBombDefuser()` 只有在某个 CT 真正按住 E 键开始读条拆包的那一瞬间才会变为非空。在此之前（进包点的路上），全队所有 CT 全部被赋予 `DEFUSE_BOMB` 任务，2~4 个人同时撞向 C4 抢拆，导致包点周围完全没有任何人架枪与掩护。
* **修复机制**：
  1. 确立严谨的物理运动学等效优先权模型：
     $$\text{PriorityDistance} = (10.0\text{s} - 5.0\text{s}) \times 250.0\text{ units/s} = \mathbf{1250.0\text{ 码}}$$
     持钳 CT 在 1250 码内赶到拆完的耗时等于/优于贴脸无钳者；
  2. 动态竞选单人「首席拆包员」直扑 C4 拆包；其余到达包点触发区（`m_bInBombZone`）的支援 CT 提前转入 `GUARD_BOMB_DEFUSER`，调用原生 `CCSBot::Hide` 在包点掩体后散开架枪蹲点，形成立体交叉火力掩护；首席倒地秒级接力。

---

## 支持架构 (Supported Architectures)

- **32-bit**：Windows non-Steam (v91/v92) `server.dll`
- **64-bit**：Windows Steam x64 (2026 最新构建) `server.dll`

---

## 逆向签名与结构体档案库 (Reverse Engineering Archive)

以下所有签名与成员偏移量均已在 32 位与 64 位官方 `server.dll` 中通过 IDA Pro 实机逆向，并通过 `find_bytes` 验证为 **100% 唯一匹配**。

### 1. 核心活跃签名库（Active Signatures - 当前插件直接使用）

| 标识符 | 32-bit 地址 | 64-bit 地址 | 对应 C++ 原型与使用模块 |
| :--- | :--- | :--- | :--- |
| **`IdleState_GuardBombsiteChance`** | `0x102BBE15` | `0x18036448C` | `IdleState::OnUpdate` 守点概率覆写点 (模块 1 / Patch 1) |
| **`IdleState_DefenseRush`** | `0x102BBDED` | `0x18036444C` | `IdleState::OnUpdate` 全队前冲 NOP 拦截点 (模块 1 / Patch 2) |
| **`IdleState_C4PlantDelay`** | `0x102BB882` | `0x180363E0D` | `IdleState::OnUpdate` 包匪选点与 TheCSBotsPtr 解析点 (模块 2 / Patch 4) |
| **`IdleState_PlantBombClosestZone`** | `0x102BB849` | `0x180363E51` | `IdleState::OnUpdate` 汇编级即时数注入锁包点 (模块 2 / Patch 4) |
| **`CCSBotManager_GetRandomArea`** | `0x10290D70` | `0x180362A80` | `CCSBotManager::GetRandomArea` (用于解析 TheNavAreas 向量基址) (模块 3) |
| **`CCSBot_Follow`** | `0x102A6B60` | `0x1803496E0` | `void CCSBot::Follow(CCSPlayer *leader)` (保镖跟随) (模块 5) |
| **`CCSBot_StopFollowing`** | `0x102A73C0` | `0x18034A1A0` | `void CCSBot::StopFollowing()` (解除跟随) (模块 5) |
| **`HuntState_OnUpdate`** | `0x102BA430` | `0x180362350` | `void HuntState::OnUpdate(CCSBot *me)` (45s老家破除与目标抢占) (模块 6) |
| **`CCSBot_NoticeLooseBomb`** | `0x102914F0` | `0x18032B600` | `bool CCSBot::NoticeLooseBomb()` (掉落 C4 真实距离 Top 3 筛选) (模块 7) |
| **`CCSBot_ComputePath`** | `0x102A2000` | `0x180343550` | `bool CCSBot::ComputePath(const Vector &goal, int route)` (A* 真实行走距离测量) (模块 7/8) |
| **`CCSBot_Hide`** | `0x102A6D50` | `0x180349940` | `bool CCSBot::Hide(CNavArea *area, ...)` (掩体潜伏与交叉架枪) (模块 8) |

### 2. 预研与归档签名库（Archived / Reserved Signatures - 底层逆向备用）

| 标识符 | 32-bit 地址 | 64-bit 地址 | 对应 C++ 原型与归档用途 |
| :--- | :--- | :--- | :--- |
| **`CCSBotManager_GetRandomZone`** | `0x10290E50` | `0x180362B90` | `const Zone* CCSBotManager::GetRandomZone()` (50% 随机包点原生函数，已被 Patch 4 汇编即时数注入方案完全替代) |
| **`CCSBot_MoveToInitialEncounter`** | `0x102A6FF0` | `0x180349CF0` | `bool CCSBot::MoveToInitialEncounter()` (开局前沿交火线推进机制分析) |
| **`PathCost_FriendDensity`** | `0x10292CB0` | `0x18032D2B0` | `PathCost::operator()` (50000.0f 队友密度代价读取点分析) |
| **`CCSBot_UpdatePathMovement`** | `0x102A5000` | `0x180347330` | `int CCSBot::UpdatePathMovement()` (路径跟踪与礼貌排队机制分析) |

### 3. 核心活跃成员偏移量（Active Offsets - 当前插件直接使用）

| 结构体与成员 | 32-bit (v91/v92) | 64-bit (Steam x64) | 数据类型与使用模块 |
| :--- | :--- | :--- | :--- |
| **`TheCSBots()->m_zoneBase`** | `+0x184C` (`6220`) | `+0x1860` (`6240`) | 包点数组 `m_zone[4]` 起始地址 (模块 2 / Patch 4) |
| **`TheCSBots()->m_zoneStride`** | `120` 字节 (`0x78`) | `192` 字节 (`0xC0`) | 单个 `Zone` 结构体物理尺寸 (模块 2 / Patch 4) |
| **`TheCSBots()->m_zoneCount`** | `+0x1A2C` (`6700`) | `+0x1B60` (`7008`) | 包点总数 `int` (`m_zone[4]` 之后) (模块 2 / Patch 4) |
| **`TheCSBots()->m_earliestBombPlantTimestamp`** | `+0x1A38` (`6712`) | `+0x1B6C` (`7020`) | 最早运包延迟时间戳 `float` (模块 2) |
| **`CCSBot::m_morale`** | `+0x1A08` (`6664`) | `+0x1E50` (`7760`) | 个人士气值枚举 `MoraleType` (-3 ~ +3) (模块 1/2/5) |
| **`CCSBot::m_isStopping`** | `+0x1C20` (`7200`) | `+0x2100` (`8448`) | 减速停车标志位 (bool/int8) (模块 4) |
| **`CCSBot::m_hasVisitedEnemySpawn`** | `+0x1C21` (`7201`) | `+0x2101` (`8449`) | 访问过敌方老家标志位 (bool/int8，破除 45s 短路) (模块 6) |
| **`CCSBot::m_path`** | `+0x1C28` (**步长 24B**) | `+0x2108` (**步长 32B**) | 寻路节点数组 `ConnectInfo[256]` (模块 4) |
| **`CCSBot::m_pathLength`** | `+0x3428` (`13352`) | `+0x4108` (`16648`) | 当前路径总节点数 (int32) (模块 4) |
| **`CCSBot::m_pathIndex`** | `+0x342C` (`13356`) | `+0x410C` (`16652`) | 当前正前进的节点索引 (int32) (模块 4) |
| **`CCSBot::m_isWaitingBehindFriend`** | `+0x345C` (`13404`) | `+0x4150` (`16720`) | 遇到队友挡路的礼貌等待标志位 (bool/int8) (模块 4) |
| **`CCSBot::m_politeTimer`** | `+0x3450` (`13392`) | `+0x4140` (`16704`) | 礼貌等待计时器 `CountdownTimer` (模块 4) |
| **`CountdownTimer::m_duration`** | `+0x04` | `+0x08` | 计时器设定时长 `float` (模块 4) |
| **`CountdownTimer::m_timestamp`** | `+0x08` | `+0x0C` | 计时器到期时间戳 `float` (模块 4) |
| **`ConnectInfo::pos`** | `+0x08` | `+0x0C` | 节点世界坐标 `Vector` (模块 4) |
| **`CCSBot::m_pathLadder`** | `+0x3468` (`13416`) | `+0x4160` (`16736`) | 当前梯子指针 (pointer) (模块 4) |
| **`CCSBot::m_task`** | `+0x1BF8` (`7160`) | `+0x20D0` (`8400`) | 当前任务枚举 `BotTaskType` (模块 8) |
| **`HuntState::m_huntArea`** | `+0x04` | `+0x08` | 选定的搜敌目标 `CNavArea*` (模块 6) |
| **`CNavArea::m_clearedTimestamp`** | `+0xB4` (`180`) | `+0xF8` (`248`) | `float[2]`，最后被 T/CT 清理的时间戳 (模块 6) |
| **`CNavArea::m_danger`** | `+0xBC` (`188`) | `+0x100` (`256`) | `float[2]`，阵营动态危险惩罚值 (模块 3/4) |
| **`CNavArea::m_dangerTimestamp`** | `+0xC4` (`196`) | `+0x108` (`264`) | `float[2]`，Danger 最后更新时间戳 (模块 3/4) |
| **`Pointer_Size`** | `4` 字节 | `8` 字节 | 平台指针宽度 (模块 3) |

### 4. 预研与归档成员偏移量（Archived / Reserved Offsets - 底层逆向备用）

| 结构体与成员 | 32-bit (v91/v92) | 64-bit (Steam x64) | 数据类型与归档说明 |
| :--- | :--- | :--- | :--- |
| **`CCSBot::m_noiseTimestamp`** | `+0x388C` (`14476`) | `+0x4460` (`17504`) | `float`，最后听到非友军声音时间戳（声音自锁备用） |
| **`CCSBot::m_noisePosition`** | `+0x387C` (`14460`) | `+0x4450` (`17488`) | `Vector`，最后听到声音的世界坐标 |
| **`CCSBot::m_noisePriority`** | `+0x3894` (`14484`) | `+0x4470` (`17520`) | `PriorityType`，声音优先级 |
| **`CCSBot::m_disposition`** | `+0x1BF0` (`7152`) | `+0x20C8` (`8392`) | `DispositionType`，接战策略（OPPORTUNITY_FIRE 备用） |
| **`CCSBot::m_lastKnownArea`** | `+0x1C14` (`7188`) | `+0x20F0` (`8432`) | `CNavArea*`，Bot 当前所在的 NavArea |
| **`CNavArea::m_center`** | `+0x2C` (`44`) | `+0x30` (`48`) | `Vector`，区域三维几何中心坐标 |
| **`CNavArea::m_earliestOccupyTime`** | `+0xD4` (`212`) | `+0x120` (`288`) | `float[2]`，双方阵营全速跑图最早到达时间 |
