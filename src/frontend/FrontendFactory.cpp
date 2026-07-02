#include "frontend/FrontendFactory.h"

#include "frontend/LightweightFrontend.h"

#if defined(MODERNCPP_ENABLE_CLANG_EXPERIMENTS)
#include "frontend/ClangExperimentalFrontend.h"
#endif

#include <utility>

std::unique_ptr<IModernizationFrontend> createDefaultModernizationFrontend()
{
    return std::make_unique<LightweightFrontend>();
}

std::unique_ptr<IModernizationFrontend> createClangExperimentalFrontend()
{
#if defined(MODERNCPP_ENABLE_CLANG_EXPERIMENTS)
    return std::make_unique<ClangExperimentalFrontend>();
#else
    return nullptr;
#endif
}

std::unique_ptr<IModernizationFrontend> createClangExperimentalFrontend(ClangParseConfig config)
{
#if defined(MODERNCPP_ENABLE_CLANG_EXPERIMENTS)
    return std::make_unique<ClangExperimentalFrontend>(std::move(config));
#else
    (void)config;
    return nullptr;
#endif
}

bool clangExperimentsEnabled()
{
#if defined(MODERNCPP_ENABLE_CLANG_EXPERIMENTS)
    return true;
#else
    return false;
#endif
}
