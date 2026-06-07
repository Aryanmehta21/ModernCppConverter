#include "repository/RepositoryBackupService.h"

RepositoryBackupResult RepositoryBackupService::createBackup(const std::filesystem::path& filePath) const
{
    const std::filesystem::path backupPath = filePath.string() + ".legacy_backup";
    std::error_code error;
    if (std::filesystem::exists(backupPath)) {
        return {true, backupPath, "Backup already exists."};
    }

    std::filesystem::copy_file(filePath, backupPath, std::filesystem::copy_options::none, error);
    if (error) {
        return {false, backupPath, "Backup failed: " + error.message()};
    }
    return {true, backupPath, "Backup created."};
}
