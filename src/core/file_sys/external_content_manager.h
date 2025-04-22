// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>

#include "common/common_types.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {
class ContentProvider;
class NSP;
} // namespace FileSys

namespace FileSys {

class VfsFile;
class VfsFilesystem;

/**
 * @brief Struct that holds metadata for an external content file).
 */
struct ExternalFileMetadata {
    std::string name;
    std::string version;
};


/**
 * @brief  The type of external content.
 */
enum class ExternalContentType {
    Update,
    DLC
};

/**
 * @brief Manages external NSP addon files that are not installed to the virtual NAND.
 *
 */
class ExternalContentManager {
public:
    explicit ExternalContentManager();
    ~ExternalContentManager();

    /**
     * @brief Registers an external NSP file with the manager.
     * @param path The literal path to the NSP file.
     * @param program_id The program ID to associate with this file. If 0, the ID is read from the NSP.
     * @return true if registration was successful, false otherwise (e.g., file not found, parse error).
     */
    bool RequestRegisterExternalNSP(const std::string& path, u64 program_id = 0);


    /**
     * @brief Checks if an external update is registered for the given base game ID.
     * @param program_id The base program ID of the game.
     */
    bool HasExternalUpdate(u64 program_id) const;

    /**
     * @brief Checks if any external DLC is registered for the given base game ID.
     * @param program_id The base program ID of the game.
     */
    bool HasExternalDLC(u64 program_id) const;

    /**
     * @brief Gets the VFS file object for a registered external file for a given base game ID.
     * @param program_id The base program ID of the game.
     * @param content_type The type of external content (update or DLC).
     * @param first_only If true, only the first file is returned. If false, all files are returned.
     * @return A vector of shared pointers to VfsFile objects. Empty if no files found.
     */
    std::vector<std::shared_ptr<VfsFile>> GetExternalFiles(u64 program_id, ExternalContentType content_type,
                                                           bool first_only) const;

    /**
     * @brief Gets the VFS file object for the registered external update for a given base game ID.
     * @param program_id The base program ID of the game.
     * @return A shared pointer to the VfsFile if an update is found, nullptr otherwise.
     */
    std::shared_ptr<VfsFile> GetExternalUpdateFile(u64 program_id) const;

    /**
     * @brief Gets a list of VFS file objects for all registered external DLC for a given base game ID.
     * @param program_id The base program ID of the game.
     * @return A vector of shared pointers to VfsFile objects for the DLC. Empty if no DLC found.
     */
    std::vector<std::shared_ptr<VfsFile>> GetExternalDLCFiles(u64 program_id) const;

    /**
     * @brief Gets the extracted metadata for the registered external file for a given base game ID.
     * @param program_id The base program ID of the game.
     * @return An ExternalFileMetadata struct. Contains default values if no update or metadata is found.
     */
    ExternalFileMetadata GetExternalUpdateMetadata(u64 program_id) const;

    /**
     * @brief Get metadata for DLC files associated with the given base program ID.
     * @param program_id The base program ID of the game.
     * @return An ExternalFileMetadata struct. Contains default values if no DLC or metadata is found.
     */
    ExternalFileMetadata GetExternalDLCMetadata(u64 program_id) const;

    /**
     * @brief Extracts the program ID from an NSP file using multiple fallback methods.
     * @param nsp The NSP file to extract the program ID from.
     * @return The extracted program ID, or 0 if extraction failed.
     */
    static u64 ExtractProgramIDFromFile(const std::shared_ptr<NSP> &nsp);

    /**
     * @brief Loads the list of registered external NSP paths from a configuration file.
     * Clears existing state before loading. Validates paths and attempts re-registration.
     * @param config_path Path to the configuration file.
     */
    void LoadRegisteredPaths(const std::string& config_path);

    /**
     * @brief Saves the list of currently registered and existing external NSP paths to a configuration file.
     * @param config_path Path to the configuration file.
     */
    void SaveRegisteredPaths(const std::string& config_path) const;

    /**
     * @brief Gets the set of all currently registered external NSP file paths.
     * @return A copy of the set containing absolute paths.
     */
    std::set<std::string> GetAllRegisteredPaths() const;

    /**
   * @brief Gets the mapping of external DLC IDs to their base game IDs.
   * @return A const reference to the unordered map of external DLC IDs to base game IDs.
   */
    const std::unordered_map<u64, u64>& GetExternalDLCMapping() const {
        return dlc_to_base_map;
    }

    /**
     * @brief Gets the VFS file object for a registered external DLC by its title ID.
     * @param title_id The title ID of the DLC.
     */
    std::shared_ptr<VfsFile> GetExternalDLCFileByID(u64 title_id) const {
        std::lock_guard lock{mutex};
          auto it = external_files.find(title_id);
        if (it != external_files.end() && !it->second.empty()) {
            return it->second.front().second;
        }
          return nullptr;
    }

private:
    /**
     * @brief Internal, non-locking version of NSP registration logic.
     * Assumes caller holds the mutex.
     */
    bool RegisterExternalNSP(const std::string& path, u64 program_id);

    /**
     * @brief Parses a registered file to determine if it's an update or DLC and populates internal maps.
     * Also triggers metadata extraction and storage.
     */
    void ParseExternalFile(const std::string& path, u64 program_id);

    template <typename MapType>
    bool CheckMapForBaseID(const MapType& map, u64 program_id) const;

    std::shared_ptr<VfsFilesystem> vfs;
    std::set<std::string> registered_paths;
    std::unordered_map<u64, std::vector<std::pair<std::string, std::shared_ptr<VfsFile>>>> external_files;
    std::unordered_map<u64, u64> update_to_base_map;
    std::unordered_map<u64, u64> dlc_to_base_map;
    std::unordered_map<u64, ExternalFileMetadata> file_metadata;
    mutable std::mutex mutex;
};

/**
 * @brief Gets the instance of the ExternalContentManager.
 * @return A shared pointer to the ExternalContentManager instance.
 */
std::shared_ptr<ExternalContentManager> GetExternalContentManager();

} // namespace FileSys