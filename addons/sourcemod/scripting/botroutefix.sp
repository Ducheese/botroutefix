//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点与防发疯前冲底层补丁 (CT Bombsite Defense & Anti-Rush Patches)]
//   1. 对 guardBombsiteChance 跳转打入 6 字节 NOP，激活官方原生 CT 守包点体系；
//   2. 对 IsDefenseRushing 跳转打入 6 字节 NOP，彻底消除官方 33.3% 的全员前冲白给局。
//
// [模块 2：T 包匪开局即刻运包底层补丁 (T C4 Carrier Instant Plant Patch)]
//   3. 对 IsTimeToPlantBomb 开局 10~30 秒强制延迟跳转打入 6 字节 NOP，消除包匪开局
//      不运包反而带 C4 冲 CT 家送人头的致命缺陷，开局即刻前往包点。
//
// [模块 3：开局全图 Danger 彻底重置 (Round Danger Reset)]
//   4. 遍历全局 CUtlVector<CNavArea*> TheNavAreas，在 round_start 时将全图各区域的
//      m_danger 彻底清零并重置时间戳，彻底消除上一回合残局死人留下的跨回合“幽灵恐惧”假象。
//
// [模块 4：礼貌排队 1.2 秒超时打碎与门口掉头 (1.2s Queue Breaker & Anti-Jam Turnaround)]
//   5. 监控 Bot 在狭窄门口/路口遇到队友挡路时的 m_isWaitingBehindFriend 礼貌等待状态。
//      官方原版等待高达 3.5~5.0 秒导致开火车堵死，模块 4 在等待超过 1.2 秒（人类犹豫极限）
//      时立即强制清空路径并打碎排队，配合模块 3 的走廊 Danger 阻力，驱使 Bot 立即掉头绕道。
//
// [模块 5：T 阵营 C4 包匪专属护卫与保镖协同 (T C4 Carrier Escort & Bodyguard System)]
//   6. 解决不带包的 T 队员与包匪完全脱节、各自为战的致命缺陷。
//      开局及捡包时，按存活比例挑选距离包匪最近的 1~2 名 T 队友调用原生 CCSBot::Follow
//      贴身护送包匪，负责探路、掩护补枪与秒捡掉落 C4；下包后由引擎底层自动解除跟随并就地守包。
//
// [模块 6：搜敌目标即时抢占与全图打散 (Hunt Target Claim & Flush / Anti-Swarm Search)]
//   7. 解决官方 HuntState 选定全图最久未搜区域（oldest cleared）后不刷新时间戳的缺陷。
//      Detour HuntState::OnUpdate Post 阶段，一旦 Bot 选定目标区域立即刷新其 m_clearedTimestamp，
//      驱使后续队友必须选择全图其他不同要道与战区，实现全队兵力 100% 互不重复的立体多路排查。
//
// [模块 7：C4 掉落 Top 3 真实寻路距离捡包与团队火力掩护 (NoticeLooseBomb / Top 3 Retriever & Tactical Cover)]
//   8. 解决官方 NoticeLooseBomb 只要有掉落 C4 便对全队所有 T 恒为 true 导致的“全员哄抢送死”缺陷。
//      Detour CCSBot::NoticeLooseBomb Post 阶段，调用底层 NavMesh A* 算法计算每名 T 到雷包的真实可行走距离，
//      仅对最优路径排名前 3 的 T 队员返回 true 组成协同抢包突击组，其余队友保持 false 维持当前战术架枪与搜敌，提供火力掩护。
//
// [模块 8：CT 首席拆包员与交叉火力架枪体系 (CT Designated Defuser & Crossfire Guard)]
//   9. 解决官方 CT 在按住 E 读条前全员判定 DEFUSE_BOMB 一起往雷包上撞无掩护的缺陷。
//      实时竞选单人「首席拆包员」直扑 C4 拆包，其余赶到包点 1500 码内的 CT 提前转入 GUARD_BOMB_DEFUSER，
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
	url         = "https://github.com/Ducheese"
};

//========================================================================================
// LIFECYCLE
//========================================================================================

public void OnPluginStart()
{
	LogMessage("[BotRouteFix] ========== Initializing BotRouteFix (v%s) ==========", PLUGIN_VERSION);

	PrepOffsets();
	PrepDefensePatch();
	PrepDefenseRushPatch();
	PrepC4PlantDelayPatch();
	PrepDangerReset();
	ResetAllNavAreaDanger();
	StartQueueBreakerTimer();
	PrepFollowSDKCalls();
	PrepHuntStateHook();
	PrepNoticeLooseBombHook();
	PrepDefuseSDKCalls();
	StartDefuseCoordTimer();
	HookGameEvents();

	// Mid-round hot-reload synchronization: check if bomb is already planted on map
	int bomb = FindEntityByClassname(-1, "planted_c4");
	if (bomb != -1 && IsValidEntity(bomb))
	{
		g_bBombPlanted = true;
		GetEntPropVector(bomb, Prop_Data, "m_vecAbsOrigin", g_fBombPlantedPos);
		UpdateDesignatedDefuser();
		LogMessage("[BotRouteFix] [Hot-Reload] Synchronized active planted C4 at (%.1f, %.1f, %.1f)",
			g_fBombPlantedPos[0], g_fBombPlantedPos[1], g_fBombPlantedPos[2]);
	}

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	StopQueueBreakerTimer();
	StopDefuseCoordTimer();
	RestoreDefensePatch();
	RestoreDefenseRushPatch();
	RestoreC4PlantDelayPatch();
	RestoreHuntStateHook();
	RestoreNoticeLooseBombHook();
}
