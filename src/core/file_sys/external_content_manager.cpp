// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <set>
#include <regex>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/external_content_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "core/file_sys/common_funcs.h"

namespace FileSys {

ExternalContentManager::ExternalContentManager() {
    vfs = std::make_shared<RealVfsFilesystem>();
}

ExternalContentManager::~ExternalContentManager() = default;

bool ExternalContentManager::RegisterExternalNSP(const std::string& path, u64 program_id) {
    if (registered_paths.count(path)) {
        return true;
    }

    if (path.empty()) {
        LOG_ERROR(Loader, "Empty path provided to RegisterExternalNSPInternal");
        return false;
    }

    if (!Common::FS::Exists(path)) {
        LOG_ERROR(Loader, "File does not exist: {}", path);
        return false;
    }

    auto file = vfs->OpenFile(path, FileSys::OpenMode::Read);
    if (file == nullptr) {
        LOG_ERROR(Loader, "Failed to open NSP file with VFS: {}", path);
        return false;
    }

    std::shared_ptr<NSP> nsp;
    try {
        nsp = std::make_shared<NSP>(file);
        if (nsp->GetStatus() != Loader::ResultStatus::Success) {
            LOG_ERROR(Loader, "Failed to parse NSP file: {} (status: {})",
                      path, static_cast<int>(nsp->GetStatus()));
            return false;
        }
    } catch (const std::exception& e) {
        LOG_CRITICAL(Loader, "Exception when parsing NSP file {}: {}", path, e.what());
        return false;
    }

    u64 target_program_id = program_id;
    if (target_program_id == 0) {
        try {
            target_program_id = ExtractProgramIDFromFile(nsp);
        } catch (const std::exception& e) {
            LOG_ERROR(Loader, "Exception when getting program ID from NSP {}: {}", path, e.what());
            return false;
        }
    }

    external_files[target_program_id].emplace_back(path, file);
    registered_paths.insert(path);

    ParseExternalFile(path, target_program_id);
    return true;
}

bool ExternalContentManager::RequestRegisterExternalNSP(const std::string& path, u64 program_id) {
    std::lock_guard lock{mutex};
    return RegisterExternalNSP(path, program_id);
}

void ExternalContentManager::LoadRegisteredPaths(const std::string& config_path) {
    std::lock_guard lock{mutex};
    registered_paths.clear();
    external_files.clear();
    update_to_base_map.clear();
    dlc_to_base_map.clear();
    file_metadata.clear();

    std::ifstream file(config_path);
    if (!file.is_open()) {
        LOG_WARNING(Service_FS, "Could not open external files config for loading: {}", config_path);
        return;
    }

    std::string path;
    std::set<std::string> valid_paths_from_config;

    while (std::getline(file, path)) {
        if (path.empty()) continue;
        if (!Common::FS::Exists(path)) {
            LOG_WARNING(Loader, "External file path from config not found, skipping: {}", path);
            continue;
        }

        valid_paths_from_config.insert(path);

        const auto extension = Common::ToLower(std::filesystem::path(path).extension().string());
        bool success = false;
        if (extension == ".nsp") {
            success = RegisterExternalNSP(path, 0);
        }

        if (!success) {
            LOG_ERROR(Loader, "Failed to re-register external NSP from config: {}", path);
            valid_paths_from_config.erase(path);
        }
    }

    file.close();
    registered_paths = std::move(valid_paths_from_config);
}

void ExternalContentManager::SaveRegisteredPaths(const std::string& config_path) const {
    std::lock_guard lock{mutex};

    std::ofstream file(config_path);
    if (!file.is_open()) {
        LOG_ERROR(Frontend, "Could not open external files config for saving: {}", config_path);
        return;
    }

    for (const auto& path : registered_paths) {
        if (Common::FS::Exists(path)) {
             const auto extension = Common::ToLower(std::filesystem::path(path).extension().string());
             if (extension == ".nsp") { // Only save NSP paths
                file << path << std::endl;
            }
        }
    }

    file.close();
}

std::set<std::string> ExternalContentManager::GetAllRegisteredPaths() const {
     std::lock_guard lock{mutex};
     return registered_paths;
}

/**
 * @brief Checks if a given base program ID exists as a value in the map.
 * @tparam MapType The type of the map (e.g., std::unordered_map<u64, u64>)
 * @param map The map to search (e.g., update_to_base_map or dlc_to_base_map)
 * @param program_id The base program ID to look for as a value in the map
 * @return true if the program_id is found as a base ID in the map, false otherwise.
 */
template <typename MapType>
bool ExternalContentManager::CheckMapForBaseID(const MapType& map, u64 program_id) const {
    for (const auto& [content_id, base_id] : map) {
        if (base_id == program_id) {
            return true;
        }
    }
    return false;
}

bool ExternalContentManager::HasExternalUpdate(u64 program_id) const {
    std::lock_guard lock{mutex};
    return CheckMapForBaseID(update_to_base_map, program_id);
}

bool ExternalContentManager::HasExternalDLC(u64 program_id) const {
    std::lock_guard lock{mutex};
    const bool has_dlc = CheckMapForBaseID(dlc_to_base_map, program_id);
    return has_dlc;
}

std::vector<std::shared_ptr<VfsFile>> ExternalContentManager::GetExternalFiles(
    u64 program_id, ExternalContentType content_type, bool first_only = false) const {
    std::lock_guard lock{mutex};

    std::vector<std::shared_ptr<VfsFile>> result;

    const auto& map = (content_type == ExternalContentType::Update) ?
                       update_to_base_map : dlc_to_base_map;

    for (const auto& [content_id, base_id] : map) {
        if (base_id == program_id) {
            auto it = external_files.find(content_id);
            if (it != external_files.end() && !it->second.empty()) {
                if (first_only) {
                    // Update
                    result.push_back(it->second.front().second);
                    break;
                }
                // DLC
                for (const auto& [path, file] : it->second) {
                    result.push_back(file);
                }
            }
        }
    }

    return result;
}

std::shared_ptr<VfsFile> ExternalContentManager::GetExternalUpdateFile(u64 program_id) const {
    auto files = GetExternalFiles(program_id, ExternalContentType::Update, true);
    return files.empty() ? nullptr : files.front();
}

std::vector<std::shared_ptr<VfsFile>> ExternalContentManager::GetExternalDLCFiles(u64 program_id) const {
    return GetExternalFiles(program_id, ExternalContentType::DLC, false);
}

void ExternalContentManager::ParseExternalFile(const std::string& path, u64 program_id) {
    ExternalFileMetadata metadata;
    std::string filename = std::filesystem::path(path).filename().string();

    const auto extension = std::filesystem::path(path).extension().string();

    // Should never happen because non nsp files are blocked by the frontend but just in case
    if (Common::ToLower(extension) != ".nsp") {
        LOG_CRITICAL(Loader, "Only NSP files are supported: {}", path);
        return;
    }

    auto file = vfs->OpenFile(path, FileSys::OpenMode::Read);
    if (file == nullptr) {
        LOG_ERROR(Loader, "Failed to open file for parsing: {}", path);
        return;
    }

    const auto program_id_low = program_id & 0xFFFFFFFF;
    const bool is_update = (program_id_low & 0x800) != 0 && (program_id_low & 0xFFF) < 0x1000;
    const bool is_dlc = (program_id_low & 0xF000) != 0 || (GetBaseTitleID(program_id) != program_id);

    metadata.name = is_update ?
        fmt::format("Update (File): {}", filename) :
        (is_dlc ? fmt::format("DLC (File): {}", filename) : filename);

    try {
        auto nsp = std::make_shared<FileSys::NSP>(file);
        if (nsp->GetStatus() != Loader::ResultStatus::Success) {
            LOG_WARNING(Loader, "Could not parse NSP for metadata: {}", path);
            file_metadata[program_id] = metadata;
            return;
        }

        const TitleType title_type = is_update ? TitleType::Update :
                                   (is_dlc ? TitleType::AOC : TitleType::Application);

        // For updates, we need program_id but for DLC we need the actual program_id (not OR'd with 0x800)
        const u64 control_id = is_update ? program_id :
                              (is_dlc ? program_id : (program_id | 0x800));

        auto control = nsp->GetNCA(control_id, ContentRecordType::Control, title_type);

        if (control && control->GetStatus() == Loader::ResultStatus::Success) {
            // Extract metadata from control NCA (Needed for version string)
            if (auto romfs = control->GetRomFS()) {
                if (auto extracted = ExtractRomFS(romfs)) {
                    auto nacp_file = extracted->GetFile("control.nacp");
                    if (!nacp_file) {
                        nacp_file = extracted->GetFile("Control.nacp");
                    }

                    if (nacp_file) {
                        NACP nacp(nacp_file);
                        metadata.version = nacp.GetVersionString();
                    }

                }
            }
        }

        if (is_update) {
            const u64 base_program_id = program_id & ~0x800ULL;
            update_to_base_map[program_id] = base_program_id;
            LOG_INFO(Loader, "Identified external NSP as update: {} ({}) for base game {:016X}",
                    path, metadata.version, base_program_id);
        } else if (is_dlc) {
            const u64 possible_base_id = GetBaseTitleID(program_id);
            dlc_to_base_map[program_id] = possible_base_id;
            LOG_INFO(Loader, "Identified external NSP as DLC: {} ({}) for base game {:016X}",
                    path, metadata.version, possible_base_id);
        } else {
            LOG_WARNING(Loader, "External NSP {} does not match update or DLC criteria", path);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Loader, "Error processing external file: {}", e.what());
    }

    file_metadata[program_id] = metadata;
}

ExternalFileMetadata ExternalContentManager::GetExternalUpdateMetadata(u64 program_id) const {
    std::lock_guard lock{mutex};

    for (const auto& [update_id, base_id] : update_to_base_map) {
        if (base_id == program_id) {
            auto it = file_metadata.find(update_id);
            if (it != file_metadata.end()) {
                return it->second;
            }
        }
    }

    // Placeholder empty metadata
    ExternalFileMetadata empty_metadata;
    empty_metadata.name = "";
    empty_metadata.version = "";
    return empty_metadata;
}

ExternalFileMetadata ExternalContentManager::GetExternalDLCMetadata(u64 program_id) const {
    std::lock_guard lock{mutex};
    ExternalFileMetadata metadata;
    metadata.name = "DLC (File)";

    std::string version_list;
    bool first = true;

    for (const auto& [dlc_id, base_id] : dlc_to_base_map) {
        if (base_id == program_id) {
            if (!first) {
                version_list += ", ";
            }

            version_list += fmt::format("{}", dlc_id & 0x7FF);
            first = false;

            auto it = file_metadata.find(dlc_id);
            if (it != file_metadata.end() && !metadata.name.empty()) {
                metadata.name = it->second.name;
            }
        }
    }

    metadata.version = version_list.empty() ? "Unknown" : version_list;
    return metadata;
}

u64 ExternalContentManager::ExtractProgramIDFromFile(const std::shared_ptr<NSP>& nsp) {
    u64 id = nsp->GetProgramTitleID();
    if (id != 0) return id;

    LOG_WARNING(Loader, "NSP returned zero program ID, attempting fallback methods");

    // Check all title IDs
    const auto all_ids = nsp->GetProgramTitleIDs();
    for (const auto& title_id : all_ids) {
        if (title_id != 0) {
            LOG_INFO(Loader, "Found ID from title IDs list: {:016X}", title_id);
            return title_id;
        }
    }

    // If all else fails, check the NCA files
    const auto ncas = nsp->GetNCAsCollapsed();
    for (const auto& nca : ncas) {
        const auto nca_id = nca->GetTitleId();
        if (nca_id != 0) {
            LOG_INFO(Loader, "Found ID from NCA: {:016X}", nca_id);
            return nca_id;
        }
    }

    LOG_CRITICAL(Loader, "All ID detection methods failed, defaulting to 0");
    return 0;
}

std::shared_ptr<ExternalContentManager> GetExternalContentManager() {
    static std::shared_ptr<ExternalContentManager> instance =
        std::make_shared<ExternalContentManager>();
    return instance;
}

} // namespace FileSys
