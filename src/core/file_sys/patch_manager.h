// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/memory/dmnt_cheat_types.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {
class ExternalContentManager;
class ContentProvider;
class NCA;
class NACP;

enum class PatchType { Update, DLC, Mod };

struct Patch {
    bool enabled;
    std::string name;
    std::string version;
    PatchType type;
    u64 program_id;
    u64 title_id;
};

// A centralized class to manage patches to games.
class PatchManager {
public:
    using BuildID = std::array<u8, 0x20>;
    using Metadata = std::pair<std::unique_ptr<NACP>, VirtualFile>;

    explicit PatchManager(u64 title_id_,
                          const Service::FileSystem::FileSystemController& fs_controller_,
                          const ContentProvider& content_provider_);
    ~PatchManager();

    [[nodiscard]] u64 GetTitleID() const;

    // Currently tracked ExeFS patches:
    // - Game Updates
    [[nodiscard]] VirtualDir PatchExeFS(VirtualDir exefs) const;

    // Currently tracked NSO patches:
    // - IPS
    // - IPSwitch
    [[nodiscard]] std::vector<u8> PatchNSO(const std::vector<u8>& nso,
                                           const std::string& name) const;

    // Checks to see if PatchNSO() will have any effect given the NSO's build ID.
    // Used to prevent expensive copies in NSO loader.
    [[nodiscard]] bool HasNSOPatch(const BuildID& build_id, std::string_view name) const;

    // Creates a CheatList object with all
    [[nodiscard]] std::vector<Core::Memory::CheatEntry> CreateCheatList(
        const BuildID& build_id) const;

    // Currently tracked RomFS patches:
    // - Game Updates
    // - LayeredFS
    [[nodiscard]] VirtualFile PatchRomFS(const NCA* base_nca, VirtualFile base_romfs,
                                         ContentRecordType type = ContentRecordType::Program,
                                         VirtualFile packed_update_raw = nullptr,
                                         bool apply_layeredfs = true) const;

    /**
     * Applies an external update file to patch a game's RomFS.
     * @param base_nca The base NCA of the content being patched
     * @param base_romfs The original RomFS to be patched
     * @param external_update The external update file (NSP/XCI) containing the update
     * @param type The type of content being patched (Program, Control, etc.)
     * @return Patched RomFS VirtualFile if successful, nullptr otherwise
     */
    VirtualFile PatchRomFSWithExternal(const NCA *base_nca, VirtualFile base_romfs,
                                       VirtualFile external_update, ContentRecordType type) const;

    /**
     * Applies an external update file to patch a game's ExeFS.
     * @param exefs The original ExeFS to be patched
     * @param base_nca The base NCA of the content being patched
     * @param external_update The external update file (NSP/XCI) containing the update
     * @return Patched ExeFS VirtualDir if successful, nullptr otherwise
     */
    VirtualDir PatchExeFSWithExternal(VirtualDir exefs, const NCA *base_nca, VirtualFile external_update) const;


    /**
     *  Extract an NCA of the specified type from an external update file.
     * @param update_file The external update file (NSP/XCI)
     * @param type The type of NCA to extract (Program, Control, etc.)
     * @param base_nca The base NCA to use as a template for the update NCA
     * @return Shared pointer to the extracted and templated NCA if successful, nullptr otherwise
     */
    std::shared_ptr<NCA> GetNCAfromExternalFile(VirtualFile update_file, ContentRecordType type,
                                                const NCA *base_nca) const;

    // Returns a vector of patches
    [[nodiscard]] std::vector<Patch> GetPatches(VirtualFile update_raw = nullptr) const;

    // If the game update exists, returns the u32 version field in its Meta-type NCA. If that fails,
    // it will fallback to the Meta-type NCA of the base game. If that fails, the result will be
    // std::nullopt
    [[nodiscard]] std::optional<u32> GetGameVersion() const;

    // Given title_id of the program, attempts to get the control data of the update and parse
    // it, falling back to the base control data.
    [[nodiscard]] Metadata GetControlMetadata() const;

    // Version of GetControlMetadata that takes an arbitrary NCA
    [[nodiscard]] Metadata ParseControlNCA(const NCA& nca) const;

private:
    [[nodiscard]] std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir>& patch_dirs,
                                                          const std::string& build_id) const;

    /**
     * Checks if a specific feature is disabled in user settings for this title.
     * @param feature_name The string name of the addon to check against
     * @param check_prefix If true, checks if the addon name starts with the given string else it's a literal check
     */
    bool IsAddOnDisabled(const std::string &feature_name, bool check_prefix) const;


    u64 title_id;
    const Service::FileSystem::FileSystemController& fs_controller;
    const ContentProvider& content_provider;
    std::shared_ptr<ExternalContentManager> external_manager;
};

} // namespace FileSys
