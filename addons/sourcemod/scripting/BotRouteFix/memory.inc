#if defined _botroutefix_memory_included
    #endinput
#endif
#define _botroutefix_memory_included

//========================================================================================
// MEMORY PATCH & DETOUR INFRASTRUCTURE
//
// 提供严谨、安全的底层改写生命周期管理：
//
// MemoryPatch —— 二进制内存补丁：
//   1. 现场指令字节备份 (Backup)
//   2. 写入前模板匹配与常量校验 (Preflight Verification)
//   3. 幂等性检测（已打补丁跳过，防止热重载重复覆盖）
//   4. 插件卸载/热重载时严格原样还原 (Restore)
//
// SyncDetourEnabled —— DHook 状态机收敛：
//   SourceMod 的 DHookDisableDetour 在钩子未挂载时抛 "Function not detoured" 致命错误，
//   重复 Enable 亦会污染 DHook 内部状态机。每个钩子必须持有自己的真实启用态，
//   只在目标态与现状不一致时才调用底层 Enable/Disable。
//========================================================================================

enum struct MemoryPatch
{
    char name[32];                  // 补丁名称标识
    Address address;                // 目标绝对内存地址
    int size;                       // 补丁覆盖字节数
    int originalBytes[MAX_PATCH_SIZE]; // 原始字节备份
    bool isPatched;                 // 当前生效状态
    bool hasBackup;                 // 是否已锁定原始基线字节备份

    // 初始化补丁结构
    void Init(const char[] patchName, Address addr, int patchSize)
    {
        strcopy(this.name, sizeof(this.name), patchName);

        // 同一目标重入（Enable -> Disable -> Enable）：保留原始基线备份与生效旗标。
        // 若在此抹掉 hasBackup，后续幂等检测会把自家补丁误判为"外来补丁"而记
        // isPatched=false，Restore() 随即被零写入熔断拦截 —— 补丁永久卡死无法卸载。
        if (this.hasBackup && this.address == addr && this.size == patchSize)
        {
            return;
        }

        this.address = addr;
        this.size = patchSize;
        this.isPatched = false;
        this.hasBackup = false;

        for (int i = 0; i < MAX_PATCH_SIZE; i++)
        {
            this.originalBytes[i] = 0;
        }
    }

    // 备份原始指令字节（仅在初次备份时固化，防止二次补丁污染原始基线）
    bool BackupOriginal()
    {
        if (this.hasBackup)
        {
            return true;
        }

        if (this.address == Address_Null || this.size <= 0 || this.size > MAX_PATCH_SIZE)
        {
            return false;
        }

        for (int i = 0; i < this.size; i++)
        {
            this.originalBytes[i] = LoadFromAddress(this.address + view_as<Address>(i), NumberType_Int8);
        }
        this.hasBackup = true;
        return true;
    }

    // 写入目标字节补丁
    bool Apply(const int[] patchBytes)
    {
        if (this.address == Address_Null || this.size <= 0 || this.size > MAX_PATCH_SIZE)
        {
            return false;
        }

        // 若尚未备份，则先行固化当前基线
        if (!this.hasBackup)
        {
            this.BackupOriginal();
        }

        for (int i = 0; i < this.size; i++)
        {
            StoreToAddress(this.address + view_as<Address>(i), patchBytes[i], NumberType_Int8, true);
        }

        this.isPatched = true;
        return true;
    }

    // 使用 NOP (0x90) 填充目标指令区
    bool NopOut()
    {
        if (this.address == Address_Null || this.size <= 0 || this.size > MAX_PATCH_SIZE)
        {
            return false;
        }

        if (!this.hasBackup)
        {
            this.BackupOriginal();
        }

        for (int i = 0; i < this.size; i++)
        {
            StoreToAddress(this.address + view_as<Address>(i), 0x90, NumberType_Int8, true);
        }

        this.isPatched = true;
        return true;
    }

    // 还原原始现场字节
    bool Restore()
    {
        if (!this.isPatched || this.address == Address_Null || this.size <= 0)
        {
            return false;
        }

        // 核心安全熔断：若未锁定原始基线，绝对禁止向代码段写回零字节，防止服务器崩溃
        if (!this.hasBackup)
        {
            LogError("[BotRouteFix] [MemoryPatch] Refusing to restore '%s' @ %X: no valid baseline backup present!",
                this.name, this.address);
            this.isPatched = false;
            return false;
        }

        for (int i = 0; i < this.size; i++)
        {
            StoreToAddress(this.address + view_as<Address>(i), this.originalBytes[i], NumberType_Int8, true);
        }

        this.isPatched = false;
        return true;
    }
}

//========================================================================================
// DETOUR HOOK STATE SYNCHRONIZER
//========================================================================================

/**
 * 幂等同步单个 DHook Detour 的启用态
 *
 * 调用前先比对 per-hook 真实启用态，只有目标态与现状不一致时才动作，
 * 从根上杜绝重复 Enable/Disable 引发的 "Function not detoured" 致命状态机异常。
 *
 * @param hook       Detour 句柄（null = 未挂载，直接返回 false）
 * @param enabled    Per-hook 真实启用态（唯一事实来源，调用成功后更新）
 * @param post       true = post 钩子, false = pre 钩子
 * @param callback   回调函数
 * @param want       目标启用态
 * @param label      日志标识（模块名）
 * @return           调用后钩子是否处于目标态
 */
bool SyncDetourEnabled(Handle hook, bool &enabled, bool post, DHookCallback callback, bool want, const char[] label)
{
    if (hook == null)
    {
        // 钩子从未挂载（待机或因签名缺失降级）：不存在可同步的底层状态
        enabled = false;
        return false;
    }

    if (enabled == want)
    {
        return true;
    }

    bool ok = want ? DHookEnableDetour(hook, post, callback) : DHookDisableDetour(hook, post, callback);
    if (!ok)
    {
        LogError("[BotRouteFix] [%s] %s detour %s hook failed!", label, want ? "Enable" : "Disable", post ? "post" : "pre");
        return false;
    }

    enabled = want;
    return true;
}
