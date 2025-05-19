// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include "core/arm/arm_interface.h"
#include "core/arm/nce/guest_context.h"
#include "core/arm/nce/host_mapped_memory.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

class System;

class ArmNce final : public ArmInterface {
public:
    ArmNce(System& system, bool uses_wall_clock, std::size_t core_index);
    ~ArmNce() override;

    void Initialize() override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    void LockThread(Kernel::KThread* thread) override;
    void UnlockThread(Kernel::KThread* thread) override;

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

    // Fast memory access using host-mapped memory (inspired by Ryujinx)
    template <typename T>
    T& GetHostRef(u64 guest_addr);

private:
    // Assembly definitions.
    static HaltReason ReturnToRunCodeByTrampoline(void* tpidr, GuestContext* ctx,
                                                  u64 trampoline_addr);
    static HaltReason ReturnToRunCodeByExceptionLevelChange(int tid, void* tpidr);

    static void ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void* info,
                                                                   void* raw_context);
    static void BreakFromRunCodeSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAlignmentFaultSignalHandler(int sig, void* info, void* raw_context);
    static void GuestAccessFaultSignalHandler(int sig, void* info, void* raw_context);

    static void LockThreadParameters(void* tpidr);
    static void UnlockThreadParameters(void* tpidr);

    // Alternate stack management (inspired by Ryujinx)
    void SetupAlternateSignalStack();
    void CleanupAlternateSignalStack();

    // Enhanced signal handling
    static bool HandleThreadInterrupt(GuestContext* ctx);

private:
    // C++ implementation functions for assembly definitions.
    static void* RestoreGuestContext(void* raw_context);
    static void SaveGuestContext(GuestContext* ctx, void* raw_context);
    static bool HandleFailedGuestFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAlignmentFault(GuestContext* ctx, void* info, void* raw_context);
    static bool HandleGuestAccessFault(GuestContext* ctx, void* info, void* raw_context);
    static void HandleHostAlignmentFault(int sig, void* info, void* raw_context);
    static void HandleHostAccessFault(int sig, void* info, void* raw_context);

public:
    Core::System& m_system;

    // Members set on initialization.
    std::size_t m_core_index{};
    pid_t m_thread_id{-1};

    // Core context.
    GuestContext m_guest_ctx{};
    Kernel::KThread* m_running_thread{};

    // Stack for signal processing.
    std::unique_ptr<u8[]> m_stack{};

    // Alternate signal stack (inspired by Ryujinx)
    static constexpr size_t AlternateStackSize = 16384;
    std::unique_ptr<u8[]> m_alt_signal_stack{};

    // Host mapped memory for efficient access (inspired by Ryujinx)
    std::unique_ptr<HostMappedMemory> m_host_mapped_memory{};
};

} // namespace Core
