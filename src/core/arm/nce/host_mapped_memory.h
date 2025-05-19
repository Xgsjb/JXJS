// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/page_table.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

/**
 * HostMappedMemory provides direct host-mapped memory access for NCE
 * This is inspired by Ryujinx's MemoryManagerNative for faster memory operations
 */
class HostMappedMemory {
public:
    explicit inline HostMappedMemory(Memory::Memory& memory) : memory{memory} {}
    inline ~HostMappedMemory() {
        // Unmap all allocations
        for (void* allocation : allocations) {
            if (munmap(allocation, page_size) != 0) {
                LOG_ERROR(Core_ARM, "Failed to unmap allocation at {:p}: {}", allocation, std::strerror(errno));
            }
        }
    }

    /**
     * Maps a guest memory region to host memory
     * @param guest_addr Guest virtual address to map
     * @param size Size of the region to map
     * @return True if the mapping succeeded
     */
    inline bool MapRegion(u64 guest_addr, u64 size) {
        const u64 start_page = guest_addr >> page_bits;
        const u64 end_page = (guest_addr + size + page_mask) >> page_bits;

        for (u64 page = start_page; page < end_page; page++) {
            const u64 current_addr = page << page_bits;

            // Skip if already mapped
            if (page_table.contains(page)) {
                continue;
            }

            // Allocate memory for this page
            void* allocation = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (allocation == MAP_FAILED) {
                LOG_ERROR(Core_ARM, "Failed to allocate host page for guest address {:016X}: {}",
                          current_addr, std::strerror(errno));
                return false;
            }

            // Copy data from guest memory to our allocation
            bool result = false;
            std::array<u8, page_size> data;

            // Try to read the memory from guest
            result = memory.ReadBlock(current_addr, data.data(), page_size);
            if (!result) {
                LOG_ERROR(Core_ARM, "Failed to read memory block at {:016X}", current_addr);
                munmap(allocation, page_size);
                return false;
            }

            // Copy to our allocation
            std::memcpy(allocation, data.data(), page_size);

            // Store the allocation
            page_table[page] = static_cast<u8*>(allocation);
            allocations.push_back(allocation);
        }

        return true;
    }

    /**
     * Unmaps a previously mapped guest memory region
     * @param guest_addr Guest virtual address to unmap
     * @param size Size of the region to unmap
     */
    inline void UnmapRegion(u64 guest_addr, u64 size) {
        const u64 start_page = guest_addr >> page_bits;
        const u64 end_page = (guest_addr + size + page_mask) >> page_bits;

        for (u64 page = start_page; page < end_page; page++) {
            // Skip if not mapped
            auto it = page_table.find(page);
            if (it == page_table.end()) {
                continue;
            }

            u8* host_ptr = it->second;

            // Try to write the memory back to guest
            const u64 current_addr = page << page_bits;
            memory.WriteBlock(current_addr, host_ptr, page_size);

            // Remove from page table
            page_table.erase(it);

            // Don't unmap immediately - we'll do that in the destructor
            // to avoid potential reuse problems
        }
    }

    /**
     * Gets a typed reference to memory at the specified guest address
     * @tparam T Type of the reference to return
     * @param guest_addr Guest virtual address
     * @return Reference to the memory at the specified address
     * @note The memory region must be continuous and mapped
     */
    template <typename T>
    T& GetRef(u64 guest_addr) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

        // Check if region covers the entire value
        const u64 page_offset = guest_addr & page_mask;

        if (page_offset + sizeof(T) <= page_size) {
            // Fast path - contained within single page
            return *reinterpret_cast<T*>(TranslateAddress(guest_addr));
        } else {
            // Slow path - spans pages, need to check if all are mapped
            if (!IsRangeMapped(guest_addr, sizeof(T))) {
                throw std::runtime_error("Memory region is not continuous");
            }
            return *reinterpret_cast<T*>(TranslateAddress(guest_addr));
        }
    }

    /**
     * Gets a span over memory at the specified guest address
     * @param guest_addr Guest virtual address
     * @param size Size of the span
     * @return Span over the memory at the specified address
     * @note The memory region must be continuous and mapped
     */
    inline std::span<u8> GetSpan(u64 guest_addr, u64 size) {
        // Ensure the memory is mapped and continuous
        if (!IsRangeMapped(guest_addr, size)) {
            throw std::runtime_error("GetSpan requested on unmapped or non-continuous memory region");
        }

        return std::span<u8>(TranslateAddress(guest_addr), size);
    }

    /**
     * Checks if an address is mapped
     * @param guest_addr Guest virtual address to check
     * @return True if the address is mapped
     */
    inline bool IsMapped(u64 guest_addr) const {
        const u64 page = guest_addr >> page_bits;
        return page_table.contains(page);
    }

    /**
     * Checks if a range of memory is mapped continuously
     * @param guest_addr Starting guest virtual address
     * @param size Size of the region to check
     * @return True if the entire range is mapped continuously
     */
    inline bool IsRangeMapped(u64 guest_addr, u64 size) const {
        const u64 start_page = guest_addr >> page_bits;
        const u64 end_page = (guest_addr + size + page_mask) >> page_bits;

        for (u64 page = start_page; page < end_page; page++) {
            if (!page_table.contains(page)) {
                return false;
            }
        }

        return true;
    }

    /**
     * Gets the host address for a guest virtual address
     * @param guest_addr Guest virtual address to translate
     * @return Host address corresponding to the guest address
     */
    inline u8* TranslateAddress(u64 guest_addr) {
        const u64 page = guest_addr >> page_bits;
        const u64 offset = guest_addr & page_mask;

        auto it = page_table.find(page);
        if (it == page_table.end()) {
            throw std::runtime_error(fmt::format("Tried to translate unmapped address {:016X}", guest_addr));
        }

        return it->second + offset;
    }

private:
    static constexpr u64 page_bits = 12;
    static constexpr u64 page_size = 1ULL << page_bits;
    static constexpr u64 page_mask = page_size - 1;

    Memory::Memory& memory;

    // Page table mapping guest pages to host addresses
    std::unordered_map<u64, u8*> page_table;

    // Allocation pool for mapped regions
    std::vector<void*> allocations;
};

} // namespace Core
