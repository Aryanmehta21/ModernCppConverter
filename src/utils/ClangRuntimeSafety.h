#pragma once

#include <string>

namespace ClangRuntimeSafety
{
[[nodiscard]] bool inProcessClangAllowed();
[[nodiscard]] std::string inProcessClangBlockReason();
} // namespace ClangRuntimeSafety
