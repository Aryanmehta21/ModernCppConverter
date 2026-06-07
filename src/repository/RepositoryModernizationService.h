#pragma once

#include "converter/IConverterEngine.h"
#include "models/RepositoryModernizationModels.h"
#include "repository/RepositoryBackupService.h"
#include "repository/RepositoryReportWriter.h"
#include "repository/RepositoryScanner.h"
#include "repository/RepositoryVerificationService.h"

#include <memory>
#include <vector>

class RepositoryModernizationService
{
public:
    explicit RepositoryModernizationService(std::unique_ptr<IConverterEngine> converterEngine);

    [[nodiscard]] std::vector<std::filesystem::path> scanFiles(const std::filesystem::path& clonePath) const;
    [[nodiscard]] RepositoryModernizationResult modernizeRepository(const RepositoryModernizationOptions& options,
                                                                    const std::filesystem::path& clonePath) const;

private:
    std::unique_ptr<IConverterEngine> converterEngine_;
    RepositoryScanner scanner_;
    RepositoryBackupService backupService_;
    RepositoryVerificationService verificationService_;
    RepositoryReportWriter reportWriter_;

    [[nodiscard]] static ModernizationOptions fileOptions(const RepositoryModernizationOptions& options);
};
