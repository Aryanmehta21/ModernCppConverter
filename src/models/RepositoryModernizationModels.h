#pragma once

#include "models/ConversionChange.h"
#include "models/ModernizationOptions.h"

#include <filesystem>
#include <string>
#include <vector>

struct RepositoryModernizationOptions
{
    std::string repositoryUrl;
    std::string branch;
    std::filesystem::path outputWorkspaceFolder;
    OfflineModernizationLevel modernizationLevel = OfflineModernizationLevel::Balanced;
    bool allowOverwrite = false;
    bool compileVerificationEnabled = true;
};

struct FileModernizationResult
{
    std::filesystem::path filePath;
    bool modified = false;
    bool skipped = false;
    std::string warning;
    std::vector<ConversionChange> changes;
    bool compileVerificationEnabled = false;
    bool compileVerificationPassed = false;
    std::string compilerUsed;
    std::string compilerOutput;
    std::vector<std::string> diagnosticMessages;
};

struct RepositoryModernizationResult
{
    std::string repositoryUrl;
    std::string branch;
    std::filesystem::path clonePath;
    std::filesystem::path textReportPath;
    std::filesystem::path jsonReportPath;
    std::string timestamp;
    OfflineModernizationLevel modernizationLevel = OfflineModernizationLevel::Balanced;
    std::size_t filesScanned = 0;
    std::size_t filesModified = 0;
    std::size_t filesSkipped = 0;
    std::size_t totalAppliedChanges = 0;
    std::size_t totalSuggestions = 0;
    std::size_t totalWarnings = 0;
    std::vector<FileModernizationResult> files;
    std::string verificationSummary;
};

using RepositoryChangeReport = RepositoryModernizationResult;
