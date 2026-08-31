//========================================================================================
// botroutefix.sp
//
// CS:S Bot 寻路与战术决策底层修复插件 (Bot AI Route & Decision Fix)
//
// ---------------------------------------------------------------------------------------
// [模块 1：CT 守包点与防发疯前冲修复 (CT Bombsite Defense & Anti-Rush Fix)]
//   1. 对 guardBombsiteChance 跳转打入 6 字节 NOP，激活官方原生 CT 守包点体系；
//   2. 对 IsDefenseRushing 跳转打入 6 字节 NOP，彻底消除官方 33.3% 的全员前冲白给局。
//
// [模块 2：T 包匪开局即刻运包与 50% 随机包点修复 (T C4 Carrier Instant Plant & 50/50 Bombsite Fix)]
//   3. 对 IsTimeToPlantBomb 开局 10~30 秒强制延迟跳转打入 6 字节 NOP，消除包匪开局
//      不运包反而带 C4 冲 CT 家送人头的致命缺陷，开局即刻前往包点；
//   4. 对 GetClosestZone 调用打入补丁重定向为 GetRandomZone，打破死板的“永远去最近包点”
//      机制，实现开局在 A 包点与 B 包点之间以 50%/50% 真实随机轮换进攻。
//
// [模块 3：全动态走廊 Danger 预约与多路分流 (Dynamic Corridor Reservation & Route Splitting)]
//   5. Detour CCSBot::ComputePath Post 阶段，在刚规划好路线的前沿 5 个 NavArea 节点
//      动态注入 +0.8f 的 m_danger 占用阻力，驱使后续队友的 A* 算法自动分支转向副道/侧翼，
//      全图自然实现两翼包抄、兵分多路，彻底消除单一路线开火车拥堵。
//
// [模块 4：礼貌排队 1.2 秒超时打碎与门口掉头 (1.2s Queue Breaker & Anti-Jam Turnaround)]
//   6. 监控 Bot 在狭窄门口/路口遇到队友挡路时的 m_isWaitingBehindFriend 礼貌等待状态。
//      官方原版等待高达 3.5~5.0 秒导致开火车堵死，模块 4 在等待超过 1.2 秒（人类犹豫极限）
//      时立即强制清空路径并打碎排队，配合模块 3 的走廊 Danger 阻力，驱使 Bot 立即掉头绕道。
//
// [模块 5：T 阵营 C4 包匪专属护卫与保镖协同 (T C4 Carrier Escort & Bodyguard System)]
//   7. 解决不带包的 T 队员与包匪完全脱节、各自为战的致命缺陷。
//      开局及捡包时，按存活比例挑选距离包匪最近的 1~2 名 T 队友调用原生 CCSBot::Follow
//      贴身护送包匪，负责探路、掩护补枪与秒捡掉落 C4；下包后由引擎底层自动解除跟随并就地守包。
//
// [模块 6：搜敌目标即时抢占与全图打散 (Hunt Target Claim & Flush / Anti-Swarm Search)]
//   8. 解决官方 HuntState 选定全图最久未搜区域（oldest cleared）后不刷新时间戳的缺陷。
//      Detour HuntState::OnUpdate Post 阶段，一旦 Bot 选定目标区域立即刷新其 m_clearedTimestamp，
//      驱使后续队友必须选择全图其他不同要道与战区，实现全队兵力 100% 互不重复的立体多路排查。
//
// [模块 7：C4 掉落单人拾取与团队火力掩护 (NoticeLooseBomb / Single Retriever & Tactical Cover)]
//   9. 解决官方 NoticeLooseBomb 只要有掉落 C4 便对全队所有 T 恒为 true 导致的“全员哄抢送死”缺陷。
//      Detour CCSBot::NoticeLooseBomb Post 阶段，仅对物理距离最近的 1 名 T 队员返回 true 前去捡包，
//      其余队友保持 false 维持 HuntState 状态，就地架枪、反打敌人并提供火力掩护。
//
// [模块 8：CT 首席拆包员与交叉火力架枪体系 (CT Designated Defuser & Crossfire Guard)]
//  10. 解决官方 CT 在按住 E 读条前全员判定 DEFUSE_BOMB 一起往雷包上撞无掩护的缺陷。
//      实时竞选单人「首席拆包员」直扑 C4 拆包，其余赶到包点的 CT 提前转入 GUARD_BOMB_DEFUSER，
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

//========================================================================================
// HANDLES & VARIABLES
//========================================================================================

bool g_bIsWin64 = false;

// Patch 1: CT Bombsite Defense Unlock (guardBombsiteChance)
Address g_pDefensePatchAddress;
int g_iOriginalDefenseBytes[6];
bool g_bDefensePatched = false;

// Patch 2: CT Anti-Defense-Rush (IsDefenseRushing)
Address g_pDefenseRushPatchAddress;
int g_iOriginalDefenseRushBytes[6];
bool g_bDefenseRushPatched = false;

// Patch 3: T C4 Carrier Instant Plant (IsTimeToPlantBomb delay removal)
Address g_pC4PlantDelayPatchAddress;
int g_iOriginalC4PlantDelayBytes[6];
bool g_bC4PlantDelayPatched = false;

// Patch 4: T C4 Carrier Bombsite Randomization (GetClosestZone -> GetRandomZone)
Address g_pC4RandomZonePatchAddress;
int g_iC4RandomZonePatchLength = 0;
int g_iOriginalC4RandomZoneBytes[24];
bool g_bC4RandomZonePatched = false;

// Module 3: Dynamic Corridor Danger Reservation (CCSBot::ComputePath Post Detour)
Handle g_hComputePathDetour = INVALID_HANDLE;
int g_iOffset_Path = 0;
int g_iPathStride = 0;
int g_iOffset_PathLength = 0;
int g_iOffset_Danger = 0;
int g_iOffset_DangerTimestamp = 0;

// Module 4: 1.2s Queue Breaker & Doorway Turnaround
int g_iOffset_IsWaitingBehindFriend = 0;
int g_iOffset_PoliteTimer = 0;
int g_iOffset_IsStopping = 0;
int g_iOffset_PathLadder = 0;
Handle g_hQueueBreakerTimer = INVALID_HANDLE;

// Module 5: T Carrier Escort (Follow & StopFollowing SDKCalls)
Handle g_hFollowSDKCall = INVALID_HANDLE;
Handle g_hStopFollowingSDKCall = INVALID_HANDLE;

// Module 6: Hunt Target Claim & Flush (HuntState::OnUpdate Post Detour)
Handle g_hHuntStateDetour = INVALID_HANDLE;
int g_iOffset_HuntArea = 0;
int g_iOffset_ClearedTimestamp = 0;

// Module 7: Single Retriever & Tactical Cover (NoticeLooseBomb Post Detour)
Handle g_hNoticeLooseBombDetour = INVALID_HANDLE;

// Module 8: CT Designated Defuser & Crossfire Guard
Handle g_hHideSDKCall = INVALID_HANDLE;
int g_iOffset_Task = 0;
bool g_bBombPlanted = false;
float g_fBombPlantedPos[3];
int g_iDesignatedDefuser = 0;
Handle g_hDefuseCoordTimer = INVALID_HANDLE;

//========================================================================================
// PLUGIN INFO
//========================================================================================

public Plugin myinfo = 
{
	name        = "Bot Route Fix",
	author      = "Ducheese",
	description = "CS:S Bot 寻路与战术决策底层修复 (CT 守包/防冲 + T 运包/50%选点 + 走廊分流 + 1.2s排队打碎 + C4保镖护送 + 搜点抢占 + 单人捡包掩护 + CT首席拆包架枪)",
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
	PrepC4RandomZonePatch();
	PrepComputePathHook();
	StartQueueBreakerTimer();
	PrepFollowSDKCalls();
	PrepHuntStateHook();
	PrepNoticeLooseBombHook();
	PrepDefuseSDKCalls();
	StartDefuseCoordTimer();
	HookGameEvents();

	LogMessage("[BotRouteFix] ========== Initialization Complete on %s ==========", g_bIsWin64 ? "64-bit (Steam x64)" : "32-bit (non-Steam)");
}

public void OnPluginEnd()
{
	StopQueueBreakerTimer();
	StopDefuseCoordTimer();
	RestoreDefensePatch();
	RestoreDefenseRushPatch();
	RestoreC4PlantDelayPatch();
	RestoreC4RandomZonePatch();
	RestoreComputePathHook();
	RestoreHuntStateHook();
	RestoreNoticeLooseBombHook();
}

//========================================================================================
// CONFIGURATION & OFFSETS
//========================================================================================

void PrepOffsets()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
	{
		SetFailState("[BotRouteFix] Failed to load gamedata file: %s.txt", GAMEDATA);
		return;
	}

	g_bIsWin64 = (GameConfGetOffset(gc, "IsWin64") == 1);

	g_iOffset_Path = GameConfGetOffset(gc, "CCSBot_Path_Offset");
	g_iPathStride = GameConfGetOffset(gc, "CCSBot_PathStride");
	g_iOffset_PathLength = GameConfGetOffset(gc, "CCSBot_PathLength_Offset");
	g_iOffset_Danger = GameConfGetOffset(gc, "NavArea_Danger_Offset");
	g_iOffset_DangerTimestamp = GameConfGetOffset(gc, "NavArea_DangerTimestamp_Offset");

	g_iOffset_IsWaitingBehindFriend = GameConfGetOffset(gc, "CCSBot_IsWaitingBehindFriend_Offset");
	g_iOffset_PoliteTimer = GameConfGetOffset(gc, "CCSBot_PoliteTimer_Offset");
	g_iOffset_IsStopping = GameConfGetOffset(gc, "CCSBot_IsStopping_Offset");
	g_iOffset_PathLadder = GameConfGetOffset(gc, "CCSBot_PathLadder_Offset");
	g_iOffset_Task = GameConfGetOffset(gc, "CCSBot_Task_Offset");

	g_iOffset_HuntArea = GameConfGetOffset(gc, "HuntState_HuntArea_Offset");
	g_iOffset_ClearedTimestamp = GameConfGetOffset(gc, "NavArea_ClearedTimestamp_Offset");

	if (g_iOffset_Path == -1 || g_iPathStride == -1 || g_iOffset_PathLength == -1 ||
		g_iOffset_Danger == -1 || g_iOffset_DangerTimestamp == -1 ||
		g_iOffset_IsWaitingBehindFriend == -1 || g_iOffset_PoliteTimer == -1 ||
		g_iOffset_IsStopping == -1 || g_iOffset_PathLadder == -1 ||
		g_iOffset_Task == -1 ||
		g_iOffset_HuntArea == -1 || g_iOffset_ClearedTimestamp == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read one or more offsets from gamedata!");
		return;
	}

	delete gc;
}

//========================================================================================
// PATCH 1: CT BOMBSITE DEFENSE (guardBombsiteChance)
//========================================================================================

void PrepDefensePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_GuardBombsiteChance");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_GuardBombsiteChance!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "GuardBombsite_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read GuardBombsite_PatchOffset!");
		return;
	}

	g_pDefensePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pDefensePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 1] Already patched (NOPs present @ %X), skipping", g_pDefensePatchAddress);
		g_bDefensePatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (32-bit: 0x0F 0x86, 64-bit: 0x0F 0x83)
	if (g_bIsWin64)
	{
		if (firstByte != 0x0F || secondByte != 0x83)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 0F 83, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x86)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 1] Byte mismatch at %X (expected 0F 86, got %02X %02X), patch aborted!",
				g_pDefensePatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseBytes[i] = LoadFromAddress(g_pDefensePatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pDefensePatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bDefensePatched = true;
	LogMessage("[BotRouteFix] [Patch 1] Replaced guardBombsite jump with 6x NOPs @ %X (CT Defense Unlocked)", g_pDefensePatchAddress);

	delete gc;
}

void RestoreDefensePatch()
{
	if (g_bDefensePatched && g_pDefensePatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pDefensePatchAddress + view_as<Address>(i), g_iOriginalDefenseBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 1] Restored original guardBombsite jump @ %X", g_pDefensePatchAddress);
		g_bDefensePatched = false;
	}
}

//========================================================================================
// PATCH 2: CT ANTI-DEFENSE-RUSH (IsDefenseRushing)
//========================================================================================

void PrepDefenseRushPatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_DefenseRush");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_DefenseRush!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "DefenseRush_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read DefenseRush_PatchOffset!");
		return;
	}

	g_pDefenseRushPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pDefenseRushPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pDefenseRushPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 2] Already patched (NOPs present @ %X), skipping", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (both 32-bit and 64-bit are 0x0F 0x85)
	if (firstByte != 0x0F || secondByte != 0x85)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Patch 2] Byte mismatch at %X (expected 0F 85, got %02X %02X), patch aborted!",
			g_pDefenseRushPatchAddress, firstByte, secondByte);
		return;
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalDefenseRushBytes[i] = LoadFromAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bDefenseRushPatched = true;
	LogMessage("[BotRouteFix] [Patch 2] Replaced IsDefenseRushing jump with 6x NOPs @ %X (Anti-Rush Activated)", g_pDefenseRushPatchAddress);

	delete gc;
}

void RestoreDefenseRushPatch()
{
	if (g_bDefenseRushPatched && g_pDefenseRushPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pDefenseRushPatchAddress + view_as<Address>(i), g_iOriginalDefenseRushBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 2] Restored original IsDefenseRushing jump @ %X", g_pDefenseRushPatchAddress);
		g_bDefenseRushPatched = false;
	}
}

//========================================================================================
// PATCH 3: T C4 CARRIER INSTANT PLANT (IsTimeToPlantBomb delay removal)
//========================================================================================

void PrepC4PlantDelayPatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_C4PlantDelay");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_C4PlantDelay!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "C4PlantDelay_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read C4PlantDelay_PatchOffset!");
		return;
	}

	g_pC4PlantDelayPatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pC4PlantDelayPatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(1), NumberType_Int8);

	// Check if already patched (0x90 = NOP)
	if (firstByte == 0x90 && secondByte == 0x90)
	{
		LogMessage("[BotRouteFix] [Patch 3] Already patched (NOPs present @ %X), skipping", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = true;
		delete gc;
		return;
	}

	// Verify expected jump opcode (32-bit: 0x0F 0x82, 64-bit: 0x0F 0x87)
	if (g_bIsWin64)
	{
		if (firstByte != 0x0F || secondByte != 0x87)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 3] Byte mismatch at %X (expected 0F 87, got %02X %02X), patch aborted!",
				g_pC4PlantDelayPatchAddress, firstByte, secondByte);
			return;
		}
	}
	else
	{
		if (firstByte != 0x0F || secondByte != 0x82)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 3] Byte mismatch at %X (expected 0F 82, got %02X %02X), patch aborted!",
				g_pC4PlantDelayPatchAddress, firstByte, secondByte);
			return;
		}
	}

	// Backup original 6 bytes
	for (int i = 0; i < 6; i++)
	{
		g_iOriginalC4PlantDelayBytes[i] = LoadFromAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), NumberType_Int8);
	}

	// Apply 6x NOPs
	for (int i = 0; i < 6; i++)
	{
		StoreToAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), 0x90, NumberType_Int8, true);
	}

	g_bC4PlantDelayPatched = true;
	LogMessage("[BotRouteFix] [Patch 3] Replaced IsTimeToPlantBomb delay jump with 6x NOPs @ %X (C4 Carrier Instant Plant Activated)", g_pC4PlantDelayPatchAddress);

	delete gc;
}

void RestoreC4PlantDelayPatch()
{
	if (g_bC4PlantDelayPatched && g_pC4PlantDelayPatchAddress != Address_Null)
	{
		for (int i = 0; i < 6; i++)
		{
			StoreToAddress(g_pC4PlantDelayPatchAddress + view_as<Address>(i), g_iOriginalC4PlantDelayBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 3] Restored original C4 plant delay jump @ %X", g_pC4PlantDelayPatchAddress);
		g_bC4PlantDelayPatched = false;
	}
}

//========================================================================================
// PATCH 4: T C4 CARRIER BOMBSITE RANDOMIZATION (GetClosestZone -> GetRandomZone)
//========================================================================================

void PrepC4RandomZonePatch()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	Address sigAddr = GameConfGetAddress(gc, "IdleState_C4PlantDelay");
	if (sigAddr == Address_Null)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to locate signature for IdleState_C4PlantDelay for Patch 4!");
		return;
	}

	int patchOffset = GameConfGetOffset(gc, "C4RandomZone_PatchOffset");
	if (patchOffset == -1)
	{
		delete gc;
		SetFailState("[BotRouteFix] Failed to read C4RandomZone_PatchOffset!");
		return;
	}

	g_pC4RandomZonePatchAddress = sigAddr + view_as<Address>(patchOffset);

	int firstByte = LoadFromAddress(g_pC4RandomZonePatchAddress, NumberType_Int8);
	int secondByte = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(1), NumberType_Int8);

	if (g_bIsWin64)
	{
		g_iC4RandomZonePatchLength = 16;

		// Check if already patched: 48 8B CB E8 (mov rcx, rbx; call GetRandomZone)
		if (firstByte == 0x48 && secondByte == 0x8B)
		{
			LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping", g_pC4RandomZonePatchAddress);
			g_bC4RandomZonePatched = true;
			delete gc;
			return;
		}

		// Verify expected original bytes: 4C 8D (lea r8, [rsp+50h])
		if (firstByte != 0x4C || secondByte != 0x8D)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 4C 8D, got %02X %02X), patch aborted!",
				g_pC4RandomZonePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 16 bytes
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x64 bytes: mov rcx, rbx; call GetRandomZone (sub_180362B90); 8x NOPs
		int patchBytes[16] = {
			0x48, 0x8B, 0xCB,             // mov rcx, rbx (TheCSBots)
			0xE8, 0x34, 0xED, 0xFF, 0xFF, // call sub_180362B90 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 8x NOPs (pad out unused PathCost parameter prep)
		};

		for (int i = 0; i < 16; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), patchBytes[i], NumberType_Int8, true);
		}
	}
	else
	{
		g_iC4RandomZonePatchLength = 23;

		// Check if already patched: 8B CE E8 (mov ecx, esi; call GetRandomZone)
		if (firstByte == 0x8B && secondByte == 0xCE)
		{
			LogMessage("[BotRouteFix] [Patch 4] Already patched @ %X, skipping", g_pC4RandomZonePatchAddress);
			g_bC4RandomZonePatched = true;
			delete gc;
			return;
		}

		// Verify expected original bytes: 8B 07 (mov eax, [edi])
		if (firstByte != 0x8B || secondByte != 0x07)
		{
			delete gc;
			SetFailState("[BotRouteFix] [Patch 4] Byte mismatch at %X (expected 8B 07, got %02X %02X), patch aborted!",
				g_pC4RandomZonePatchAddress, firstByte, secondByte);
			return;
		}

		// Backup original 23 bytes
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			g_iOriginalC4RandomZoneBytes[i] = LoadFromAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), NumberType_Int8);
		}

		// Replacement x86 bytes: mov ecx, esi; call sub_10290E50 (GetRandomZone); 16x NOPs
		int patchBytes[23] = {
			0x8B, 0xCE,                   // mov ecx, esi (TheCSBots)
			0xE8, 0xB5, 0x55, 0xFD, 0xFF, // call sub_10290E50 (GetRandomZone)
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // 16x NOPs (pad out unused PathCost parameter prep)
		};

		for (int i = 0; i < 23; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), patchBytes[i], NumberType_Int8, true);
		}
	}

	g_bC4RandomZonePatched = true;
	LogMessage("[BotRouteFix] [Patch 4] Replaced GetClosestZone with GetRandomZone @ %X (C4 50/50 Bombsite Randomization Activated)", g_pC4RandomZonePatchAddress);

	delete gc;
}

void RestoreC4RandomZonePatch()
{
	if (g_bC4RandomZonePatched && g_pC4RandomZonePatchAddress != Address_Null)
	{
		for (int i = 0; i < g_iC4RandomZonePatchLength; i++)
		{
			StoreToAddress(g_pC4RandomZonePatchAddress + view_as<Address>(i), g_iOriginalC4RandomZoneBytes[i], NumberType_Int8, true);
		}
		LogMessage("[BotRouteFix] [Patch 4] Restored original GetClosestZone @ %X", g_pC4RandomZonePatchAddress);
		g_bC4RandomZonePatched = false;
	}
}

//========================================================================================
// MODULE 3: DYNAMIC CORRIDOR DANGER RESERVATION (CCSBot::ComputePath Post Detour)
//========================================================================================

void PrepComputePathHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hComputePathDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hComputePathDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to create DHook detour for CCSBot_ComputePath!");
		return;
	}

	if (!DHookSetFromConf(g_hComputePathDetour, gc, SDKConf_Signature, "CCSBot_ComputePath"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to find signature for CCSBot_ComputePath in gamedata!");
		return;
	}

	DHookAddParam(g_hComputePathDetour, HookParamType_VectorPtr);
	DHookAddParam(g_hComputePathDetour, HookParamType_Int);

	if (!DHookEnableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 3] Failed to enable post detour for CCSBot_ComputePath!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 3] Hooked CCSBot::ComputePath Post (Dynamic Corridor Reservation Activated)");
	delete gc;
}

void RestoreComputePathHook()
{
	if (g_hComputePathDetour != null)
	{
		DHookDisableDetour(g_hComputePathDetour, true, Hook_ComputePath_Post);
		delete g_hComputePathDetour;
		g_hComputePathDetour = null;
	}
}

public MRESReturn Hook_ComputePath_Post(int client, Handle hReturn, Handle hParams)
{
	// If pathfinding failed, do not inject any corridor reservation
	if (!DHookGetReturn(hReturn))
		return MRES_Ignored;

	if (client <= 0 || client > MaxClients || !IsClientInGame(client))
		return MRES_Ignored;

	int team = GetClientTeam(client);
	if (team != 2 && team != 3) // 2=T, 3=CT
		return MRES_Ignored;

	int teamIdx = team - 2; // T=0, CT=1 (CS:S internal MAX_NAV_TEAMS index)

	Address pBot = GetEntityAddress(client);
	if (pBot == Address_Null)
		return MRES_Ignored;

	int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
	if (pathLength <= 0)
		return MRES_Ignored;

	// Limit corridor reservation depth to first 5 nav areas ahead
	int maxNodes = pathLength;
	if (maxNodes > 5)
		maxNodes = 5;

	float curtime = GetGameTime();

	for (int i = 0; i < maxNodes; i++)
	{
		Address pConnectInfo = pBot + view_as<Address>(g_iOffset_Path + i * g_iPathStride);
		Address pArea = LoadAddressFromAddress(pConnectInfo);
		if (pArea == Address_Null)
			continue;

		Address pDangerAddr = pArea + view_as<Address>(g_iOffset_Danger + teamIdx * 4);
		Address pTimestampAddr = pArea + view_as<Address>(g_iOffset_DangerTimestamp + teamIdx * 4);

		float currentDanger = view_as<float>(LoadFromAddress(pDangerAddr, NumberType_Int32));
		float lastTimestamp = view_as<float>(LoadFromAddress(pTimestampAddr, NumberType_Int32));

		// Decay danger based on elapsed time (rate: 1.0 / 120.0 per second)
		float deltaT = curtime - lastTimestamp;
		if (deltaT > 0.0)
		{
			float decay = (1.0 / 120.0) * deltaT;
			currentDanger -= decay;
			if (currentDanger < 0.0)
				currentDanger = 0.0;
		}

		// Inject dynamic reservation penalty (+0.8f per bot)
		currentDanger += 0.8;
		if (currentDanger > 2.5)
			currentDanger = 2.5;

		StoreToAddress(pDangerAddr, view_as<int>(currentDanger), NumberType_Int32);
		StoreToAddress(pTimestampAddr, view_as<int>(curtime), NumberType_Int32);
	}

	return MRES_Ignored;
}

//========================================================================================
// MODULE 4: 1.2S QUEUE BREAKER & DOORWAY TURNAROUND
//========================================================================================

void StartQueueBreakerTimer()
{
	if (g_hQueueBreakerTimer == INVALID_HANDLE)
	{
		g_hQueueBreakerTimer = CreateTimer(0.1, Timer_CheckPoliteQueue, _, TIMER_REPEAT);
		LogMessage("[BotRouteFix] [Module 4] 1.2s Queue Breaker Timer Started (Doorway Jam Prevention Activated)");
	}
}

void StopQueueBreakerTimer()
{
	if (g_hQueueBreakerTimer != INVALID_HANDLE)
	{
		KillTimer(g_hQueueBreakerTimer);
		g_hQueueBreakerTimer = INVALID_HANDLE;
	}
}

public Action Timer_CheckPoliteQueue(Handle timer)
{
	float curtime = GetGameTime();

	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsPlayerAlive(i) || !IsFakeClient(i))
			continue;

		Address pBot = GetEntityAddress(i);
		if (pBot == Address_Null)
			continue;

		int isWaiting = LoadFromAddress(pBot + view_as<Address>(g_iOffset_IsWaitingBehindFriend), NumberType_Int8);
		if (!isWaiting)
			continue;

		int pathLength = LoadFromAddress(pBot + view_as<Address>(g_iOffset_PathLength), NumberType_Int32);
		if (pathLength <= 0)
			continue;

		// Read CountdownTimer m_duration and m_timestamp (offset: +8/+12 on x64, +4/+8 on x86)
		int durationOffset = g_bIsWin64 ? (g_iOffset_PoliteTimer + 8) : (g_iOffset_PoliteTimer + 4);
		int timestampOffset = g_bIsWin64 ? (g_iOffset_PoliteTimer + 12) : (g_iOffset_PoliteTimer + 8);

		float duration = view_as<float>(LoadFromAddress(pBot + view_as<Address>(durationOffset), NumberType_Int32));
		float timestamp = view_as<float>(LoadFromAddress(pBot + view_as<Address>(timestampOffset), NumberType_Int32));

		float startTime = timestamp - duration;
		float elapsedWait = curtime - startTime;

		// If bot has been politely waiting behind a friend for >= 1.2s, break the queue
		if (elapsedWait >= 1.2 && duration > 0.0)
		{
			// 1. Clear waiting behind friend flag
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsWaitingBehindFriend), 0, NumberType_Int8);

			// 2. DestroyPath: clear stopping, pathLength, and pathLadder to force immediate repath
			StoreToAddress(pBot + view_as<Address>(g_iOffset_IsStopping), 0, NumberType_Int8);
			StoreToAddress(pBot + view_as<Address>(g_iOffset_PathLength), 0, NumberType_Int32);
			StoreAddressToAddress(pBot + view_as<Address>(g_iOffset_PathLadder), Address_Null);
		}
	}

	return Plugin_Continue;
}

//========================================================================================
// MODULE 5: T C4 CARRIER ESCORT & BODYGUARD SYSTEM
//========================================================================================

void PrepFollowSDKCalls()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_Follow");
	PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer);
	g_hFollowSDKCall = EndPrepSDKCall();
	if (g_hFollowSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 5] Failed to create SDKCall for CCSBot_Follow!");
		return;
	}

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_StopFollowing");
	g_hStopFollowingSDKCall = EndPrepSDKCall();
	if (g_hStopFollowingSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 5] Failed to create SDKCall for CCSBot_StopFollowing!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 5] Follow & StopFollowing SDKCalls Prepared (Carrier Escort Activated)");
	delete gc;
}

void HookGameEvents()
{
	HookEvent("round_freeze_end", Event_RoundFreezeEnd, EventHookMode_Post);
	HookEvent("item_pickup", Event_ItemPickup, EventHookMode_Post);
	HookEvent("bomb_dropped", Event_BombDropped, EventHookMode_Post);
	HookEvent("bomb_planted", Event_BombPlanted, EventHookMode_Post);
	HookEvent("bomb_defused", Event_BombEnded, EventHookMode_Post);
	HookEvent("bomb_exploded", Event_BombEnded, EventHookMode_Post);
	HookEvent("round_start", Event_RoundStart, EventHookMode_Post);
	HookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
}

public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = false;
	g_iDesignatedDefuser = 0;
}

public void Event_RoundFreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
	AssignCarrierBodyguards();
}

public void Event_ItemPickup(Event event, const char[] name, bool dontBroadcast)
{
	char item[32];
	event.GetString("item", item, sizeof(item));

	if (StrEqual(item, "c4") || StrEqual(item, "weapon_c4"))
	{
		RequestFrame(Frame_AssignCarrierBodyguards);
	}
}

public void Frame_AssignCarrierBodyguards(any data)
{
	AssignCarrierBodyguards();
}

public void Event_BombDropped(Event event, const char[] name, bool dontBroadcast)
{
	// When C4 is dropped on the floor, release bodyguards so they can react and cover
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			if (g_hStopFollowingSDKCall != null)
			{
				SDKCall(g_hStopFollowingSDKCall, i);
			}
		}
	}
}

public void Event_BombPlanted(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = true;

	int bomb = FindEntityByClassname(-1, "planted_c4");
	if (bomb != -1 && IsValidEntity(bomb))
	{
		GetEntPropVector(bomb, Prop_Data, "m_vecAbsOrigin", g_fBombPlantedPos);
	}

	UpdateDesignatedDefuser();
}

public void Event_BombEnded(Event event, const char[] name, bool dontBroadcast)
{
	g_bBombPlanted = false;
	g_iDesignatedDefuser = 0;
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
	int victim = GetClientOfUserId(event.GetInt("userid"));
	if (victim > 0 && victim == g_iDesignatedDefuser)
	{
		UpdateDesignatedDefuser();
	}
}

void AssignCarrierBodyguards()
{
	int carrier = GetC4Carrier();
	if (carrier <= 0 || !IsPlayerAlive(carrier))
		return;

	// Reset all active followers first to strictly enforce the bodyguard quota
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			if (g_hStopFollowingSDKCall != null)
			{
				SDKCall(g_hStopFollowingSDKCall, i);
			}
		}
	}

	// Collect alive T teammates (excluding the carrier)
	int teammates[MAXPLAYERS + 1];
	int teammateCount = 0;
	float carrierOrigin[3];
	GetClientAbsOrigin(carrier, carrierOrigin);

	for (int i = 1; i <= MaxClients; i++)
	{
		if (i == carrier)
			continue;

		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2 && IsFakeClient(i))
		{
			teammates[teammateCount++] = i;
		}
	}

	if (teammateCount <= 0)
		return;

	// Determine bodyguard count based on alive teammates ratio:
	// - 1 teammate left: 1 follows
	// - 2 teammates left: 1 follows, 1 free
	// - >= 3 teammates left: 2 follow, remaining free
	int bodyguardsToAssign = 1;
	if (teammateCount >= 3)
		bodyguardsToAssign = 2;

	// Sort teammates by distance to carrier (ascending)
	for (int i = 0; i < teammateCount - 1; i++)
	{
		for (int j = i + 1; j < teammateCount; j++)
		{
			float posI[3], posJ[3];
			GetClientAbsOrigin(teammates[i], posI);
			GetClientAbsOrigin(teammates[j], posJ);

			if (GetVectorDistance(carrierOrigin, posJ) < GetVectorDistance(carrierOrigin, posI))
			{
				int temp = teammates[i];
				teammates[i] = teammates[j];
				teammates[j] = temp;
			}
		}
	}

	// Assign the closest bodyguards
	for (int i = 0; i < bodyguardsToAssign && i < teammateCount; i++)
	{
		int bot = teammates[i];
		if (g_hFollowSDKCall != null)
		{
			SDKCall(g_hFollowSDKCall, bot, carrier);
			LogMessage("[BotRouteFix] [Module 5] Assigned %N to escort C4 carrier %N", bot, carrier);
		}
	}
}

int GetC4Carrier()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2)
		{
			int c4 = GetPlayerWeaponSlot(i, 4);
			if (c4 != -1 && IsValidEntity(c4))
				return i;
		}
	}
	return 0;
}

//========================================================================================
// MODULE 6: HUNT TARGET CLAIM & FLUSH (HuntState::OnUpdate Post Detour)
//========================================================================================

void PrepHuntStateHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hHuntStateDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Void, ThisPointer_Address);
	if (g_hHuntStateDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to create DHook detour for HuntState_OnUpdate!");
		return;
	}

	if (!DHookSetFromConf(g_hHuntStateDetour, gc, SDKConf_Signature, "HuntState_OnUpdate"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to find signature for HuntState_OnUpdate in gamedata!");
		return;
	}

	DHookAddParam(g_hHuntStateDetour, HookParamType_CBaseEntity);

	if (!DHookEnableDetour(g_hHuntStateDetour, true, Hook_HuntState_OnUpdate_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 6] Failed to enable post detour for HuntState_OnUpdate!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 6] Hooked HuntState::OnUpdate Post (Target Claim & Anti-Swarm Activated)");
	delete gc;
}

void RestoreHuntStateHook()
{
	if (g_hHuntStateDetour != null)
	{
		DHookDisableDetour(g_hHuntStateDetour, true, Hook_HuntState_OnUpdate_Post);
		delete g_hHuntStateDetour;
		g_hHuntStateDetour = null;
	}
}

public MRESReturn Hook_HuntState_OnUpdate_Post(Address pThis, DHookParam hParams)
{
	if (pThis == Address_Null)
		return MRES_Ignored;

	int client = hParams.Get(1);
	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
		return MRES_Ignored;

	int team = GetClientTeam(client);
	if (team != 2 && team != 3) // 2=T, 3=CT
		return MRES_Ignored;

	int teamIdx = team - 2; // T=0, CT=1 (CS:S internal MAX_NAV_TEAMS index)

	// Read m_huntArea from HuntState instance (+4 on 32-bit, +8 on 64-bit)
	Address pArea = LoadAddressFromAddress(pThis + view_as<Address>(g_iOffset_HuntArea));
	if (pArea == Address_Null)
		return MRES_Ignored;

	// Flush and claim this area immediately: set m_clearedTimestamp[teamIdx] = curtime
	float curtime = GetGameTime();
	Address pClearedTimestampAddr = pArea + view_as<Address>(g_iOffset_ClearedTimestamp + teamIdx * 4);
	StoreToAddress(pClearedTimestampAddr, view_as<int>(curtime), NumberType_Int32);

	return MRES_Ignored;
}

//========================================================================================
// MODULE 7: SINGLE RETRIEVER & TACTICAL COVER (CCSBot::NoticeLooseBomb Post Detour)
//========================================================================================

void PrepNoticeLooseBombHook()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	g_hNoticeLooseBombDetour = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (g_hNoticeLooseBombDetour == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to create DHook detour for CCSBot_NoticeLooseBomb!");
		return;
	}

	if (!DHookSetFromConf(g_hNoticeLooseBombDetour, gc, SDKConf_Signature, "CCSBot_NoticeLooseBomb"))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to find signature for CCSBot_NoticeLooseBomb in gamedata!");
		return;
	}

	if (!DHookEnableDetour(g_hNoticeLooseBombDetour, true, Hook_NoticeLooseBomb_Post))
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 7] Failed to enable post detour for CCSBot_NoticeLooseBomb!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 7] Hooked CCSBot::NoticeLooseBomb Post (Single Retriever & Tactical Cover Activated)");
	delete gc;
}

void RestoreNoticeLooseBombHook()
{
	if (g_hNoticeLooseBombDetour != null)
	{
		DHookDisableDetour(g_hNoticeLooseBombDetour, true, Hook_NoticeLooseBomb_Post);
		delete g_hNoticeLooseBombDetour;
		g_hNoticeLooseBombDetour = null;
	}
}

public MRESReturn Hook_NoticeLooseBomb_Post(int client, Handle hReturn)
{
	if (client <= 0 || client > MaxClients || !IsClientInGame(client) || !IsPlayerAlive(client))
		return MRES_Ignored;

	if (GetClientTeam(client) != 2) // Terrorists only
		return MRES_Ignored;

	// Only intercept if the official logic returned true (meaning loose C4 exists)
	if (!DHookGetReturn(hReturn))
		return MRES_Ignored;

	int looseBomb = GetLooseBombEntity();
	if (looseBomb == -1)
		return MRES_Ignored;

	float bombPos[3];
	GetEntPropVector(looseBomb, Prop_Data, "m_vecAbsOrigin", bombPos);

	int closestT = 0;
	float closestDist = 99999999.0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 2)
		{
			float pos[3];
			GetClientAbsOrigin(i, pos);
			float dist = GetVectorDistance(pos, bombPos);
			if (dist < closestDist)
			{
				closestDist = dist;
				closestT = i;
			}
		}
	}

	// Only the single closest T retrieves the bomb; all other Ts maintain HuntState to provide cover!
	if (client == closestT)
	{
		return MRES_Ignored;
	}
	else
	{
		DHookSetReturn(hReturn, false);
		return MRES_Override;
	}
}

int GetLooseBombEntity()
{
	int ent = -1;
	while ((ent = FindEntityByClassname(ent, "weapon_c4")) != -1)
	{
		if (IsValidEntity(ent))
		{
			int owner = GetEntPropEnt(ent, Prop_Data, "m_hOwnerEntity");
			if (owner <= 0 || owner > MaxClients)
			{
				return ent;
			}
		}
	}
	return -1;
}

//========================================================================================
// MODULE 8: CT DESIGNATED DEFUSER & CROSSFIRE GUARD
//========================================================================================

void PrepDefuseSDKCalls()
{
	Handle gc = LoadGameConfigFile(GAMEDATA);
	if (gc == null)
		return;

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(gc, SDKConf_Signature, "CCSBot_Hide");
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain); // CNavArea *area
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);        // float duration
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);        // float holdPositionTime
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);         // bool isStealth
	g_hHideSDKCall = EndPrepSDKCall();
	if (g_hHideSDKCall == null)
	{
		delete gc;
		SetFailState("[BotRouteFix] [Module 8] Failed to create SDKCall for CCSBot_Hide!");
		return;
	}

	LogMessage("[BotRouteFix] [Module 8] Hide SDKCall Prepared (CT Defuse & Crossfire Guard Activated)");
	delete gc;
}

void StartDefuseCoordTimer()
{
	if (g_hDefuseCoordTimer == INVALID_HANDLE)
	{
		g_hDefuseCoordTimer = CreateTimer(0.2, Timer_DefuseCoordination, _, TIMER_REPEAT);
	}
}

void StopDefuseCoordTimer()
{
	if (g_hDefuseCoordTimer != INVALID_HANDLE)
	{
		KillTimer(g_hDefuseCoordTimer);
		g_hDefuseCoordTimer = INVALID_HANDLE;
	}
}

public Action Timer_DefuseCoordination(Handle timer)
{
	if (!g_bBombPlanted)
		return Plugin_Continue;

	UpdateDesignatedDefuser();
	if (g_iDesignatedDefuser <= 0)
		return Plugin_Continue;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsPlayerAlive(i) || GetClientTeam(i) != 3 || !IsFakeClient(i))
			continue;

		if (i == g_iDesignatedDefuser)
			continue;

		float ctPos[3];
		GetClientAbsOrigin(i, ctPos);
		float distToBomb = GetVectorDistance(ctPos, g_fBombPlantedPos);

		// Distance Gating: Only switch to Hide/Guard when CT enters the bombsite perimeter (<= 1500 units)
		// If farther away, let them continue sprinting to the bombsite via MoveTo!
		if (distToBomb > 1500.0)
			continue;

		Address pBot = GetEntityAddress(i);
		if (pBot == Address_Null)
			continue;

		int task = LoadFromAddress(pBot + view_as<Address>(g_iOffset_Task), NumberType_Int32);
		// If a non-defuser CT bot is attempting task 3 (DEFUSE_BOMB):
		if (task == 3) // DEFUSE_BOMB
		{
			// Switch task to GUARD_BOMB_DEFUSER (task 5) and take cover to set up crossfire
			StoreToAddress(pBot + view_as<Address>(g_iOffset_Task), 5, NumberType_Int32);
			if (g_hHideSDKCall != null)
			{
				SDKCall(g_hHideSDKCall, i, Address_Null, -1.0, 0.0, false);
				LogMessage("[BotRouteFix] [Module 8] CT %N reached site perimeter (%.0f units) -> assigned crossfire guard (Lead defuser is %N)",
					i, distToBomb, g_iDesignatedDefuser);
			}
		}
	}

	return Plugin_Continue;
}

void UpdateDesignatedDefuser()
{
	if (!g_bBombPlanted)
	{
		g_iDesignatedDefuser = 0;
		return;
	}

	int bestCT = 0;
	float bestScore = 99999999.0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsPlayerAlive(i) && GetClientTeam(i) == 3)
		{
			float pos[3];
			GetClientAbsOrigin(i, pos);
			float dist = GetVectorDistance(pos, g_fBombPlantedPos);

			// Bonus if CT has defusal kit (-500 units priority)
			bool hasKit = (GetEntProp(i, Prop_Send, "m_bHasDefuser") == 1);
			float score = hasKit ? (dist - 500.0) : dist;

			if (score < bestScore)
			{
				bestScore = score;
				bestCT = i;
			}
		}
	}

	if (bestCT != g_iDesignatedDefuser && bestCT > 0)
	{
		g_iDesignatedDefuser = bestCT;
		LogMessage("[BotRouteFix] [Module 8] Designated %N as lead defuser", g_iDesignatedDefuser);
	}
}
