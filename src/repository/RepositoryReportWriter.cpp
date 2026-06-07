#include "repository/RepositoryReportWriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <fstream>
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
        fileObject["changes"] = changes;
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
