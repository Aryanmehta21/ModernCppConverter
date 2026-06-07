#include "repository/RepositoryCloneService.h"

#include <QProcess>
#include <QStandardPaths>

#include <regex>

namespace
{
std::string repositoryFolderName(const std::string& url)
{
    std::string name = url.substr(url.find_last_of('/') + 1);
    if (name.ends_with(".git")) {
        name.erase(name.size() - 4);
    }
    return name.empty() ? "repository" : name;
}
} // namespace

bool RepositoryCloneService::isValidGitHubUrl(const std::string& url)
{
    static const std::regex pattern(R"(^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:\.git)?/?$)");
    return std::regex_match(url, pattern);
}

bool RepositoryCloneService::isSafeBranchName(const std::string& branch)
{
    static const std::regex pattern(R"(^[A-Za-z0-9._/\-]+$)");
    return branch.empty() || (branch.size() <= 200 && std::regex_match(branch, pattern));
}

RepositoryCloneResult RepositoryCloneService::cloneRepository(const RepositoryModernizationOptions& options) const
{
    if (!isValidGitHubUrl(options.repositoryUrl)) {
        return {false, {}, "Only public https://github.com/<owner>/<repo> URLs are supported."};
    }
    if (!isSafeBranchName(options.branch)) {
        return {false, {}, "Branch name contains unsupported characters."};
    }

    const QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty()) {
        return {false, {}, "git executable was not found."};
    }

    std::error_code error;
    std::filesystem::create_directories(options.outputWorkspaceFolder, error);
    if (error) {
        return {false, {}, "Could not create output workspace folder: " + error.message()};
    }

    const std::filesystem::path target = options.outputWorkspaceFolder / repositoryFolderName(options.repositoryUrl);
    if (std::filesystem::exists(target) && !options.allowOverwrite) {
        return {false, target, "Target clone folder already exists."};
    }
    if (std::filesystem::exists(target) && options.allowOverwrite) {
        if (std::filesystem::exists(target / ".git")) {
            return {true, target, "Using existing cloned repository folder. No clone overwrite was performed."};
        }
        return {false, target, "Target exists and is not a git repository. Choose a different workspace folder."};
    }

    QStringList arguments;
    arguments << "clone";
    if (!options.branch.empty()) {
        arguments << "--branch" << QString::fromStdString(options.branch);
    }
    arguments << QString::fromStdString(options.repositoryUrl);
    arguments << QString::fromStdString(target.string());

    QProcess process;
    process.start(git, arguments);
    if (!process.waitForStarted(5000)) {
        return {false, target, "git clone could not be started."};
    }
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished();
        return {false, target, "git clone timed out."};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const std::string output = (QString::fromUtf8(process.readAllStandardError()) + QString::fromUtf8(process.readAllStandardOutput())).trimmed().toStdString();
        return {false, target, output.empty() ? "git clone failed." : output};
    }

    return {true, target, "Repository cloned successfully."};
}
