#include "converter/RuleBasedConverterEngine.h"
#include "utils/AppVersion.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct SmokeCase
{
    std::string name;
    std::string input;
    std::vector<std::string> requiredFragments;
    std::vector<std::string> forbiddenFragments;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

ModernizationOptions smokeOptions()
{
    ModernizationOptions options;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;
    options.targetStandard = CppStandard::Cpp20;
    options.useConstexpr = true;
    options.useEnumClass = true;
    options.useRangeBasedFor = true;
    options.useStructuredBindings = true;
    options.useSmartPointers = true;
    options.useMakeUnique = true;
    options.applySafeOwnershipModernization = true;
    options.compileVerificationEnabled = true;
    return options;
}

void requireCompilePass(const ConversionResult& result, const std::string& caseName)
{
    require(result.compileVerificationEnabled, caseName + " should request compile verification");
    require(!result.compilerUsed.empty(), caseName + " should find a compiler for smoke verification");
    require(result.compileVerificationPassed,
            caseName + " should pass syntax-only compile verification\nCompiler output:\n"
                + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

std::vector<SmokeCase> smokeCases()
{
    return {
        {
            "typedef to using",
            "typedef int Count;\n"
            "Count answer()\n"
            "{\n"
            "    return 42;\n"
            "}\n",
            {"using Count = int;", "Count answer()"},
            {"typedef int Count;"},
        },
        {
            "raw pointer to smart pointer",
            "class Device\n"
            "{\n"
            "public:\n"
            "    void ping() {}\n"
            "};\n"
            "void inspect(Device* device)\n"
            "{\n"
            "    if (device != nullptr) { device->ping(); }\n"
            "}\n"
            "void run()\n"
            "{\n"
            "    Device* device = new Device();\n"
            "    inspect(device);\n"
            "    delete device;\n"
            "}\n",
            {"std::make_unique<Device>()", "inspect(device.get())"},
            {"delete device", "inspect(device);"},
        },
        {
            "char array to std::string",
            "#include <cstring>\n"
            "void copyName(const char* input)\n"
            "{\n"
            "    char name[50];\n"
            "    std::strncpy(name, input, sizeof(name));\n"
            "}\n",
            {"#include <string>", "std::string name = input;"},
            {"std::strncpy", "char name[50]"},
        },
        {
            "enum class propagation",
            "#include <iostream>\n"
            "enum Status { Ready, Busy };\n"
            "Status current()\n"
            "{\n"
            "    return Ready;\n"
            "}\n"
            "void inspect(Status status)\n"
            "{\n"
            "    if (status == Busy) { status = Ready; }\n"
            "    std::cout << status << '\\n';\n"
            "}\n",
            {"enum class Status", "return Status::Ready;", "status == Status::Busy", "static_cast<std::underlying_type_t<Status>>(status)"},
            {"return Ready;", "status == Busy"},
        },
        {
            "iterator to range-for",
            "#include <vector>\n"
            "int sum(const std::vector<int>& values)\n"
            "{\n"
            "    int total = 0;\n"
            "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
            "    {\n"
            "        total += *it;\n"
            "    }\n"
            "    return total;\n"
            "}\n",
            {"for (const auto& value : values)", "total += value;"},
            {"std::vector<int>::const_iterator", "*it"},
        },
        {
            "FILE pointer to stream",
            "#include <cstdio>\n"
            "void save(const char* path)\n"
            "{\n"
            "    FILE* file = fopen(path, \"w\");\n"
            "    if (file != NULL) {\n"
            "        fprintf(file, \"ok\\n\");\n"
            "    }\n"
            "    fclose(file);\n"
            "}\n",
            {"#include <fstream>", "std::ofstream file(path);", "if (file)"},
            {"fprintf(file", "fclose(file)", "file != NULL"},
        },
        {
            "pthread atomic counter",
            "#include <pthread.h>\n"
            "int counter = 0;\n"
            "void* worker(void*)\n"
            "{\n"
            "    ++counter;\n"
            "    return nullptr;\n"
            "}\n"
            "int main()\n"
            "{\n"
            "    pthread_t thread;\n"
            "    pthread_create(&thread, nullptr, worker, nullptr);\n"
            "    return counter;\n"
            "}\n",
            {"#include <atomic>", "std::atomic<int> counter{0};"},
            {"int counter = 0;"},
        },
        {
            "mixed stress case",
            "#include <iostream>\n"
            "#include <vector>\n"
            "typedef int Count;\n"
            "enum Mode { Low, High };\n"
            "class Device\n"
            "{\n"
            "public:\n"
            "    Mode mode = Low;\n"
            "    void ping() {}\n"
            "};\n"
            "void inspect(Device* device)\n"
            "{\n"
            "    if (device != nullptr) { device->ping(); }\n"
            "}\n"
            "int run(const std::vector<int>& values)\n"
            "{\n"
            "    Count total = 0;\n"
            "    Device* device = new Device();\n"
            "    device->mode = High;\n"
            "    inspect(device);\n"
            "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
            "    {\n"
            "        total += *it;\n"
            "    }\n"
            "    std::cout << device->mode << '\\n';\n"
            "    delete device;\n"
            "    return total;\n"
            "}\n",
            {"using Count = int;", "enum class Mode", "std::make_unique<Device>()", "inspect(device.get())", "for (const auto& value : values)"},
            {"delete device", "std::vector<int>::const_iterator", "device->mode = High;"},
        },
    };
}

void runSmokeCase(const SmokeCase& smokeCase)
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(smokeCase.input, smokeOptions());

    require(!result.modernCode.empty(), smokeCase.name + " should return converted code");
    requireCompilePass(result, smokeCase.name);

    for (const std::string& fragment : smokeCase.requiredFragments) {
        require(contains(result.modernCode, fragment),
                smokeCase.name + " should contain required fragment: " + fragment
                    + "\nConverted code:\n" + result.modernCode);
    }
    for (const std::string& fragment : smokeCase.forbiddenFragments) {
        require(!contains(result.modernCode, fragment),
                smokeCase.name + " should not contain stale fragment: " + fragment
                    + "\nConverted code:\n" + result.modernCode);
    }
}

void runVersionMetadataSmokeTest()
{
    require(AppVersion::version() == "1.2.0-rc1", "release candidate version should be 1.2.0-rc1");
    require(AppVersion::releaseChannel() == "rc", "release channel should be rc");
    require(!AppVersion::buildType().empty(), "build type metadata should be available");
    require(!AppVersion::gitCommit().empty(), "git commit metadata should be populated or marked unknown");
    require(!AppVersion::buildDate().empty(), "build date metadata should be populated or marked unknown");

    const std::string diagnostic = AppVersion::diagnosticLine();
    require(contains(diagnostic, "ModernCppConverter version=1.2.0-rc1"),
            "diagnostic line should include app version");
    require(contains(diagnostic, "channel=rc"), "diagnostic line should include release channel");
    require(contains(diagnostic, "build_type="), "diagnostic line should include build type");
    require(contains(diagnostic, "clang_enabled="), "diagnostic line should include Clang support flag");
    require(contains(AppVersion::startupLogLine(), diagnostic),
            "startup diagnostics should include the version diagnostic line");

    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert("int main()\n{\n    return 0;\n}\n", smokeOptions());
    const bool conversionDiagnosticsIncludeVersion = std::any_of(result.diagnosticMessages.begin(),
                                                                 result.diagnosticMessages.end(),
                                                                 [](const std::string& message) {
                                                                     return contains(message, "ModernCppConverter version=1.2.0-rc1");
                                                                 });
    require(conversionDiagnosticsIncludeVersion, "conversion diagnostics should include version metadata");

#ifdef MODERNCPP_SOURCE_DIR
    const std::filesystem::path sourceRoot = MODERNCPP_SOURCE_DIR;
    const std::string rootCMake = readFile(sourceRoot / "CMakeLists.txt");
    require(!contains(rootCMake, "VERSION 0.1.0"), "root CMake should not contain the old 0.1.0 project version");
    require(contains(rootCMake, "VERSION 1.2.0"), "root CMake should carry the 1.2.0 project version");
#endif
}
} // namespace

int main()
{
    runVersionMetadataSmokeTest();

    const std::vector<SmokeCase> cases = smokeCases();
    for (const SmokeCase& smokeCase : cases) {
        runSmokeCase(smokeCase);
    }

    std::cout << "ModernCppConverterSmokeTests passed: " << cases.size() << " snippets\n";
    return EXIT_SUCCESS;
}
