#pragma once

#include "frontend/IModernizationFrontend.h"

#include <memory>

[[nodiscard]] std::unique_ptr<IModernizationFrontend> createDefaultModernizationFrontend();
[[nodiscard]] std::unique_ptr<IModernizationFrontend> createClangExperimentalFrontend();
[[nodiscard]] std::unique_ptr<IModernizationFrontend> createClangExperimentalFrontend(ClangParseConfig config);
[[nodiscard]] bool clangExperimentsEnabled();
