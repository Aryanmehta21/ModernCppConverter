#include "repository/RepositoryModernizationService.h"

#include <QDateTime>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return false;
    }
    output << text;
    return output.good();
}
} // namespace

RepositoryModernizationService::RepositoryModernizationService(std::unique_ptr<IConverterEngine> converterEngine)
    : converterEngine_(std::move(converterEngine))
{
    if (!converterEngine_) {
        throw std::invalid_argument("RepositoryModernizationService requires a converter engine.");
    }
}

std::vector<std::filesystem::path> RepositoryModernizationService::scanFiles(const std::filesystem::path& clonePath) const
{
    return scanner_.scanCppFiles(clonePath);
}

RepositoryModernizationResult RepositoryModernizationService::modernizeRepository(const RepositoryModernizationOptions& options,
                                                                                  const std::filesystem::path& clonePath) const
{
    RepositoryModernizationResult result;
    result.repositoryUrl = options.repositoryUrl;
    result.branch = options.branch;
    result.clonePath = clonePath;
    result.timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss").toStdString();
    result.modernizationLevel = options.modernizationLevel;

    const std::vector<std::filesystem::path> files = scanner_.scanCppFiles(clonePath);
    result.filesScanned = files.size();
    const ModernizationOptions conversionOptions = fileOptions(options);

    for (const std::filesystem::path& filePath : files) {
        FileModernizationResult fileResult;
        fileResult.filePath = filePath;

        const std::string original = readFile(filePath);
        if (original.empty()) {
            fileResult.skipped = true;
            fileResult.warning = "File is empty or could not be read.";
            ++result.filesSkipped;
            ++result.totalWarnings;
            result.files.push_back(std::move(fileResult));
            continue;
        }

        ConversionResult conversion = converterEngine_->convert(original, conversionOptions);
        fileResult.changes = conversion.changes;

        for (const ConversionChange& change : fileResult.changes) {
            if (change.applied) {
                ++result.totalAppliedChanges;
            } else if (!change.skipped) {
                ++result.totalSuggestions;
            }
        }

        if (conversion.modernCode != original) {
            const RepositoryBackupResult backup = backupService_.createBackup(filePath);
            if (!backup.success) {
                fileResult.skipped = true;
                fileResult.warning = backup.message;
                ++result.filesSkipped;
                ++result.totalWarnings;
                result.files.push_back(std::move(fileResult));
                continue;
            }
            if (writeFile(filePath, conversion.modernCode)) {
                fileResult.modified = true;
                ++result.filesModified;
            } else {
                fileResult.skipped = true;
                fileResult.warning = "Could not write modernized file.";
                ++result.filesSkipped;
                ++result.totalWarnings;
            }
        }

        if (options.compileVerificationEnabled) {
            verificationService_.verifyFile(fileResult, conversion.modernCode);
            if (fileResult.compileVerificationEnabled && !fileResult.compileVerificationPassed) {
                ++result.totalWarnings;
            }
        }

        result.files.push_back(std::move(fileResult));
    }

    result.verificationSummary = verificationService_.summarize(result);
    if (!reportWriter_.writeReports(result)) {
        result.verificationSummary += " Report generation failed.";
        ++result.totalWarnings;
    }
    return result;
}

ModernizationOptions RepositoryModernizationService::fileOptions(const RepositoryModernizationOptions& options)
{
    ModernizationOptions modernOptions;
    modernOptions.offlineModernizationLevel = options.modernizationLevel;
    modernOptions.offlineRewriteStyle = options.modernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite
        ? OfflineRewriteStyle::AggressiveAiLikeRewrite
        : OfflineRewriteStyle::SafeModernization;
    modernOptions.compileVerificationEnabled = false;
    modernOptions.useAuto = options.modernizationLevel != OfflineModernizationLevel::Conservative;
    modernOptions.useLambdas = options.modernizationLevel != OfflineModernizationLevel::Conservative;
    modernOptions.useConstexpr = options.modernizationLevel != OfflineModernizationLevel::Conservative;
    modernOptions.useStructuredBindings = options.modernizationLevel == OfflineModernizationLevel::AggressiveSafe
        || options.modernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
    modernOptions.useRanges = options.modernizationLevel == OfflineModernizationLevel::AiStyleAggressiveRewrite;
    modernOptions.applySafeOwnershipModernization = true;
    modernOptions.useRangeBasedFor = true;
    modernOptions.useSmartPointers = true;
    modernOptions.useMakeUnique = true;
    return modernOptions;
}
