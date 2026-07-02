#include "utils/AppVersion.h"

#include <sstream>

#ifndef MODERNCPP_APP_VERSION
#define MODERNCPP_APP_VERSION "unknown"
#endif

#ifndef MODERNCPP_RELEASE_CHANNEL
#define MODERNCPP_RELEASE_CHANNEL "unknown"
#endif

#ifndef MODERNCPP_BUILD_TYPE
#define MODERNCPP_BUILD_TYPE "Unknown"
#endif

#ifndef MODERNCPP_GIT_COMMIT
#define MODERNCPP_GIT_COMMIT "unknown"
#endif

#ifndef MODERNCPP_BUILD_DATE
#define MODERNCPP_BUILD_DATE "unknown"
#endif

#ifndef MODERNCPP_CLANG_ENABLED
#define MODERNCPP_CLANG_ENABLED 0
#endif

namespace AppVersion
{
std::string version()
{
    return MODERNCPP_APP_VERSION;
}

std::string releaseChannel()
{
    return MODERNCPP_RELEASE_CHANNEL;
}

std::string buildType()
{
    return MODERNCPP_BUILD_TYPE;
}

std::string gitCommit()
{
    return MODERNCPP_GIT_COMMIT;
}

std::string buildDate()
{
    return MODERNCPP_BUILD_DATE;
}

bool clangEnabled()
{
    return MODERNCPP_CLANG_ENABLED != 0;
}

std::string diagnosticLine()
{
    std::ostringstream output;
    output << "ModernCppConverter version=" << version()
           << " channel=" << releaseChannel()
           << " build_type=" << buildType()
           << " clang_enabled=" << (clangEnabled() ? "true" : "false")
           << " git_commit=" << gitCommit()
           << " build_date=" << buildDate();
    return output.str();
}

std::string startupLogLine()
{
    return "STARTUP " + diagnosticLine();
}

std::string windowTitle()
{
    return "Modern C++ Converter " + version();
}
} // namespace AppVersion
