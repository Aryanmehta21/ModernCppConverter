#include "repository/RepositoryScanner.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace
{
bool isIgnoredDirectory(const std::filesystem::path& path)
{
    static const std::set<std::string> ignored{
        ".git", "build", "out", "third_party", "external", "vendor", "node_modules", ".venv",
    };
    const std::string name = path.filename().string();
    return ignored.contains(name) || name.starts_with("cmake-build-");
}

bool isSupportedCppFile(const std::filesystem::path& path)
{
    static const std::set<std::string> extensions{
        ".cpp", ".cc", ".cxx", ".hpp", ".h", ".hh", ".hxx",
    };
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extensions.contains(extension);
}
} // namespace

std::vector<std::filesystem::path> RepositoryScanner::scanCppFiles(const std::filesystem::path& root) const
{
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(root)) {
        return files;
    }

    std::filesystem::recursive_directory_iterator iterator(root);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        if (entry.is_directory() && isIgnoredDirectory(entry.path())) {
            iterator.disable_recursion_pending();
        } else if (entry.is_regular_file() && isSupportedCppFile(entry.path())) {
            files.push_back(entry.path());
        }
        ++iterator;
    }
    return files;
}
