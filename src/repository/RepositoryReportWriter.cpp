#include "repository/RepositoryReportWriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace
{
std::string levelName(OfflineModernizationLevel level)
{
    switch (level) {
    case OfflineModernizationLevel::Conservative:
        return "Conservative";
    case OfflineModernizationLevel::Balanced:
        return "Balanced";
    case OfflineModernizationLevel::AggressiveSafe:
        return "Aggressive Safe";
    case OfflineModernizationLevel::AiStyleAggressiveRewrite:
        return "AI-Style Aggressive Rewrite";
    }
    return "Balanced";
}

QJsonObject changeToJson(const ConversionChange& change)
{
    QJsonObject object;
    object["ruleName"] = QString::fromStdString(change.ruleName);
    object["before"] = QString::fromStdString(change.before);
    object["after"] = QString::fromStdString(change.after);
    object["reason"] = QString::fromStdString(change.reason);
    object["applied"] = change.applied;
    object["skipped"] = change.skipped;
    return object;
}

struct PassSummaryTotals
{
    std::size_t applied = 0;
    std::size_t skipped = 0;
    std::size_t warnings = 0;
    std::size_t rollbacks = 0;
    std::size_t occurrences = 0;
};

struct RollbackSummaryTotals
{
    std::size_t count = 0;
    std::size_t info = 0;
    std::size_t warnings = 0;
    std::size_t errors = 0;
};

struct SkippedRiskSummaryTotals
{
    std::size_t count = 0;
    std::size_t info = 0;
    std::size_t warnings = 0;
    std::size_t errors = 0;
};

std::optional<std::string> quotedValue(const std::string& text, const std::string& key)
{
    const std::string marker = key + "=\"";
    const std::size_t start = text.find(marker);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t valueEnd = text.find('"', valueStart);
    if (valueEnd == std::string::npos) {
        return std::nullopt;
    }
    return text.substr(valueStart, valueEnd - valueStart);
}

std::optional<std::string> tokenValue(const std::string& text, const std::string& key)
{
    if (const std::optional<std::string> quoted = quotedValue(text, key); quoted.has_value()) {
        return quoted;
    }
    const std::string marker = key + "=";
    const std::size_t start = text.find(marker);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + marker.size();
    std::size_t valueEnd = valueStart;
    while (valueEnd < text.size() && std::isspace(static_cast<unsigned char>(text[valueEnd])) == 0) {
        ++valueEnd;
    }
    if (valueEnd == valueStart) {
        return std::nullopt;
    }
    return text.substr(valueStart, valueEnd - valueStart);
}

std::size_t numericValue(const std::string& text, const std::string& key)
{
    const std::string marker = key + "=";
    const std::size_t start = text.find(marker);
    if (start == std::string::npos) {
        return 0;
    }
    const std::size_t valueStart = start + marker.size();
    std::size_t valueEnd = valueStart;
    while (valueEnd < text.size() && std::isdigit(static_cast<unsigned char>(text[valueEnd])) != 0) {
        ++valueEnd;
    }
    if (valueEnd == valueStart) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoull(text.substr(valueStart, valueEnd - valueStart)));
}

std::map<std::string, PassSummaryTotals> aggregatePassSummaries(const RepositoryModernizationResult& result)
{
    std::map<std::string, PassSummaryTotals> totals;
    for (const FileModernizationResult& file : result.files) {
        for (const std::string& diagnostic : file.diagnosticMessages) {
            if (diagnostic.find("PASS SUMMARY") == std::string::npos) {
                continue;
            }
            const std::optional<std::string> passName = quotedValue(diagnostic, "pass");
            if (!passName.has_value()) {
                continue;
            }
            PassSummaryTotals& passTotals = totals[*passName];
            passTotals.applied += numericValue(diagnostic, "applied");
            passTotals.skipped += numericValue(diagnostic, "skipped");
            passTotals.warnings += numericValue(diagnostic, "warnings");
            passTotals.rollbacks += numericValue(diagnostic, "rollbacks");
            ++passTotals.occurrences;
        }
    }
    return totals;
}

std::map<std::string, RollbackSummaryTotals> aggregateRollbackSummaries(const RepositoryModernizationResult& result)
{
    std::map<std::string, RollbackSummaryTotals> totals;
    for (const FileModernizationResult& file : result.files) {
        for (const std::string& diagnostic : file.diagnosticMessages) {
            if (diagnostic.find("ROLLBACK DETAIL") == std::string::npos) {
                continue;
            }
            const std::string category = tokenValue(diagnostic, "category").value_or("Semantic");
            const std::string severity = tokenValue(diagnostic, "severity").value_or("Warning");
            RollbackSummaryTotals& categoryTotals = totals[category];
            ++categoryTotals.count;
            if (severity == "Info") {
                ++categoryTotals.info;
            } else if (severity == "Error") {
                ++categoryTotals.errors;
            } else {
                ++categoryTotals.warnings;
            }
        }
    }
    return totals;
}

std::map<std::string, SkippedRiskSummaryTotals> aggregateSkippedRiskSummaries(const RepositoryModernizationResult& result)
{
    std::map<std::string, SkippedRiskSummaryTotals> totals;
    for (const FileModernizationResult& file : result.files) {
        for (const std::string& diagnostic : file.diagnosticMessages) {
            if (diagnostic.find("SKIPPED RISK") == std::string::npos
                || diagnostic.find("SKIPPED RISK SUMMARY") != std::string::npos) {
                continue;
            }
            const std::string category = tokenValue(diagnostic, "category").value_or("Semantic");
            const std::string severity = tokenValue(diagnostic, "severity").value_or("Warning");
            SkippedRiskSummaryTotals& categoryTotals = totals[category];
            ++categoryTotals.count;
            if (severity == "Info") {
                ++categoryTotals.info;
            } else if (severity == "Error") {
                ++categoryTotals.errors;
            } else {
                ++categoryTotals.warnings;
            }
        }
    }
    return totals;
}
} // namespace

bool RepositoryReportWriter::writeReports(RepositoryModernizationResult& result) const
{
    result.textReportPath = result.clonePath / "modernization_report.txt";
    result.jsonReportPath = result.clonePath / "modernization_report.json";

    std::ofstream text(result.textReportPath);
    if (!text.good()) {
        return false;
    }

    text << "Repository Modernization Report\n";
    text << "===============================\n\n";
    text << "Repository URL: " << result.repositoryUrl << "\n";
    text << "Branch: " << (result.branch.empty() ? "(default)" : result.branch) << "\n";
    text << "Clone path: " << result.clonePath.string() << "\n";
    text << "Timestamp: " << result.timestamp << "\n";
    text << "Modernization level: " << levelName(result.modernizationLevel) << "\n";
    text << "Files scanned: " << result.filesScanned << "\n";
    text << "Files modified: " << result.filesModified << "\n";
    text << "Files skipped: " << result.filesSkipped << "\n";
    text << "Total applied changes: " << result.totalAppliedChanges << "\n";
    text << "Total suggestions: " << result.totalSuggestions << "\n";
    text << "Total warnings: " << result.totalWarnings << "\n";
    text << "Verification: " << result.verificationSummary << "\n\n";

    const std::map<std::string, PassSummaryTotals> passSummaryTotals = aggregatePassSummaries(result);
    const std::map<std::string, SkippedRiskSummaryTotals> skippedRiskSummaryTotals = aggregateSkippedRiskSummaries(result);
    const std::map<std::string, RollbackSummaryTotals> rollbackSummaryTotals = aggregateRollbackSummaries(result);
    text << "Aggregate Pass Summary\n";
    text << "======================\n\n";
    if (passSummaryTotals.empty()) {
        text << "No pass diagnostics were recorded.\n\n";
    } else {
        for (const auto& [passName, totals] : passSummaryTotals) {
            text << "- " << passName
                 << ": applied=" << totals.applied
                 << " skipped=" << totals.skipped
                 << " warnings=" << totals.warnings
                 << " rollbacks=" << totals.rollbacks
                 << " files=" << totals.occurrences << "\n";
        }
        text << "\n";
    }

    text << "Aggregate Skipped Risk Summary\n";
    text << "==============================\n\n";
    if (skippedRiskSummaryTotals.empty()) {
        text << "No skipped risky transformation diagnostics were recorded.\n\n";
    } else {
        for (const auto& [category, totals] : skippedRiskSummaryTotals) {
            text << "- " << category
                 << ": skipped=" << totals.count
                 << " info=" << totals.info
                 << " warnings=" << totals.warnings
                 << " errors=" << totals.errors << "\n";
        }
        text << "\n";
    }

    text << "Aggregate Rollback Summary\n";
    text << "==========================\n\n";
    if (rollbackSummaryTotals.empty()) {
        text << "No rollback diagnostics were recorded.\n\n";
    } else {
        for (const auto& [category, totals] : rollbackSummaryTotals) {
            text << "- " << category
                 << ": rollbacks=" << totals.count
                 << " info=" << totals.info
                 << " warnings=" << totals.warnings
                 << " errors=" << totals.errors << "\n";
        }
        text << "\n";
    }

    for (const FileModernizationResult& file : result.files) {
        text << "File: " << file.filePath.string() << "\n";
        text << "Modified: " << (file.modified ? "true" : "false") << "\n";
        text << "Skipped: " << (file.skipped ? "true" : "false") << "\n";
        if (!file.warning.empty()) {
            text << "Warning: " << file.warning << "\n";
        }
        text << "Compile verification: ";
        if (!file.compileVerificationEnabled) {
            text << "not run\n";
        } else {
            text << (file.compileVerificationPassed ? "passed" : "failed/skipped") << "\n";
            text << "Compiler: " << (file.compilerUsed.empty() ? "not found" : file.compilerUsed) << "\n";
            text << "Compiler output: " << file.compilerOutput << "\n";
        }
        if (!file.diagnosticMessages.empty()) {
            text << "Diagnostics:\n";
            for (const std::string& diagnostic : file.diagnosticMessages) {
                text << "  - " << diagnostic << "\n";
            }
        }
        for (const ConversionChange& change : file.changes) {
            text << "\n  Rule: " << change.ruleName << "\n";
            text << "  Before: " << change.before << "\n";
            text << "  After: " << change.after << "\n";
            text << "  Reason: " << change.reason << "\n";
            text << "  Applied: " << (change.applied ? "true" : "false") << "\n";
        }
        text << "\n---\n\n";
    }

    QJsonObject root;
    root["repositoryUrl"] = QString::fromStdString(result.repositoryUrl);
    root["branch"] = QString::fromStdString(result.branch);
    root["clonePath"] = QString::fromStdString(result.clonePath.string());
    root["timestamp"] = QString::fromStdString(result.timestamp);
    root["modernizationLevel"] = QString::fromStdString(levelName(result.modernizationLevel));
    root["filesScanned"] = static_cast<qint64>(result.filesScanned);
    root["filesModified"] = static_cast<qint64>(result.filesModified);
    root["filesSkipped"] = static_cast<qint64>(result.filesSkipped);
    root["totalAppliedChanges"] = static_cast<qint64>(result.totalAppliedChanges);
    root["totalSuggestions"] = static_cast<qint64>(result.totalSuggestions);
    root["totalWarnings"] = static_cast<qint64>(result.totalWarnings);
    root["verificationSummary"] = QString::fromStdString(result.verificationSummary);

    QJsonArray aggregatePassSummariesJson;
    for (const auto& [passName, totals] : passSummaryTotals) {
        QJsonObject passObject;
        passObject["passName"] = QString::fromStdString(passName);
        passObject["applied"] = static_cast<qint64>(totals.applied);
        passObject["skipped"] = static_cast<qint64>(totals.skipped);
        passObject["warnings"] = static_cast<qint64>(totals.warnings);
        passObject["rollbacks"] = static_cast<qint64>(totals.rollbacks);
        passObject["files"] = static_cast<qint64>(totals.occurrences);
        aggregatePassSummariesJson.append(passObject);
    }
    root["aggregatePassSummary"] = aggregatePassSummariesJson;

    QJsonArray aggregateSkippedRiskSummariesJson;
    for (const auto& [category, totals] : skippedRiskSummaryTotals) {
        QJsonObject skippedRiskObject;
        skippedRiskObject["category"] = QString::fromStdString(category);
        skippedRiskObject["skipped"] = static_cast<qint64>(totals.count);
        skippedRiskObject["info"] = static_cast<qint64>(totals.info);
        skippedRiskObject["warnings"] = static_cast<qint64>(totals.warnings);
        skippedRiskObject["errors"] = static_cast<qint64>(totals.errors);
        aggregateSkippedRiskSummariesJson.append(skippedRiskObject);
    }
    root["aggregateSkippedRiskSummary"] = aggregateSkippedRiskSummariesJson;

    QJsonArray aggregateRollbackSummariesJson;
    for (const auto& [category, totals] : rollbackSummaryTotals) {
        QJsonObject rollbackObject;
        rollbackObject["category"] = QString::fromStdString(category);
        rollbackObject["rollbacks"] = static_cast<qint64>(totals.count);
        rollbackObject["info"] = static_cast<qint64>(totals.info);
        rollbackObject["warnings"] = static_cast<qint64>(totals.warnings);
        rollbackObject["errors"] = static_cast<qint64>(totals.errors);
        aggregateRollbackSummariesJson.append(rollbackObject);
    }
    root["aggregateRollbackSummary"] = aggregateRollbackSummariesJson;

    QJsonArray files;
    for (const FileModernizationResult& file : result.files) {
        QJsonObject fileObject;
        fileObject["filePath"] = QString::fromStdString(file.filePath.string());
        fileObject["modified"] = file.modified;
        fileObject["skipped"] = file.skipped;
        fileObject["warning"] = QString::fromStdString(file.warning);
        fileObject["compileVerificationEnabled"] = file.compileVerificationEnabled;
        fileObject["compileVerificationPassed"] = file.compileVerificationPassed;
        fileObject["compilerUsed"] = QString::fromStdString(file.compilerUsed);
        fileObject["compilerOutput"] = QString::fromStdString(file.compilerOutput);
        QJsonArray changes;
        for (const ConversionChange& change : file.changes) {
            changes.append(changeToJson(change));
        }
        QJsonArray diagnostics;
        for (const std::string& diagnostic : file.diagnosticMessages) {
            diagnostics.append(QString::fromStdString(diagnostic));
        }
        fileObject["changes"] = changes;
        fileObject["diagnostics"] = diagnostics;
        files.append(fileObject);
    }
    root["files"] = files;

    std::ofstream json(result.jsonReportPath);
    if (!json.good()) {
        return false;
    }
    json << QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
    return true;
}
