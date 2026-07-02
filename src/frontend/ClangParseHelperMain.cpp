#include "frontend/ClangExperimentalFrontend.h"
#include "frontend/ClangParseService.h"

#include <QCoreApplication>

#include <iostream>
#include <string>

namespace
{
ClangParseConfig configFromArguments(const QStringList& arguments)
{
    ClangParseConfig config;
    config.compileArguments.clear();

    for (int index = 1; index < arguments.size(); ++index) {
        const QString option = arguments.at(index);
        auto takeValue = [&]() -> std::string {
            if (index + 1 >= arguments.size()) {
                return {};
            }
            ++index;
            return arguments.at(index).toStdString();
        };

        if (option == QStringLiteral("--standard")) {
            config.languageStandard = takeValue();
        } else if (option == QStringLiteral("--virtual-file")) {
            config.virtualFileName = takeValue();
        } else if (option == QStringLiteral("--resource-dir")) {
            config.resourceDir = takeValue();
        } else if (option == QStringLiteral("--system-root")) {
            config.systemRoot = takeValue();
        } else if (option == QStringLiteral("--include")) {
            config.includePaths.push_back(takeValue());
        } else if (option == QStringLiteral("--arg")) {
            config.compileArguments.push_back(takeValue());
        }
    }

    if (config.languageStandard.empty()) {
        config.languageStandard = "c++20";
    }
    if (config.virtualFileName.empty()) {
        config.virtualFileName = "input.cpp";
    }
    if (config.compileArguments.empty()) {
        config.compileArguments = {"-std=" + config.languageStandard, "-fsyntax-only", "-x", "c++"};
    }
    return config;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    const ClangParseConfig config = configFromArguments(application.arguments());
    const ClangExperimentalFrontend frontend(config);
    const ModernizationFrontendResult result = frontend.analyzeInProcess(source);
    std::cout << serializeClangFrontendResult(result);
    return result.parseSucceeded ? 0 : 2;
}
