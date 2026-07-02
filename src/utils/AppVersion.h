#pragma once

#include <string>

namespace AppVersion
{
std::string version();
std::string releaseChannel();
std::string buildType();
std::string gitCommit();
std::string buildDate();
bool clangEnabled();
std::string diagnosticLine();
std::string startupLogLine();
std::string windowTitle();
} // namespace AppVersion
