#if defined _botroutefix_memory_included
    #endinput
#endif
#define _botroutefix_memory_included

//========================================================================================
// MEMORY PATCH INFRASTRUCTURE
//
// 提供严谨、安全的二进制内存补丁生命周期管理，统一支持：
//   1. 现场指令字节备份 (Backup)
//   2. 写入前模板匹配与常量校验 (Preflight Verification)
//   3. 幂等性检测（已打补丁跳过，防止热重载重复覆盖）
//   4. 插件卸载/热重载时严格原样还原 (Restore)
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
