//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点与防发疯前冲底层补丁 (CT Bombsite Defense & Anti-Rush Patches)]
//   1. 对 guardBombsiteChance 条件跳转打入 6 字节 NOP，激活官方原生 CT 守包点体系；
//   2. 对 IsDefenseRushing 条件跳转打入 6 字节 NOP，彻底消除官方 33.3% 的 CT 前冲白给局。
//
// [模块 2：T 包匪开局即刻运包与 A/B 随机包点锁定 (T C4 Carrier Instant Plant & Bombsite Target Lock)]
//   3. 对 IsTimeToPlantBomb 开局延迟条件跳转打入 6 字节 NOP，消除包匪开局 10~30 秒原地罚站/逛街送人头的致命缺陷，开局即刻前往包点；
//   4. 解决官方 GetClosestZone 导致 T 包匪 100% 局数只冲近点缺陷，在 IdleState 运包决策点进行汇编级 64位/32位 内存即时数注入 (mov rax, imm64 / mov eax, imm32)，
//      每回合开局 50%/50% 随机预选并锁定目标包点，整回合恒定有效，既实现全图随机战略进攻，又彻底杜绝 180 度横跳折返跑。
//
// [模块 3：开局全图 Danger 软衰减 (Round Danger Soft Decay)]
//   5. 遍历全局 CUtlVector<CNavArea*> TheNavAreas，在 round_start 时将全图各区域的
//      m_danger 衰减 80%（保留 20% 微弱战场记忆），既防止跨回合永久封路死锁，又为 A* 寻路注入动态扰动以产生多变进攻路线。
//
// [模块 4：礼貌排队 1.2 秒超时打碎与门口掉头 (1.2s Queue Breaker & Anti-Jam Turnaround)]
//   6. 监控 Bot 在狭窄门口/路口遇到队友挡路时的 m_isWaitingBehindFriend 礼貌等待状态。
//      官方原版等待高达 3.5~5.0 秒导致开火车堵死，模块 4 在等待超过 1.2 秒（人类犹豫极限）
//      时立即强制清空路径并打碎排队，配合 NavArea 阻力，驱使 Bot 立即掉头绕道。
//
// [模块 5：T 阵营 C4 包匪专属护卫与保镖协同 (T C4 Carrier Escort & Bodyguard System)]
//   7. 解决不带包的 T 队员与包匪完全脱节、各自为战的致命缺陷。
//      开局及捡包时，挑选距离包匪最近的 2 名 T 队友调用官方原生 CCSBot::Follow
//      贴身护送包匪，负责探路、掩护补枪与秒捡掉落 C4；下包后由引擎底层自动解除跟随并就地守包。
//
// [模块 6：搜敌目标即时抢占与全图打散 (Hunt Target Claim & Anti-Swarm Search)]
//   8. 解决官方 HuntState 多人同时选中同一个偏远小角落产生“全图排队跑图”缺陷。
//      Detour CCSBot::HuntState_OnUpdate Post 阶段，Bot 选定搜敌目标瞬间立即刷新其 m_clearedTimestamp 为当前时间，
//      驱使后续队友必须选择全图其他不同要道与战区，实现全队兵力 100% 互不重复的立体多路排查。
//
// [模块 7：C4 掉落 Top 3 真实寻路距离捡包与团队火力掩护 (Loose Bomb Top 3 Retriever & Tactical Cover)]
//   9. 解决官方 NoticeLooseBomb 只要有掉落 C4 便对全队所有 T 恒为 true 导致的“全员哄抢送死”缺陷。
//      Detour CCSBot::NoticeLooseBomb Post 阶段，调用底层原生 CCSBot::ComputePath 驱动 NavMesh A* 算法计算每名存活 T 到雷包的真实弯道行走距离并升序排序，
//      仅对排名前 3 的 T 队员返回 true 组成 3 人协同抢包突击组，其余队友保持 false 维持当前战术架枪与搜敌，提供火力掩护。
//
// [模块 8：CT 首席拆包员与交叉火力架枪体系 (CT Designated Defuser & Crossfire Guard)]
//  10. 解决官方 CT 在按住 E 读条前全员判定 DEFUSE_BOMB 一起往雷包上撞无掩护的缺陷。
//      实时竞选单人「首席拆包员」直扑 C4 拆包，其余到达包点触发区（m_bInBombZone）的支援 CT 提前转入 GUARD_BOMB_DEFUSER，
//      调用原生 CCSBot::Hide 在包点掩体后散开架枪蹲点，形成立体交叉火力掩护；首席倒地秒级接力。
//
// 支持架构：
//   - 32-bit: Windows non-Steam (v91/v92) server.dll
//   - 64-bit: Windows Steam x64 server.dll
//========================================================================================

#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>
#include <dhooks>

#define GAMEDATA "botroutefix.gamedata"
#define PLUGIN_VERSION "1.0.0"

#include "BotRouteFix/globals.inc"
#include "BotRouteFix/patches.inc"
#include "BotRouteFix/danger_reset.inc"
#include "BotRouteFix/queue_breaker.inc"
#include "BotRouteFix/carrier_escort.inc"
#include "BotRouteFix/hunt_claim.inc"
#include "BotRouteFix/loose_bomb.inc"
#include "BotRouteFix/designated_defuser.inc"
#include "BotRouteFix/events.inc"

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复",
	version     = PLUGIN_VERSION,
	url         = "https://space.bilibili.com/1889622121"
};

//========================================================================================
// LIFECYCLE
//========================================================================================

public void OnPluginStart()
{
	LogMessage("[BotRouteFix] ========== Initializing BotRouteFix (v%s) ==========", PLUGIN_VERSION);

	// =========================================================================
	// [阶段 1] 基础设施与核心偏移加载 (Base Infrastructure & Offsets)
	// =========================================================================
	PrepOffsets();

	// =========================================================================
	// [阶段 2] SDKCalls 与 Detour 钩子准备 (SDKCalls & DHook Detours)
	// =========================================================================
	PrepFollowSDKCalls();
	PrepHideSDKCall();
	PrepHuntStateHook();
	PrepNoticeLooseBombHook();

	// =========================================================================
	// [阶段 3] 二进制内存补丁准备与注入 (Binary Memory Patches)
	// =========================================================================
	PrepGuardBombsitePatch();
	PrepAntiRushPatch();
	PrepInstantPlantPatch();
	PrepBombsiteLockPatch();
	PrepDangerReset();

	// =========================================================================
	// [阶段 4] 运行时状态初始化与定时器 (Runtime State & Active Timers)
	// =========================================================================
	SelectRandomBombsite();
	ResetAllNavAreaDanger();
	StartQueueBreakerTimer();

	// =========================================================================
	// [阶段 5] 游戏事件监听与热重载同步 (Game Events & Hot-Reload Sync)
	// =========================================================================
	HookGameEvents();
	SyncMidRoundState();

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	StopQueueBreakerTimer();
	StopDefuseCoordTimer();

	RestoreGuardBombsitePatch();
	RestoreAntiRushPatch();
	RestoreInstantPlantPatch();
	RestoreBombsiteLockPatch();

	RestoreHuntStateHook();
	RestoreNoticeLooseBombHook();
}

//========================================================================================
// MID-ROUND HOT-RELOAD SYNCHRONIZATION
//========================================================================================

void SyncMidRoundState()
{
	// -----------------------------------------------------------------------------------
	// [状态 1：C4 已安放 (Planted C4)]
	// -----------------------------------------------------------------------------------
	int plantedBomb = FindEntityByClassname(-1, "planted_c4");
	if (plantedBomb != -1 && IsValidEntity(plantedBomb))
	{
		g_bBombPlanted = true;
		GetEntPropVector(plantedBomb, Prop_Data, "m_vecAbsOrigin", g_fBombPlantedPos);
		UpdateDesignatedDefuser();
		StartDefuseCoordTimer();

		LogMessage("[BotRouteFix] [Hot-Reload] (State 1/3) Active Planted C4 synchronized at (%.1f, %.1f, %.1f)",
			g_fBombPlantedPos[0], g_fBombPlantedPos[1], g_fBombPlantedPos[2]);
		return;
	}

	// -----------------------------------------------------------------------------------
	// [状态 2：C4 散落地面 (Loose Bomb)]
	// -----------------------------------------------------------------------------------
	int looseBomb = GetLooseBombEntity();
	if (looseBomb != -1)
	{
		// 处于掉包残局：立即解除开局包点锁定，恢复原生动态就近下包
		RestoreBombsiteLockPatch();

		LogMessage("[BotRouteFix] [Hot-Reload] (State 2/3) Loose C4 on floor synchronized (Adaptive zone restored)");
		return;
	}

	// -----------------------------------------------------------------------------------
	// [状态 3：C4 处于运包途中 (Carried C4)]
	// -----------------------------------------------------------------------------------
	int carrier = GetC4Carrier();
	if (carrier > 0 && IsPlayerAlive(carrier))
	{
		// 处于运包推进阶段：立即补发最近 2 名保镖随行护送
		AssignCarrierBodyguards();
		LogMessage("[BotRouteFix] [Hot-Reload] (State 3/3) Active Carrier %N synchronized (Bodyguards assigned)", carrier);
	}
}
