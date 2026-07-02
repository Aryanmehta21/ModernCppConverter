#pragma once

#include "parser/ParsedEntity.h"

#include <cstddef>
#include <string>
#include <vector>

struct ClangParseConfig
{
    std::string languageStandard = "c++20";
    std::string virtualFileName = "input.cpp";
    std::string systemRoot;
    std::string resourceDir;
    std::vector<std::string> includePaths;
    std::vector<std::string> compileArguments = {"-std=c++20", "-fsyntax-only", "-x", "c++"};
};

enum class ModernizationFrontendKind
{
    Lightweight,
    ClangExperimental,
};

struct FrontendEntityCounts
{
    std::size_t classes = 0;
    std::size_t functions = 0;
    std::size_t enums = 0;
    std::size_t variables = 0;
};

struct ModernizationFrontendResult
{
    ModernizationFrontendKind kind = ModernizationFrontendKind::Lightweight;
    std::string frontendName;
    ParsedDocument document;
    FrontendEntityCounts entityCounts;
    std::vector<std::string> diagnostics;
    bool parseSucceeded = false;
};

class IModernizationFrontend
{
public:
    virtual ~IModernizationFrontend() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual ModernizationFrontendKind kind() const = 0;
    [[nodiscard]] virtual bool isExperimental() const = 0;
    [[nodiscard]] virtual ModernizationFrontendResult analyze(const std::string& source) const = 0;
};
