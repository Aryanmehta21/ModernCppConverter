#include "app/ConversionCoordinator.h"
#include "backend/BackendClient.h"
#include "backend/IBackendClient.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/IConverterEngine.h"
#include "converter/ModernCppExplanationGenerator.h"
#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/AstRepresentation.h"
#include "converter/RawTextRepresentation.h"
#include "converter/RuleBasedConverterEngine.h"
#include "converter/StructuralAnalyzers.h"
#include "converter/TokenBasedStructureAnalyzer.h"
#include "converter/TokenRepresentation.h"
#include "converter/TransformationContext.h"
#include "converter/ValueTypePointerOperationScanner.h"
#include "converter/VectorAppendMethodRewritePass.h"
#include "converter/VectorEmulationEliminationPass.h"
#include "converter/VectorGrowthEmulationCleanupPass.h"
#include "converter/VectorParadigmRewritePass.h"
#include "repository/RepositoryBackupService.h"
#include "repository/RepositoryCloneService.h"
#include "repository/RepositoryModernizationService.h"
#include "repository/RepositoryReportWriter.h"
#include "repository/RepositoryScanner.h"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
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

std::string readTextFile(const std::filesystem::path& path)
{
    std::filesystem::path resolved = path;
    if (!std::filesystem::exists(resolved)) {
        for (const auto& prefix : {std::filesystem::path(".."), std::filesystem::path("../.."), std::filesystem::path("../../..")}) {
            const std::filesystem::path candidate = prefix / path;
            if (std::filesystem::exists(candidate)) {
                resolved = candidate;
                break;
            }
        }
    }
    std::ifstream input(resolved);
    require(input.good(), "sample file should be readable: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path makeTempDirectory(const std::string& prefix)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / (prefix + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    require(output.good(), "test file should be writable: " + path.string());
    output << text;
}

class FakeBackendClient final : public IBackendClient
{
public:
    bool available = true;
    mutable int healthChecks = 0;
    mutable bool convertCalled = false;
    mutable ConversionMode lastMode = ConversionMode::OfflineRuleBased;
    BackendConversionResponse response;

    [[nodiscard]] bool isAvailable() const override
    {
        ++healthChecks;
        return available;
    }

    [[nodiscard]] BackendConversionResponse convert(const std::string&,
                                                    const ModernizationOptions&,
                                                    ConversionMode mode,
                                                    const ConversionResult*) const override
    {
        convertCalled = true;
        lastMode = mode;
        return response;
    }
};

int countOccurrences(const std::string& text, const std::string& needle)
{
    int count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

bool hasAppliedRule(const ConversionResult& result, const std::string& ruleName)
{
    return std::any_of(result.changes.begin(), result.changes.end(), [&ruleName](const ConversionChange& change) {
        return change.applied && contains(change.ruleName, ruleName);
    });
}

bool hasAppliedRule(const std::vector<ConversionChange>& changes, const std::string& ruleName)
{
    return std::any_of(changes.begin(), changes.end(), [&ruleName](const ConversionChange& change) {
        return change.applied && contains(change.ruleName, ruleName);
    });
}

bool hasSuggestionRule(const ConversionResult& result, const std::string& ruleName)
{
    return std::any_of(result.changes.begin(), result.changes.end(), [&ruleName](const ConversionChange& change) {
        return !change.applied && contains(change.ruleName, ruleName);
    });
}

void testNullConversion()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert("int* value = NULL;\n");

    require(contains(result.modernCode, "nullptr"), "NULL should be converted to nullptr");
    require(!contains(result.modernCode, "NULL"), "modern code should not contain NULL");
    require(!result.changes.empty(), "NULL conversion should produce a change");
    require(result.changes.front().applied, "NULL conversion should be marked applied");
    require(result.changes.front().before == "int* value = NULL;", "NULL conversion should track the original line");
    require(result.changes.front().after == "int* value = nullptr;", "NULL conversion should track the transformed line");
}

void testTypedefConversion()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert("typedef unsigned long Size;\n");

    require(contains(result.modernCode, "using Size = unsigned long;"), "simple typedef should be converted to using");
    require(contains(result.changes.front().ruleName, "typedef"), "typedef conversion should record its rule name");
    require(result.changes.front().before == "typedef unsigned long Size;", "typedef conversion should track the original line");
    require(result.changes.front().after == "using Size = unsigned long;", "typedef conversion should track the transformed line");
}

void testSuggestionGeneration()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "int values[4];\n"
        "int* p = new int;\n"
        "delete p;\n"
        "int y = (int)3.5;\n"
        "for (int i = 0; i < values; ++i) { values[i] = i; }\n");

    bool sawCast = false;
    bool sawPointer = false;
    bool sawArray = false;
    bool sawLoop = false;

    for (const auto& change : result.changes) {
        if (contains(change.ruleName, "cast")) {
            sawCast = true;
        }
        if (contains(change.ruleName, "pointer")) {
            sawPointer = true;
        }
        if (contains(change.ruleName, "array")) {
            sawArray = true;
        }
        if (contains(change.ruleName, "loop")) {
            sawLoop = true;
        }
        if (!change.applied) {
            require(change.after.empty(), "suggestions should not rewrite code");
        }
    }

    require(sawCast, "old-style cast suggestion should be generated");
    require(sawPointer, "raw pointer ownership suggestion should be generated");
    require(sawArray, "C-style array suggestion should be generated");
    require(sawLoop, "manual loop suggestion should be generated");
}

void testExplanationGeneration()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "typedef unsigned long Size;\n"
        "int* value = NULL;\n"
        "int data[2];\n"
        "int* owned = new int;\n"
        "for (int i = 0; i < data; ++i) { data[i] = i; }\n");

    require(!result.explanation.empty(), "explanation should be generated");
    require(contains(result.explanation, "Summary of changes"), "explanation should contain a summary section");
    require(contains(result.explanation, "Modern C++ concepts used"), "explanation should contain a concepts section");
    require(contains(result.explanation, "Suggested future improvements"), "explanation should contain future improvements");
    require(contains(result.explanation, "automatic change"), "explanation should mention automatic changes");
    require(contains(result.explanation, "suggestion"), "explanation should mention suggestions");
    require(contains(result.explanation, "nullptr is preferred over NULL because it is type-safe and avoids overload ambiguity"),
            "explanation should explain nullptr usage");
    require(contains(result.explanation, "using aliases are the modern way"), "explanation should explain using aliases");
    require(contains(result.explanation, "Smart pointers express ownership directly"), "explanation should explain smart pointers");
    require(contains(result.explanation, "Range-based for loops"), "explanation should explain range-based loops");
}

void testStandaloneExplanationGenerator()
{
    const ModernCppExplanationGenerator generator;
    const ModernizationOptions options;
    const std::vector<ConversionChange> changes{
        {"NULL to nullptr", "int* ptr = NULL;", "int* ptr = nullptr;", "nullptr is type-safe.", true},
        {"Raw pointer ownership suggestion", "int* ptr = new int;", {}, "Consider smart pointers.", false},
    };

    const std::string explanation = generator.generate("int* ptr = nullptr;", changes, options);

    require(contains(explanation, "Summary of changes"), "standalone generator should include a summary");
    require(contains(explanation, "nullptr is preferred over NULL"), "standalone generator should explain nullptr");
    require(contains(explanation, "std::unique_ptr"), "standalone generator should suggest smart pointers");
}

void testOptionEnabledAppliesRule()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useNullptr = true;

    const ConversionResult result = converter.convert("int* value = NULL;\n", options);

    require(contains(result.modernCode, "nullptr"), "enabled nullptr option should apply NULL conversion");
    require(result.changes.front().applied, "enabled nullptr option should create an applied change");
    require(!result.changes.front().skipped, "enabled nullptr option should not be skipped");
}

void testOptionDisabledSkipsRule()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useNullptr = false;

    const ConversionResult result = converter.convert("int* value = NULL;\n", options);

    require(contains(result.modernCode, "NULL"), "disabled nullptr option should preserve NULL");
    require(!result.changes.empty(), "disabled matching option should create a skipped entry");
    require(result.changes.front().skipped, "disabled nullptr option should be marked skipped");
}

void testUnsafeSelectedRuleCreatesSuggestion()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useSmartPointers = true;

    const ConversionResult result = converter.convert("int* owned = new int;\n", options);

    require(contains(result.modernCode, "new int"), "smart pointer suggestion should not rewrite raw allocation");
    require(!result.changes.empty(), "selected unsafe rule should create a suggestion");
    require(!result.changes.front().applied, "unsafe smart pointer rule should not be applied automatically");
    require(!result.changes.front().skipped, "selected unsafe smart pointer rule should not be skipped");
}

void testDefaultSafeOptionsWork()
{
    const RuleBasedConverterEngine converter;
    const ModernizationOptions options;
    const ConversionResult result = converter.convert(
        "typedef int Count;\n"
        "int* ptr = NULL;\n"
        "int* owned = new int;\n"
        "enum Color { Red };\n"
        "void print(const std::string& name);\n",
        options);

    require(contains(result.modernCode, "using Count = int;"), "default options should convert simple typedefs");
    require(contains(result.modernCode, "nullptr"), "default options should convert NULL");

    bool sawSmartPointerSuggestion = false;
    bool sawEnumClassSuggestion = false;
    bool sawStringViewSuggestion = false;

    for (const auto& change : result.changes) {
        sawSmartPointerSuggestion = sawSmartPointerSuggestion || contains(change.ruleName, "Raw pointer");
        sawEnumClassSuggestion = sawEnumClassSuggestion || contains(change.ruleName, "enum class");
        sawStringViewSuggestion = sawStringViewSuggestion || contains(change.ruleName, "std::string_view");
    }

    require(sawSmartPointerSuggestion, "default options should include smart pointer suggestions");
    require(sawEnumClassSuggestion, "default options should include enum class suggestions");
    require(sawStringViewSuggestion, "default options should include string_view suggestions");
}

void testExplanationReflectsSelectedOptions()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useNullptr = false;
    options.useSmartPointers = true;
    options.customInstruction = "Prefer std::vector over raw arrays where possible.";

    const ConversionResult result = converter.convert(
        "int* ptr = NULL;\n"
        "int* owned = new int;\n",
        options);

    require(contains(result.explanation, "Custom modernization instruction"), "explanation should include custom instruction section");
    require(contains(result.explanation, options.customInstruction), "explanation should display the custom instruction");
    require(!contains(result.explanation, "nullptr is preferred over NULL"), "disabled nullptr option should not be explained as selected");
    require(contains(result.explanation, "Smart pointers express ownership directly"), "selected smart pointer option should be explained");
    require(contains(result.explanation, "skipped because an option was disabled"), "explanation should mention skipped disabled rules");
}

void testCodeRepresentations()
{
    RawTextRepresentation raw("int* ptr = NULL;");
    require(raw.kind() == CodeRepresentationKind::RawText, "raw representation should report raw text kind");
    require(raw.sourceText() == "int* ptr = NULL;", "raw representation should expose source text");
    raw.replaceSourceText("int* ptr = nullptr;");
    require(raw.sourceText() == "int* ptr = nullptr;", "raw representation should replace source text");

    TokenRepresentation tokenized("int value;", {"int", "value", ";"});
    require(tokenized.kind() == CodeRepresentationKind::Token, "token representation should report token kind");
    require(tokenized.tokens().size() == 3, "token representation should expose tokens");
    tokenized.replaceSourceText("long value;");
    require(tokenized.tokens().empty(), "token representation should clear stale tokens after source replacement");

    AstRepresentation ast("int value;", "translation-unit placeholder");
    require(ast.kind() == CodeRepresentationKind::Ast, "AST representation should report AST kind");
    require(ast.astSummary() == "translation-unit placeholder", "AST representation should expose placeholder metadata");
    ast.replaceSourceText("long value;");
    require(ast.astSummary().empty(), "AST representation should clear stale AST metadata after source replacement");
}

void testDependencyValidation()
{
    bool threwForNullExplanationGenerator = false;
    try {
        RuleBasedConverterEngine engine({}, nullptr);
    } catch (const std::invalid_argument&) {
        threwForNullExplanationGenerator = true;
    }
    require(threwForNullExplanationGenerator, "rule-based engine should reject a null explanation generator");
}

void testNullMacroRemoval()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#define NULL 0\n"
        "#define NULL ((void*)0)\n"
        "int* value = NULL;\n");

    require(!contains(result.modernCode, "#define NULL"), "NULL macro definitions should be removed");
    require(contains(result.modernCode, "nullptr"), "NULL uses should still convert to nullptr after macro removal");
    require(contains(result.changes.front().ruleName, "macro"), "macro removal should be recorded");
}

void testNullptrMacroWorkaroundRemoval()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#ifndef nullptr\n"
        "#define nullptr NULL\n"
        "#endif\n"
        "int* value = NULL;\n");

    require(!contains(result.modernCode, "#ifndef nullptr"), "nullptr workaround #ifndef should be removed");
    require(!contains(result.modernCode, "#define nullptr"), "nullptr workaround define should be removed");
    require(!contains(result.modernCode, "#endif"), "standalone workaround endif should be removed");
    require(contains(result.modernCode, "nullptr"), "code should use native nullptr");
}

void testStrncpyToStringConversion()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "void copyName(const char* input)\n"
        "{\n"
        "    char name[50];\n"
        "    std::strncpy(name, input, sizeof(name));\n"
        "}\n");

    require(contains(result.modernCode, "#include <string>"), "std::string conversion should add string include");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed when strncpy is gone");
    require(contains(result.modernCode, "std::string name = input;"), "safe strncpy buffer should convert to std::string");
    require(!contains(result.modernCode, "std::strncpy"), "safe strncpy call should be removed");
}

void testIteratorLoopToRangeBasedLoop()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void printValues(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n");

    require(contains(result.modernCode, "for (const auto& value : values)"), "iterator printing loop should convert to range-based loop");
    require(contains(result.modernCode, "std::cout << value << std::endl;"), "iterator dereference should become element variable");
}

void testIndexLoopToRangeBasedLoop()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void printValues(const std::vector<int>& values)\n"
        "{\n"
        "    for (int i = 0; i < values.size(); ++i)\n"
        "    {\n"
        "        std::cout << values[i] << std::endl;\n"
        "    }\n"
        "}\n");

    require(contains(result.modernCode, "for (const auto& value : values)"), "index printing loop should convert to range-based loop");
    require(!contains(result.modernCode, "values[i]"), "index expression should be replaced by element variable");
}

void testConvertedLegacySampleCompiles()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        "#define NULL 0\n"
        "#ifndef nullptr\n"
        "#define nullptr NULL\n"
        "#endif\n"
        "void convertedSample(const char* input, std::vector<int>& values)\n"
        "{\n"
        "    char name[50];\n"
        "    std::strncpy(name, input, sizeof(name));\n"
        "    int* ptr = NULL;\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "    for (int i = 0; i < values.size(); ++i)\n"
        "    {\n"
        "        std::cout << values[i] << std::endl;\n"
        "    }\n"
        "    (void)name;\n"
        "    (void)ptr;\n"
        "}\n");

    require(!contains(result.modernCode, "#define NULL"), "converted sample should not contain NULL macro");
    require(!contains(result.modernCode, "#define nullptr"), "converted sample should not contain nullptr macro");
    require(!contains(result.modernCode, "#ifndef nullptr"), "converted sample should not contain broken nullptr preprocessor block");
    require(contains(result.modernCode, "std::string name = input;"), "converted sample should use std::string");
    require(contains(result.modernCode, "for (const auto& value : values)"), "converted sample should use range-based loops");
    require(contains(result.explanation, "automatic change"), "converted sample explanation should mention changes");

    bool sawMacro = false;
    bool sawString = false;
    bool sawLoop = false;
    for (const auto& change : result.changes) {
        sawMacro = sawMacro || contains(change.ruleName, "macro");
        sawString = sawString || contains(change.ruleName, "strncpy");
        sawLoop = sawLoop || contains(change.ruleName, "Range-based");
    }
    require(sawMacro, "conversion details should explain macro cleanup");
    require(sawString, "conversion details should explain string conversion");
    require(sawLoop, "conversion details should explain loop conversion");

    const std::filesystem::path sourcePath = std::filesystem::temp_directory_path() / "modern_cpp_converter_generated_sample.cpp";
    {
        std::ofstream output(sourcePath);
        output << result.modernCode << '\n';
    }

    const std::filesystem::path objectPath = std::filesystem::temp_directory_path() / "modern_cpp_converter_generated_sample.o";
    const std::string command = "clang++ -std=c++20 -Wall -Wextra -Wpedantic -c "
        + sourcePath.string() + " -o " + objectPath.string();
    const int compileResult = std::system(command.c_str());
    require(compileResult == 0, "converted generated sample should compile as C++20");
    std::filesystem::remove(sourcePath);
    std::filesystem::remove(objectPath);
}

void testConvertsSimpleNewDeleteToMakeUnique()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "void run()\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    delete primaryTool;\n"
        "}\n");

    require(contains(result.modernCode, "#include <memory>"), "unique_ptr conversion should add memory include");
    require(contains(result.modernCode, "auto primaryTool = std::make_unique<DiagnosticTool>();"), "simple new/delete should convert to make_unique");
    require(!contains(result.modernCode, "delete primaryTool"), "matching delete should be removed");
    require(contains(result.changes.front().ruleName, "std::unique_ptr"), "applied ownership change should be recorded");
}

void testConvertsNewWithConstructorArgsToMakeUnique()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "struct Config {};\n"
        "enum Mode { Fast };\n"
        "class VehicleController { public: VehicleController(Config, Mode) {} };\n"
        "void run(Config config, Mode mode)\n"
        "{\n"
        "    VehicleController* controller = new VehicleController(config, mode);\n"
        "    delete controller;\n"
        "}\n");

    require(contains(result.modernCode, "auto controller = std::make_unique<VehicleController>(config, mode);"),
            "constructor arguments should be preserved in make_unique");
    require(!contains(result.modernCode, "delete controller"), "matching delete with constructor args should be removed");
}

void testOwnershipConversionDoesNotDuplicateSuggestions()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "void run()\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    delete primaryTool;\n"
        "}\n");

    for (const auto& change : result.changes) {
        require(!(contains(change.ruleName, "Raw pointer") && !change.applied),
                "converted allocation should not also appear as a raw pointer suggestion");
    }
}

void testDoesNotConvertEscapingPointer()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "DiagnosticTool* globalTool;\n"
        "void run()\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    globalTool = primaryTool;\n"
        "    delete primaryTool;\n"
        "}\n");

    require(contains(result.modernCode, "DiagnosticTool* primaryTool = new DiagnosticTool();"),
            "escaping pointer should not be converted");
    require(contains(result.modernCode, "delete primaryTool;"), "delete should remain for escaping pointer");
}

void testDoesNotConvertReturnedPointer()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "DiagnosticTool* makeTool()\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    return primaryTool;\n"
        "}\n");

    require(contains(result.modernCode, "DiagnosticTool* primaryTool = new DiagnosticTool();"),
            "returned pointer should not be converted");
    require(!contains(result.modernCode, "std::make_unique"), "returned pointer conversion should stay suggestion-only");
}

void testDoesNotConvertAmbiguousOwnership()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "void run(bool cleanup)\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    if (cleanup) delete primaryTool;\n"
        "}\n");

    require(contains(result.modernCode, "DiagnosticTool* primaryTool = new DiagnosticTool();"),
            "conditional delete should not be converted automatically");
    require(!contains(result.modernCode, "std::make_unique"), "ambiguous ownership should stay suggestion-only");
}

void testStringViewAppliesOnlyWhenEnabledAndSafe()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.applyStringViewWhenSafe = true;

    const ConversionResult result = converter.convert(
        "#include <string>\n"
        "void printName(const std::string& name);\n",
        options);

    require(contains(result.modernCode, "#include <string_view>"), "safe string_view conversion should add string_view include");
    require(contains(result.modernCode, "void printName(std::string_view name);"), "safe const string reference should convert to string_view");
}

void testGeneratedOwnershipSample()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class DiagnosticTool {};\n"
        "class Config {};\n"
        "enum Mode { Fast };\n"
        "class VehicleController { public: VehicleController(Config, Mode) {} };\n"
        "class SensorNode {};\n"
        "void run(Config config, Mode mode)\n"
        "{\n"
        "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
        "    DiagnosticTool* localTool = new DiagnosticTool;\n"
        "    VehicleController* controller = new VehicleController(config, mode);\n"
        "    SensorNode* sharedSensor = new SensorNode();\n"
        "    delete primaryTool;\n"
        "    delete localTool;\n"
        "    delete controller;\n"
        "    delete sharedSensor;\n"
        "}\n");

    require(contains(result.modernCode, "std::make_unique"), "generated ownership sample should use make_unique");
    require(!contains(result.modernCode, "delete primaryTool"), "primaryTool delete should be removed");
    require(!contains(result.modernCode, "delete localTool"), "localTool delete should be removed");
    require(!contains(result.modernCode, "delete controller"), "controller delete should be removed");
    require(!contains(result.modernCode, "delete sharedSensor"), "sharedSensor delete should be removed");
    require(!result.changes.empty(), "generated ownership sample should have applied changes");
    require(countOccurrences(result.modernCode, "std::make_unique") == 4, "all simple ownership patterns should convert");

    for (const auto& change : result.changes) {
        require(!(contains(change.ruleName, "Raw pointer") && !change.applied),
                "converted generated ownership sample should not include duplicate raw pointer suggestions");
    }
}

void testOfflineHelperComputationExtractedToLambda()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useLambdas = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult result = converter.convert(readTextFile("tests/samples/legacy_lambda_candidate.cpp"), options);

    require(contains(result.modernCode, "const auto computeTemp"), "local computation should be extracted to a generated lambda");
    require(contains(result.modernCode, "return x == computeTemp(x);"), "comparison should call extracted computation lambda");
    require(std::any_of(result.changes.begin(), result.changes.end(), [](const ConversionChange& change) {
        return change.applied && contains(change.ruleName, "Extract repeated/simple computation to lambda")
            && !contains(change.ruleName, "reverse")
            && !contains(change.ruleName, "palindrome");
    }), "lambda extraction should be recorded as an applied change");
}

void testOfflineFunctorToLambda()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useLambdas = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult result = converter.convert("struct Doubler { int operator()(int value) const { return value * 2; } };\n", options);

    require(contains(result.modernCode, "const auto doubler = [](int value)"), "simple stateless functor should convert to lambda");
}

void testOfflineAutoAndConstexprUsage()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useAuto = true;
    options.useConstexpr = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult result = converter.convert(
        "const int MaxCount = 10;\n"
        "std::string name = std::string{\"Ada\"};\n",
        options);

    require(contains(result.modernCode, "constexpr int MaxCount = 10;"), "simple const integral value should become constexpr");
    require(contains(result.modernCode, "auto name = std::string{\"Ada\"};"), "obvious repeated construction should use auto");
}

void testOfflineOldStyleCastConversion()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult result = converter.convert("int value = (int)count;\n", options);

    require(contains(result.modernCode, "static_cast<int>(count)"), "simple scalar old-style cast should become static_cast");
}

void testOfflineOverrideAnnotation()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useOverrideFinal = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult result = converter.convert(
        "class Base { public: virtual void run(); };\n"
        "class Derived : public Base { public:\n"
        "    void run();\n"
        "};\n",
        options);

    require(contains(result.modernCode, "void run() override;"), "clear overriding declaration should gain override");
}

void testOfflineStructuredBindingAggressiveSafe()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useStructuredBindings = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::AggressiveSafe;

    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void print(const std::map<int, int>& values)\n"
        "{\n"
        "    for (const auto& entry : values)\n"
        "    {\n"
        "        std::cout << entry.first << entry.second << std::endl;\n"
        "    }\n"
        "}\n",
        options);

    require(contains(result.modernCode, "for (const auto& [key, value] : values)"), "aggressive safe mode should apply clear structured binding loop");
}

void testOfflineRequiredIncludesAreNotDuplicated()
{
    RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <memory>\n"
        "class DiagnosticTool {};\n"
        "void run()\n"
        "{\n"
        "    DiagnosticTool* tool = new DiagnosticTool();\n"
        "    delete tool;\n"
        "}\n");

    require(countOccurrences(result.modernCode, "#include <memory>") == 1, "memory include should not be duplicated");
}

void testOfflineCompileVerificationPassesForSimpleSample()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert("int main() { return 0; }\n", options);

    require(result.compileVerificationEnabled, "compile verification should be marked enabled");
    if (result.compilerUsed.empty()) {
        require(contains(result.compilerOutput, "Compiler not found"), "missing compiler should be reported gracefully");
    } else {
        require(result.compileVerificationPassed, "simple converted sample should pass syntax verification");
    }
}

void testCompileVerifierHandlesUnavailableCompilerGracefully()
{
    const CompileVerificationResult result = CompileVerifier::verifySyntaxOnly("int main() { return 0; }\n");
    require(result.verificationEnabled, "manual compile verifier should report enabled verification");
    require(result.compilerFound || contains(result.output, "Compiler not found"), "compiler absence should be a graceful result");
}

void testOfflineSampleFilesModernize()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options;
    options.useAuto = true;
    options.useConstexpr = true;
    options.useLambdas = true;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;

    const ConversionResult pointers = converter.convert(readTextFile("tests/samples/legacy_pointers.cpp"), options);
    const ConversionResult strings = converter.convert(readTextFile("tests/samples/legacy_strings.cpp"), options);
    const ConversionResult loops = converter.convert(readTextFile("tests/samples/legacy_loops.cpp"), options);
    const ConversionResult mixed = converter.convert(readTextFile("tests/samples/legacy_mixed.cpp"), options);

    require(contains(pointers.modernCode, "std::make_unique"), "pointer sample should use make_unique");
    require(contains(strings.modernCode, "std::string name = input;"), "string sample should use std::string");
    require(contains(loops.modernCode, "for (const auto& value : values)"), "loop sample should use range-based loops");
    require(contains(mixed.modernCode, "using Size = unsigned long;"), "mixed sample should convert typedef");
    require(contains(mixed.modernCode, "static_cast<int>"), "mixed sample should convert simple old-style cast");
    require(!pointers.changes.empty() && !strings.changes.empty() && !loops.changes.empty() && !mixed.changes.empty(),
            "sample conversions should produce applied changes");
}

ModernizationOptions aiStyleOptions(CppStandard standard = CppStandard::Cpp20)
{
    ModernizationOptions options;
    options.offlineModernizationLevel = OfflineModernizationLevel::AiStyleAggressiveRewrite;
    options.offlineRewriteStyle = OfflineRewriteStyle::AggressiveAiLikeRewrite;
    options.targetStandard = standard;
    options.useAuto = true;
    options.useLambdas = true;
    options.useConstexpr = true;
    options.useRanges = standard == CppStandard::Cpp20;
    options.useStructuredBindings = true;
    return options;
}

ModernizationOptions structuralOptions()
{
    ModernizationOptions options;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;
    options.useConstexpr = true;
    options.useRangeBasedFor = true;
    options.useSmartPointers = true;
    options.applySafeOwnershipModernization = true;
    return options;
}

void testTokenBasedStructureAnalyzerFindsReusableBlocks()
{
    const std::string sample =
        "#ifndef LEGACY_FLAG\n"
        "#define LEGACY_FLAG 1\n"
        "#endif\n"
        "typedef struct\n"
        "{\n"
        "    int value;\n"
        "} Record;\n"
        "class Holder { int value; };\n"
        "void visit(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        *it = 1;\n"
        "    }\n"
        "}\n";

    const TokenBasedStructureAnalyzer analyzer;
    const CodeStructure structure = analyzer.analyze(sample);

    require(!structure.preprocessorBlocks.empty(), "token analyzer should find preprocessor blocks");
    require(!structure.typedefStructs.empty(), "token analyzer should find C-style typedef structs");
    require(!structure.classes.empty(), "token analyzer should find class blocks");
    require(!structure.loops.empty(), "token analyzer should find loop blocks");

    require(!PreprocessorAnalyzer{}.analyze(sample).empty(), "preprocessor analyzer wrapper should find blocks");
    require(!TypeDeclarationAnalyzer{}.analyzeTypedefStructs(sample).empty(), "type analyzer wrapper should find typedef structs");
    require(!ClassResourceAnalyzer{}.analyzeClasses(sample).empty(), "class resource analyzer wrapper should find classes");
    require(!LoopAnalyzer{}.analyzeLoops(sample).empty(), "loop analyzer wrapper should find loops");
    require(OwnershipAnalyzer{}.hasPointerArithmetic("values + 1", "values"), "ownership analyzer should detect pointer arithmetic");
}

void testStructuralPreprocessorCleanupAndConstantMacros()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#ifndef nullptr\n"
        "#define nullptr NULL\n"
        "#endif\n"
        "#if defined(OLD_NULL)\n"
        "#define NULL 0\n"
        "#endif\n"
        "#define MAX_ITEMS 64\n"
        "#define APP_NAME \"tool\"\n"
        "#define SCALE_VALUE(x) ((x) * 2)\n"
        "int* value = NULL;\n",
        structuralOptions());

    require(!contains(result.modernCode, "#ifndef nullptr"), "obsolete nullptr workaround block should be removed");
    require(!contains(result.modernCode, "#if defined(OLD_NULL)"), "empty NULL workaround block should be removed");
    require(!contains(result.modernCode, "#endif\n#endif"), "preprocessor cleanup should not leave dangling endif pairs");
    require(contains(result.modernCode, "inline constexpr auto MAX_ITEMS = 64;"), "numeric macro constant should become constexpr");
    require(contains(result.modernCode, "inline constexpr auto APP_NAME = \"tool\";"), "string macro constant should become constexpr");
    require(contains(result.modernCode, "#define SCALE_VALUE(x)"), "function-like macro should be preserved");
    require(hasAppliedRule(result, "Remove obsolete preprocessor workaround block"), "obsolete preprocessor removal should be tracked");
    require(hasAppliedRule(result, "Constant macro to constexpr"), "constant macro conversion should be tracked");
    require(hasSuggestionRule(result, "Constant macro to constexpr"), "function-like macro should produce suggestion");
}

void testStructuralTypedefStructModernization()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "typedef struct\n"
        "{\n"
        "    // stored value\n"
        "    int value;\n"
        "    double weight;\n"
        "} Record;\n",
        structuralOptions());

    require(contains(result.modernCode, "struct Record"), "C-style typedef struct should become normal C++ struct");
    require(contains(result.modernCode, "// stored value"), "typedef struct modernization should preserve comments");
    require(!contains(result.modernCode, "typedef struct"), "redundant typedef wrapper should be removed");
    require(hasAppliedRule(result, "C-style typedef struct to C++ struct"), "typedef struct conversion should be tracked");
}

void testStructuralCharBufferMemberModernizesToString()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class TextHolder\n"
        "{\n"
        "public:\n"
        "    void assign(const char* input)\n"
        "    {\n"
        "        std::strncpy(name, input, sizeof(name));\n"
        "        name[sizeof(name) - 1] = '\\0';\n"
        "    }\n\n"
        "    bool matches(const char* input) const\n"
        "    {\n"
        "        return std::strcmp(name, input) == 0;\n"
        "    }\n\n"
        "private:\n"
        "    char name[64];\n"
        "};\n",
        structuralOptions());

    require(contains(result.modernCode, "std::string name;"), "text char buffer member should become std::string");
    require(contains(result.modernCode, "name = input;"), "C-string copy should become string assignment");
    require(contains(result.modernCode, "return name == input;"), "C-string comparison should become string comparison");
    require(!contains(result.modernCode, "std::strncpy"), "strncpy should be removed after string modernization");
    require(!contains(result.modernCode, "std::strcmp"), "strcmp should be removed after string modernization");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed when no C-string APIs remain");
    require(contains(result.modernCode, "#include <string>"), "string modernization should add string include");
    require(hasAppliedRule(result, "Char buffer member to std::string"), "char buffer conversion should be tracked");
    require(hasAppliedRule(result, "C-string copy to std::string assignment"), "C-string copy conversion should be tracked");
    require(hasAppliedRule(result, "C-string comparison to std::string comparison"), "C-string comparison conversion should be tracked");
}

void testStructuralRawDynamicArrayModernizesToVector()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "class NumberStore\n"
        "{\n"
        "public:\n"
        "    explicit NumberStore(int count)\n"
        "        : count(count)\n"
        "    {\n"
        "        values = new int[count];\n"
        "    }\n\n"
        "    ~NumberStore()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "    int at(int index) const\n"
        "    {\n"
        "        return values[index];\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    int* values;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"), "dynamic array member should become vector");
    require(contains(result.modernCode, "values.resize(count);"), "new[] allocation should become vector resize");
    require(!contains(result.modernCode, "delete[] values;"), "delete[] should be removed after vector modernization");
    require(!contains(result.modernCode, "~NumberStore()"), "cleanup-only destructor should be removed after vector modernization");
    require(contains(result.modernCode, "#include <vector>"), "vector modernization should add vector include");
    require(hasAppliedRule(result, "Raw dynamic array to std::vector"), "dynamic array conversion should be tracked");
    require(hasAppliedRule(result, "Remove delete array after vector modernization"), "delete[] removal should be tracked");
    require(hasAppliedRule(result, "Rule of Zero after container modernization"), "Rule of Zero cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "converted vector sample should pass syntax verification");
    }
}

void testStructuralLocalDynamicArrayModernizesToVector()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "void fill(int count)\n"
        "{\n"
        "    int* values = new int[count];\n"
        "    values[0] = 1;\n"
        "    delete[] values;\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "std::vector<int> values(count);"), "local dynamic array should become vector");
    require(!contains(result.modernCode, "delete[] values;"), "local delete[] should be removed");
    require(hasAppliedRule(result, "Raw dynamic array to std::vector"), "local dynamic array conversion should be tracked");
}

void testStructuralIteratorAndIndexLoopsModernize()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(const std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n\n"
        "void reset(std::vector<int>& values)\n"
        "{\n"
        "    for (std::size_t i = 0; i < values.size(); ++i)\n"
        "    {\n"
        "        values[i] = 0;\n"
        "    }\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "for (const auto& value : values)"), "const_iterator loop should become const range loop");
    require(contains(result.modernCode, "std::cout << value << '\\n';"), "loop modernization should replace iterator dereference and endl");
    require(contains(result.modernCode, "for (auto& value : values)"), "mutable index loop should become mutable range loop");
    require(contains(result.modernCode, "value = 0;"), "index access should be replaced consistently in loop body");
    require(hasAppliedRule(result, "Explicit iterator loop to range-based for"), "iterator loop conversion should be tracked");
    require(hasAppliedRule(result, "Index loop to range-based for"), "index loop conversion should be tracked");
}

void testStructuralPreprocessorBalanceValidationRemovesDanglingEndif()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#endif\n"
        "int value = 0;\n",
        structuralOptions());

    require(!contains(result.modernCode, "#endif"), "dangling endif should be removed");
    require(hasAppliedRule(result, "Preprocessor block cleanup"), "preprocessor balance cleanup should be tracked");
}

void testDependentVectorCleanupUpdatesAllUsages()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "class ArrayOwner\n"
        "{\n"
        "public:\n"
        "    explicit ArrayOwner(int count)\n"
        "        : count(count), values(nullptr)\n"
        "    {\n"
        "        values = new int[count];\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            values[0] = 1;\n"
        "        }\n"
        "    }\n\n"
        "    ArrayOwner(const ArrayOwner& source)\n"
        "        : count(source.count), values(nullptr)\n"
        "    {\n"
        "        values = new int[count];\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            values[i] = source.values[i];\n"
        "        }\n"
        "    }\n\n"
        "    ArrayOwner& operator=(const ArrayOwner& source)\n"
        "    {\n"
        "        if (this != &source)\n"
        "        {\n"
        "            count = source.count;\n"
        "            delete[] values;\n"
        "            values = new int[count];\n"
        "            for (int i = 0; i < count; ++i)\n"
        "            {\n"
        "                values[i] = source.values[i];\n"
        "            }\n"
        "        }\n"
        "        return *this;\n"
        "    }\n\n"
        "    ~ArrayOwner()\n"
        "    {\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            delete[] values;\n"
        "        }\n"
        "        values = nullptr;\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    int* values;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"), "array member should become vector");
    require(contains(result.modernCode, "values.resize(count);"), "vector should be resized before indexed writes");
    require(!contains(result.modernCode, "new int["), "all dependent new[] allocations should be removed");
    require(!contains(result.modernCode, "delete[] values"), "all dependent delete[] cleanup should be removed");
    require(!contains(result.modernCode, "values(nullptr)"), "pointer-style vector initializer should be removed");
    require(!contains(result.modernCode, "values = nullptr"), "pointer-style vector null assignment should be removed");
    require(!contains(result.modernCode, "values != nullptr"), "vector nullptr checks should be removed or unwrapped");
    require(contains(result.modernCode, "values[0] = 1;"), "body guarded by old pointer null check should be preserved");
    require(contains(result.modernCode, "ArrayOwner(const ArrayOwner&) = default;"), "manual copy constructor should become defaulted");
    require(contains(result.modernCode, "ArrayOwner& operator=(const ArrayOwner&) = default;"), "manual copy assignment should become defaulted");
    require(!contains(result.modernCode, "~ArrayOwner()"), "cleanup-only destructor should be removed after dependent cleanup");
    require(hasAppliedRule(result, "Replace raw array allocation with vector resize"), "dependent vector allocation cleanup should be tracked");
    require(hasAppliedRule(result, "Remove nullptr check after vector modernization"), "dependent vector nullptr cleanup should be tracked");
    require(hasAppliedRule(result, "Remove obsolete copy constructor after vector modernization"), "copy constructor cleanup should be tracked");
    require(hasAppliedRule(result, "Remove obsolete copy assignment after vector modernization"), "copy assignment cleanup should be tracked");
    require(hasAppliedRule(result, "Rule of Zero cascade cleanup"), "copy operation cleanup should be tracked");
    require(hasAppliedRule(result, "Vector cascade cleanup"), "vector cascade cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "dependent vector cleanup sample should pass syntax verification");
    }
}

void testDependentStringCleanupUpdatesAllUsages()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class StringOwner\n"
        "{\n"
        "public:\n"
        "    void assign(const char* input)\n"
        "    {\n"
        "        std::strncpy(name, input, sizeof(name) - 1);\n"
        "        name[sizeof(name) - 1] = '\\0';\n"
        "    }\n\n"
        "    void copy(const char* input)\n"
        "    {\n"
        "        strcpy(name, input);\n"
        "    }\n\n"
        "    bool matches(const char* input) const\n"
        "    {\n"
        "        return strcmp(name, input) == 0;\n"
        "    }\n\n"
        "    std::size_t length() const\n"
        "    {\n"
        "        return strlen(name);\n"
        "    }\n\n"
        "    std::size_t capacityHint() const\n"
        "    {\n"
        "        return sizeof(name);\n"
        "    }\n\n"
        "private:\n"
        "    char name[32];\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string name;"), "char buffer should become string");
    require(contains(result.modernCode, "name = input;"), "strncpy/strcpy should become assignment");
    require(!contains(result.modernCode, "std::strncpy"), "strncpy should be removed");
    require(!contains(result.modernCode, "strcpy"), "strcpy should be removed");
    require(!contains(result.modernCode, "strcmp"), "strcmp should be removed");
    require(!contains(result.modernCode, "strlen"), "strlen should be removed");
    require(!contains(result.modernCode, "sizeof(name)"), "sizeof converted string should not remain as buffer size");
    require(!contains(result.modernCode, "'\\0'"), "manual null termination should be removed");
    require(contains(result.modernCode, "return name == input;"), "strcmp should become string comparison");
    require(contains(result.modernCode, "return name.size();"), "strlen should become string size");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring should be removed when C APIs are gone");
    require(hasAppliedRule(result, "Replace C-string copy with string assignment"), "dependent string copy cleanup should be tracked");
    require(hasAppliedRule(result, "Replace C-string comparison with string comparison"), "dependent string comparison cleanup should be tracked");
    require(hasAppliedRule(result, "Replace strlen with string size"), "strlen cleanup should be tracked");
    require(hasAppliedRule(result, "Remove invalid sizeof string buffer usage"), "sizeof cleanup should be tracked");
    require(hasAppliedRule(result, "Remove manual null termination after string modernization"), "dependent string null cleanup should be tracked");
    require(hasAppliedRule(result, "String cascade cleanup"), "string cascade cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "dependent string cleanup sample should pass syntax verification");
    }
}

void testNestedStringMemberCascadeCleanup()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "#include <vector>\n"
        "struct Entry\n"
        "{\n"
        "    char label[24];\n"
        "};\n\n"
        "void assign(std::vector<Entry>& entries, const char* input)\n"
        "{\n"
        "    std::strncpy(entries[0].label, input, sizeof(entries[0].label));\n"
        "    entries[0].label[sizeof(entries[0].label) - 1] = '\\0';\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::string label;"), "nested char member should become string");
    require(contains(result.modernCode, "entries[0].label = input;"), "nested string member strncpy should become assignment");
    require(!contains(result.modernCode, "strncpy"), "nested strncpy should be removed");
    require(!contains(result.modernCode, "sizeof(entries[0].label)"), "nested sizeof string capacity should be removed");
    require(!contains(result.modernCode, "'\\0'"), "nested manual terminator should be removed");
    require(hasAppliedRule(result, "Replace C-string copy with string assignment"), "nested string copy cleanup should be tracked");
    require(hasAppliedRule(result, "Remove manual null termination after string modernization"), "nested null cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "nested string member cleanup should pass syntax verification");
    }
}

void testCompilerDiagnosticCleanupFixesKnownLeftovers()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Container",
        true,
        "Raw dynamic array to std::vector",
        {"remove nullptr checks", "remove delete[]"},
        {},
        false,
    });
    context.registerTypeChange(TypeChangeRecord{
        "label",
        "char[24]",
        "std::string",
        "Entry",
        true,
        "Char buffer member to std::string",
        {"rewrite C-string APIs"},
        {},
        false,
    });

    const std::string code =
        "#include <cstring>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "struct Entry { std::string label; };\n"
        "struct Container\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    ~Container()\n"
        "    {\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            delete[] values;\n"
        "        }\n"
        "    }\n"
        "};\n"
        "void assign(std::vector<Entry>& entries, const char* input)\n"
        "{\n"
        "    std::strncpy(entries[0].label, input, sizeof(entries[0].label));\n"
        "}\n";
    const std::string compilerOutput =
        "error: no match for 'operator!=' (operand types are 'std::vector<int>' and 'std::nullptr_t')\n"
        "error: cannot convert 'std::string' to 'char*' for argument 1 to 'strncpy'\n";

    std::vector<ConversionChange> changes;
    const CompilerDiagnosticCleanupPass pass;
    const std::string fixed = pass.run(code, context, compilerOutput, changes);

    require(!contains(fixed, "values != nullptr"), "diagnostic cleanup should remove vector nullptr comparison");
    require(!contains(fixed, "delete[] values"), "diagnostic cleanup should remove vector delete[]");
    require(contains(fixed, "entries[0].label = input;"), "diagnostic cleanup should rewrite nested string strncpy");
    require(!contains(fixed, "std::strncpy"), "diagnostic cleanup should remove string write API");
    require(hasAppliedRule(changes, "Compiler diagnostic cleanup"), "compiler diagnostic cleanup should be tracked");
}

void testValueTypePointerOperationScannerRemovesPointerLeftovers()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "int*",
        "std::vector<int>",
        "ResourceOwner",
        true,
        "Raw dynamic array to std::vector",
        {"remove pointer-specific operations"},
        {},
        false,
    });

    const std::string code =
        "#include <vector>\n"
        "struct ResourceOwner\n"
        "{\n"
        "    std::vector<int> items;\n"
        "    ~ResourceOwner()\n"
        "    {\n"
        "        if (items != nullptr)\n"
        "        {\n"
        "            delete[] items;\n"
        "        }\n"
        "        items = nullptr;\n"
        "    }\n"
        "};\n";

    std::vector<ConversionChange> changes;
    const ValueTypePointerOperationScanner scanner;
    const std::string fixed = scanner.rewrite(code, context, changes);

    require(!contains(fixed, "items != nullptr"), "scanner should remove value-type nullptr comparison");
    require(!contains(fixed, "items = nullptr"), "scanner should remove value-type nullptr assignment");
    require(!contains(fixed, "delete[] items"), "scanner should remove value-type delete[]");
    require(!contains(fixed, "~ResourceOwner()"), "scanner should remove cleanup-only destructor after cleanup");
    require(hasAppliedRule(changes, "Value-type pointer operation scanner"), "scanner summary should be tracked");
    require(hasAppliedRule(changes, "Rule of Zero destructor cleanup"), "scanner should track Rule of Zero destructor cleanup");
}

void testValueTypeNullptrEqualityBecomesValueStateCheck()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "int*",
        "std::vector<int>",
        "Collection",
        true,
        "Raw dynamic array to std::vector",
        {"remove nullptr checks"},
        {},
        false,
    });

    const std::string code =
        "#include <vector>\n"
        "struct Collection\n"
        "{\n"
        "    std::vector<int> items;\n"
        "    bool empty() const\n"
        "    {\n"
        "        if (items == nullptr) { return true; }\n"
        "        return false;\n"
        "    }\n"
        "};\n";
    const std::string compilerOutput =
        "error: no match for 'operator==' (operand types are 'const std::vector<int>' and 'std::nullptr_t')\n";

    std::vector<ConversionChange> changes;
    const CompilerDiagnosticCleanupPass pass;
    const std::string fixed = pass.run(code, context, compilerOutput, changes);

    require(!contains(fixed, "items == nullptr"), "value-type nullptr equality should be removed");
    require(contains(fixed, "if (items.empty())"), "nullptr equality should become value-state check");
    require(hasAppliedRule(changes, "Remove invalid nullptr check after value-type modernization"),
            "value-type nullptr cleanup should be tracked");
    require(hasAppliedRule(changes, "Value-type pointer operation scanner"), "value-type scanner should be tracked");
}

void testEmptyCleanupBlockAfterValueTypeModernizationIsRemoved()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "int*",
        "std::vector<int>",
        "Collection",
        true,
        "Raw dynamic array to std::vector",
        {"remove empty cleanup blocks"},
        {},
        false,
    });

    const std::string code =
        "#include <vector>\n"
        "struct Collection\n"
        "{\n"
        "    std::vector<int> items;\n"
        "    void cleanup()\n"
        "    {\n"
        "        if (items != nullptr)\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "};\n";

    std::vector<ConversionChange> changes;
    const ImpactCascadingCleanupPass pass;
    const std::string fixed = pass.run(code, context, changes);

    require(!contains(fixed, "items != nullptr"), "empty nullptr block should be removed");
    require(!contains(fixed, "if (items"), "empty cleanup if should not remain");
    require(hasAppliedRule(changes, "Remove empty cleanup block"), "empty cleanup block removal should be tracked");
}

void testExplicitMutableIteratorLoopModernizes()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "void increment(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        *it = *it + 1;\n"
        "    }\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "for (auto& value : values)"), "mutable iterator loop should become auto& range loop");
    require(contains(result.modernCode, "value = value + 1;"), "mutable dereferences should be rewritten consistently");
    require(!contains(result.modernCode, "*it"), "iterator dereference should not remain");
    require(hasAppliedRule(result, "Explicit iterator loop to range-based for"), "mutable iterator conversion should be tracked");
}

void testIteratorLoopWithEraseIsPreservedWithWarning()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "void removeZeroes(std::vector<int>& values)\n"
        "{\n"
        "    for (auto it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        if (*it == 0)\n"
        "        {\n"
        "            values.erase(it);\n"
        "        }\n"
        "    }\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "for (auto it = values.begin(); it != values.end(); ++it)"),
            "erase iterator loop should remain explicit");
    require(hasSuggestionRule(result, "Explicit iterator loop to range-based for"),
            "unsafe erase loop should emit iterator modernization warning");
}

void testVectorGrowthEmulationCleanupModernizesAppend()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "class SequenceBuffer\n"
        "{\n"
        "public:\n"
        "    SequenceBuffer()\n"
        "        : count(0), capacity(2), values(nullptr)\n"
        "    {\n"
        "        values = new int[capacity];\n"
        "    }\n\n"
        "    void add(int value)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            std::size_t newCapacity = capacity * 2;\n"
        "            int* grown = new int[newCapacity];\n"
        "            for (std::size_t i = 0; i < count; ++i)\n"
        "            {\n"
        "                grown[i] = values[i];\n"
        "            }\n"
        "            delete[] values;\n"
        "            values = grown;\n"
        "            capacity = newCapacity;\n"
        "        }\n"
        "        values[count] = value;\n"
        "        ++count;\n"
        "    }\n\n"
        "    std::size_t size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "    ~SequenceBuffer()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "private:\n"
        "    std::size_t count;\n"
        "    std::size_t capacity;\n"
        "    int* values;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"), "growth storage should become vector");
    require(contains(result.modernCode, "values.reserve(capacity);"), "initial vector allocation should become reserve");
    require(contains(result.modernCode, "values.push_back(value);"), "indexed append should become push_back");
    require(contains(result.modernCode, "return values.size();"), "count mirror getter should become vector size");
    require(!contains(result.modernCode, "int* grown"), "temporary growth buffer should be removed");
    require(!contains(result.modernCode, "new int["), "growth new[] should be removed");
    require(!contains(result.modernCode, "delete[] values"), "delete[] for vector should be removed");
    require(!contains(result.modernCode, "delete[] grown"), "delete[] for growth temp should be removed");
    require(!contains(result.modernCode, "values = grown"), "raw pointer assignment to vector should be removed");
    require(!contains(result.modernCode, "grown[i] = values[i]"), "manual growth copy loop should be removed");
    require(!contains(result.modernCode, "values[count] = value"), "indexed append should not remain");
    require(!contains(result.modernCode, "~SequenceBuffer()"), "cleanup-only destructor should be removed");
    require(hasAppliedRule(result, "Vector growth emulation cleanup"), "vector growth cleanup should be tracked");
    require(hasAppliedRule(result, "Remove manual vector growth copy loop"), "manual copy loop cleanup should be tracked");
    require(hasAppliedRule(result, "Indexed append to vector push_back"), "push_back rewrite should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "vector growth cleanup sample should pass syntax verification");
    }
}

void testVectorGrowthPassFlagsAmbiguousRawAssignment()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Holder",
        true,
        "Raw dynamic array to std::vector",
        {"remove raw pointer assignment"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const VectorGrowthEmulationCleanupPass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Holder\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    void reset(int* external)\n"
        "    {\n"
        "        values = external;\n"
        "    }\n"
        "};\n",
        context,
        changes);

    require(contains(fixed, "values = external;"), "ambiguous external raw pointer assignment should not be silently changed");
}

void testVectorEmulationEliminatesFieldAppendGrowth()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct Entry\n"
        "{\n"
        "    int id;\n"
        "    int weight;\n"
        "};\n\n"
        "class EntryList\n"
        "{\n"
        "public:\n"
        "    EntryList()\n"
        "        : count(0), capacity(2), entries(nullptr)\n"
        "    {\n"
        "        entries = new Entry[capacity];\n"
        "    }\n\n"
        "    void add(int id, int weight)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            int nextCapacity = capacity * 2;\n"
        "            Entry* replacement = new Entry[nextCapacity];\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                replacement[index] = entries[index];\n"
        "            }\n"
        "            delete[] entries;\n"
        "            entries = replacement;\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "        entries[count].id = id;\n"
        "        entries[count].weight = weight;\n"
        "        count++;\n"
        "    }\n\n"
        "    std::size_t size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "    ~EntryList()\n"
        "    {\n"
        "        delete[] entries;\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    int capacity;\n"
        "    Entry* entries;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<Entry> entries;"), "entry storage should become vector");
    require(contains(result.modernCode, "Entry appendedItem{};"), "field append should create a local value object");
    require(contains(result.modernCode, "appendedItem.id = id;"), "field assignment should target local value");
    require(contains(result.modernCode, "appendedItem.weight = weight;"), "second field assignment should target local value");
    require(contains(result.modernCode, "entries.push_back(appendedItem);"), "field append should become push_back");
    require(contains(result.modernCode, "return entries.size();"), "count mirror getter should become vector size");
    require(!contains(result.modernCode, "replacement"), "removed temp buffer should have no remaining references");
    require(!contains(result.modernCode, "nextCapacity"), "capacity growth temporary should be removed");
    require(!contains(result.modernCode, "entries[count]"), "out-of-bounds indexed vector append should not remain");
    require(!contains(result.modernCode, "count++"), "manual count increment should be removed");
    require(!contains(result.modernCode, "if (count == capacity)"), "manual growth condition should be removed");
    require(!contains(result.modernCode, "delete[] entries"), "delete[] for vector storage should not remain");
    require(hasAppliedRule(result, "Vector emulation elimination"), "manual vector emulation removal should be tracked");
    require(hasAppliedRule(result, "Vector paradigm rewrite"), "vector paradigm rewrite should be tracked");
    require(hasAppliedRule(result, "Indexed append to vector push_back"), "field append push_back conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "field append vector cleanup sample should pass syntax verification");
    }
}

void testVectorModernizationRemovesCleanupOnlyCopySpecialMembers()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "class CopyBuffer\n"
        "{\n"
        "public:\n"
        "    explicit CopyBuffer(int initialCount)\n"
        "        : count(initialCount), values(nullptr)\n"
        "    {\n"
        "        values = new int[count];\n"
        "    }\n\n"
        "    CopyBuffer(const CopyBuffer& other)\n"
        "        : count(other.count), values(nullptr)\n"
        "    {\n"
        "        values = new int[other.count];\n"
        "        for (int index = 0; index < count; ++index)\n"
        "        {\n"
        "            values[index] = other.values[index];\n"
        "        }\n"
        "    }\n\n"
        "    CopyBuffer& operator=(const CopyBuffer& other)\n"
        "    {\n"
        "        if (this != &other)\n"
        "        {\n"
        "            delete[] values;\n"
        "            count = other.count;\n"
        "            values = new int[count];\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                values[index] = other.values[index];\n"
        "            }\n"
        "        }\n"
        "        return *this;\n"
        "    }\n\n"
        "    ~CopyBuffer()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    int* values;\n"
        "};\n",
        structuralOptions());

    require(contains(result.modernCode, "std::vector<int> values;"), "copy buffer storage should become vector");
    require(!contains(result.modernCode, "values[index] = other.values[index]"), "manual copy loop should be removed");
    require(!contains(result.modernCode, "delete[] values"), "manual vector cleanup should be removed");
    require(!contains(result.modernCode, "~CopyBuffer()"), "cleanup-only destructor should be removed");
    require(hasAppliedRule(result, "Rule of Zero copy constructor removal"), "copy constructor cleanup should be tracked");
    require(hasAppliedRule(result, "Rule of Zero assignment operator removal"), "assignment cleanup should be tracked");
    require(hasAppliedRule(result, "Rule of Zero destructor removal"), "destructor cleanup should be tracked");
}

void testVectorAppendPreservesLogicalMaxCapacity()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "class LimitedStore\n"
        "{\n"
        "public:\n"
        "    explicit LimitedStore(std::size_t maximum)\n"
        "        : count(0), maxCount(maximum), values(nullptr)\n"
        "    {\n"
        "        values = new int[maxCount];\n"
        "    }\n\n"
        "    bool add(int value)\n"
        "    {\n"
        "        if (count >= maxCount)\n"
        "        {\n"
        "            return false;\n"
        "        }\n"
        "        values[count] = value;\n"
        "        ++count;\n"
        "        return true;\n"
        "    }\n\n"
        "    std::size_t size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "    ~LimitedStore()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "private:\n"
        "    std::size_t count;\n"
        "    std::size_t maxCount;\n"
        "    int* values;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"), "limited storage should become vector");
    require(contains(result.modernCode, "values.size() >= static_cast<std::size_t>(maxCount)"),
            "logical maximum capacity check should be preserved with a safe vector size comparison");
    require(contains(result.modernCode, "values.push_back(value);"), "append should become push_back");
    require(contains(result.modernCode, "return values.size();"), "count mirror getter should use vector size");
    require(!contains(result.modernCode, "values[count]"), "append-style indexed vector write should not remain");
    require(!contains(result.modernCode, "++count"), "manual count increment should be removed");
    require(!contains(result.modernCode, "delete[] values"), "manual delete should be removed");
    require(hasAppliedRule(result, "Vector append method rewrite") || hasAppliedRule(result, "Indexed append to vector push_back"),
            "append method rewrite should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "logical max vector append sample should pass syntax verification");
    }
}

void testVectorAppendPassConvertsReserveToResizeForFixedIndexWrites()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Buffer",
        true,
        "Raw dynamic array to std::vector",
        {"fix reserve/index writes"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const VectorAppendMethodRewritePass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Buffer\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    void initialize(int count)\n"
        "    {\n"
        "        values.reserve(count);\n"
        "        values[0] = 1;\n"
        "    }\n"
        "};\n",
        context,
        changes);

    require(contains(fixed, "values.resize(count);"), "fixed indexed writes after reserve should become resize");
    require(!contains(fixed, "values.reserve(count);"), "reserve-only initialization should not remain before fixed index writes");
    require(hasAppliedRule(changes, "Reserve vs resize safety fix"), "reserve-to-resize safety fix should be tracked");
}

void testVectorEmulationRemovesOrphanedTempGrowthReferences()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Buffer",
        true,
        "Raw dynamic array to std::vector",
        {"remove orphan temp growth buffer"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const VectorEmulationEliminationPass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Buffer\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    void grow(int count)\n"
        "    {\n"
        "        if (count > 4)\n"
        "        {\n"
        "            for (int i = 0; i < count; ++i)\n"
        "            {\n"
        "                expanded[i] = values[i];\n"
        "            }\n"
        "            values = expanded;\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "    }\n"
        "    int capacity;\n"
        "};\n",
        context,
        changes);

    require(!contains(fixed, "expanded"), "orphaned temp growth variable references should be removed");
    require(!contains(fixed, "nextCapacity"), "orphaned temp capacity variable references should be removed");
    require(!contains(fixed, "values = expanded"), "raw pointer assignment to vector should be removed");
    require(!contains(fixed, "capacity = nextCapacity"), "stale allocation-capacity update should be removed");
    require(!contains(fixed, "expanded[i] = values[i]"), "orphaned copy loop should be removed");
    require(hasAppliedRule(changes, "Vector emulation elimination"), "orphaned growth block removal should be tracked");
}

void testOrphanedGrowthSymbolCleanupUsesCompilerDiagnostics()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Buffer",
        true,
        "Raw dynamic array to std::vector",
        {"remove orphan growth symbols after diagnostics"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const OrphanedGrowthSymbolCleanupPass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Buffer\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    int capacity;\n"
        "    void grow(int count)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            for (int i = 0; i < count; ++i)\n"
        "            {\n"
        "                scratch[i] = values[i];\n"
        "            }\n"
        "            values = scratch;\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "        capacity = capacity;\n"
        "    }\n"
        "};\n",
        context,
        "error: use of undeclared identifier 'scratch'\nerror: use of undeclared identifier 'nextCapacity'\nwarning: explicitly assigning value of variable of type 'int' to itself",
        changes);

    require(!contains(fixed, "scratch"), "diagnostic cleanup should remove orphaned temp buffer references");
    require(!contains(fixed, "nextCapacity"), "diagnostic cleanup should remove orphaned temp capacity references");
    require(!contains(fixed, "capacity = capacity"), "diagnostic cleanup should remove self-assignment fallout");
    require(!contains(fixed, "for (int i = 0"), "diagnostic cleanup should remove orphaned copy loop");
    require(hasAppliedRule(changes, "Orphaned growth symbol cleanup"), "orphaned diagnostic cleanup should be tracked");
}

void testOrphanedGrowthCleanupRemovesPartiallyCleanedCapacityFallout()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "records",
        "Record*",
        "std::vector<Record>",
        "RecordStore",
        true,
        "Raw dynamic array to std::vector",
        {"remove partially cleaned growth block"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const OrphanedGrowthSymbolCleanupPass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Record { int id; };\n"
        "struct RecordStore\n"
        "{\n"
        "    std::vector<Record> records;\n"
        "    int capacity;\n"
        "    void grow(int count)\n"
        "    {\n"
        "        if (count >= capacity)\n"
        "        {\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                expanded[index] = records[index];\n"
        "            }\n"
        "            capacity = expandedCapacity;\n"
        "        }\n"
        "    }\n"
        "};\n",
        context,
        {},
        changes);

    require(!contains(fixed, "expanded"), "partially cleaned temp buffer references should be removed");
    require(!contains(fixed, "expandedCapacity"), "partially cleaned temp capacity reference should be removed");
    require(!contains(fixed, "capacity = expandedCapacity"), "stale capacity update should be removed with the growth block");
    require(!contains(fixed, "for (int index = 0"), "orphaned copy loop should be removed with the growth block");
    require(hasAppliedRule(changes, "Orphaned growth symbol cleanup"), "partial growth fallout cleanup should be tracked");
}

void testOrphanedTempBufferLoopCleanupRemovesFullLoop()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "Buffer",
        true,
        "Raw dynamic array to std::vector",
        {"remove orphan temp buffer copy loop"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const OrphanedTempBufferLoopCleanupPass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Buffer\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    int capacity;\n"
        "    void append(int value, int count)\n"
        "    {\n"
        "        for (int index = 0; index < count; ++index)\n"
        "        {\n"
        "            temporary[index] = values[index];\n"
        "        }\n"
        "        capacity = expandedCapacity;\n"
        "        values.push_back(value);\n"
        "    }\n"
        "};\n",
        context,
        "error: use of undeclared identifier 'temporary'\nerror: use of undeclared identifier 'expandedCapacity'\n",
        changes);

    require(!contains(fixed, "temporary"), "orphan temp buffer loop cleanup should remove all temp buffer references");
    require(!contains(fixed, "expandedCapacity"), "orphan temp buffer loop cleanup should remove temp capacity references");
    require(!contains(fixed, "for (int index = 0"), "orphan temp buffer loop cleanup should remove the full copy loop block");
    require(contains(fixed, "values.push_back(value);"), "orphan temp buffer loop cleanup should preserve vector-native append");
    require(hasAppliedRule(changes, "Orphaned temp buffer loop cleanup"), "orphan temp loop cleanup should be tracked");
}

void testCompilerDiagnosticCleanupRemovesOrphanedTempBufferLoop()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "Record*",
        "std::vector<Record>",
        "RecordStore",
        true,
        "Raw dynamic array to std::vector",
        {"remove orphan temp buffer copy loop", "rewrite indexed append"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const CompilerDiagnosticCleanupPass pass;
    const std::string fixed = pass.run(
        "#include <vector>\n"
        "struct Record { int id; int weight; };\n"
        "struct RecordStore\n"
        "{\n"
        "    std::vector<Record> items;\n"
        "    int capacity;\n"
        "    void add(int id, int weight, int count)\n"
        "    {\n"
        "        if (count >= capacity)\n"
        "        {\n"
        "            for (auto index = 0; index != count; ++index)\n"
        "            {\n"
        "                grown[index] = items[index];\n"
        "            }\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "        items[count].id = id;\n"
        "        items[count].weight = weight;\n"
        "        ++count;\n"
        "    }\n"
        "};\n",
        context,
        "error: use of undeclared identifier 'grown'\nerror: use of undeclared identifier 'nextCapacity'\n",
        changes);

    require(!contains(fixed, "grown"), "compiler diagnostic cleanup should remove orphan temp buffer references");
    require(!contains(fixed, "nextCapacity"), "compiler diagnostic cleanup should remove orphan temp capacity references");
    require(!contains(fixed, "items[count]"), "compiler diagnostic cleanup should finalize vector append");
    require(contains(fixed, "items.push_back("), "compiler diagnostic cleanup should rewrite append to push_back");
    require(hasAppliedRule(changes, "Orphaned temp buffer loop cleanup"), "diagnostic temp loop cleanup should be tracked");
    require(hasAppliedRule(changes, "Compiler diagnostic cleanup"), "compiler diagnostic cleanup summary should be tracked");
}

void testConverterRemovesManualGrowthFragmentsAfterVectorModernization()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct EntryRecord\n"
        "{\n"
        "    int id;\n"
        "    int length;\n"
        "};\n\n"
        "class EntryStore\n"
        "{\n"
        "public:\n"
        "    EntryStore()\n"
        "    {\n"
        "        count = 0;\n"
        "        capacity = 2;\n"
        "        entries = new EntryRecord[capacity];\n"
        "    }\n\n"
        "    EntryStore(const EntryStore& other)\n"
        "    {\n"
        "        count = other.count;\n"
        "        capacity = other.capacity;\n"
        "        entries = new EntryRecord[capacity];\n"
        "        for (int index = 0; index < count; ++index)\n"
        "        {\n"
        "            entries[index] = other.entries[index];\n"
        "        }\n"
        "    }\n\n"
        "    ~EntryStore()\n"
        "    {\n"
        "        if (entries != NULL)\n"
        "        {\n"
        "            delete[] entries;\n"
        "            entries = NULL;\n"
        "        }\n"
        "    }\n\n"
        "    bool add(int id, int length)\n"
        "    {\n"
        "        if (count >= capacity)\n"
        "        {\n"
        "            int expandedCapacity = capacity * 2;\n"
        "            EntryRecord* expandedEntries = new EntryRecord[expandedCapacity];\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                expandedEntries[index] = entries[index];\n"
        "            }\n"
        "            delete[] entries;\n"
        "            entries = expandedEntries;\n"
        "            capacity = expandedCapacity;\n"
        "        }\n"
        "        entries[count].id = id;\n"
        "        entries[count].length = length;\n"
        "        count++;\n"
        "        return true;\n"
        "    }\n\n"
        "    int size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "private:\n"
        "    EntryRecord* entries;\n"
        "    int count;\n"
        "    int capacity;\n"
        "};\n\n"
        "int main()\n"
        "{\n"
        "    EntryStore store;\n"
        "    store.add(1, 2);\n"
        "    return store.size();\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::vector<EntryRecord> entries;"), "raw entry storage should become std::vector");
    if (!contains(result.modernCode, "entries.push_back(")) {
        std::cerr << "Converted manual growth regression code without push_back:\n" << result.modernCode << '\n';
    }
    require(contains(result.modernCode, "entries.push_back("), "indexed append should become vector push_back");
    require(!contains(result.modernCode, "expandedEntries"), "temporary growth buffer should not remain after conversion");
    require(!contains(result.modernCode, "expandedCapacity"), "temporary capacity variable should not remain after conversion");
    require(!contains(result.modernCode, "capacity = expandedCapacity"), "stale capacity update should not remain");
    require(!contains(result.modernCode, "delete[] entries"), "delete[] for vector storage should not remain");
    require(!contains(result.modernCode, "entries = expandedEntries"), "raw buffer assignment to vector should not remain");
    require(!contains(result.modernCode, "entries[count]"), "append-style vector indexing should not remain");
    require(hasAppliedRule(result, "Vector paradigm rewrite"), "full converter should record vector paradigm cleanup");
    if (!result.compilerUsed.empty()) {
        if (!result.compileVerificationPassed) {
            std::cerr << "Converted manual growth regression code:\n" << result.modernCode
                      << "\nCompiler output:\n" << result.compilerOutput << '\n';
        }
        require(result.compileVerificationPassed, "manual growth regression sample should pass syntax verification");
    }
}

void testVectorParadigmRewriteComposesGrowthAndAppendPasses()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "int*",
        "std::vector<int>",
        "Collection",
        true,
        "Raw dynamic array to std::vector",
        {"remove manual growth", "rewrite append"},
        {},
        false,
    });

    std::vector<ConversionChange> changes;
    const VectorParadigmRewritePass pass;
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "struct Collection\n"
        "{\n"
        "    std::vector<int> items;\n"
        "    void add(int value, int count, int capacity)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            int nextCapacity = capacity * 2;\n"
        "            int* grown = new int[nextCapacity];\n"
        "            for (int i = 0; i < count; ++i)\n"
        "            {\n"
        "                grown[i] = items[i];\n"
        "            }\n"
        "            delete[] items;\n"
        "            items = grown;\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "        items[count] = value;\n"
        "        ++count;\n"
        "    }\n"
        "};\n",
        context,
        changes);

    require(contains(fixed, "items.push_back(value);"), "paradigm pass should rewrite append to push_back");
    require(!contains(fixed, "grown"), "paradigm pass should remove temp growth buffer");
    require(!contains(fixed, "nextCapacity"), "paradigm pass should remove temp capacity variable");
    require(!contains(fixed, "items[count]"), "paradigm pass should remove append-style indexing");
    require(!contains(fixed, "delete[] items"), "paradigm pass should remove delete[] on vector storage");
    require(hasAppliedRule(changes, "Vector paradigm rewrite"), "paradigm pass should record summary change");
}

void testUnscopedEnumModernizesWhenSafe()
{
    const RuleBasedConverterEngine converter;
    ConversionResult result = converter.convert(
        "enum Status { Ready, Busy };\n"
        "Status current()\n"
        "{\n"
        "    Status state = Ready;\n"
        "    if (state == Busy)\n"
        "    {\n"
        "        return Busy;\n"
        "    }\n"
        "    return state;\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "enum class Status"), "safe unscoped enum should become enum class");
    require(contains(result.modernCode, "Status state = Status::Ready;"), "enum assignment should be scoped");
    require(contains(result.modernCode, "state == Status::Busy"), "enum comparison should be scoped");
    require(hasAppliedRule(result, "Unscoped enum to enum class"), "enum class conversion should be tracked");
}

void testUnscopedEnumSuggestionWhenIntegerConversionIsUsed()
{
    const RuleBasedConverterEngine converter;
    ConversionResult result = converter.convert(
        "enum Code { Ok, Failed };\n"
        "int value()\n"
        "{\n"
        "    int code = Ok;\n"
        "    return code;\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "enum Code"), "unsafe enum integer conversion should not be rewritten");
    require(hasSuggestionRule(result, "Unscoped enum to enum class"), "unsafe enum should emit suggestion");
}

void testStdFormatConversionIsOptional()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions disabled = structuralOptions();
    disabled.targetStandard = CppStandard::Cpp20;
    ConversionResult unchanged = converter.convert(
        "#include <iostream>\n"
        "void print(int value)\n"
        "{\n"
        "    std::cout << \"Value: \" << value << std::endl;\n"
        "}\n",
        disabled);

    require(!contains(unchanged.modernCode, "std::format"), "std::format conversion should be disabled by default");

    ModernizationOptions enabled = disabled;
    enabled.useStdFormatForStreams = true;
    ConversionResult converted = converter.convert(
        "#include <iostream>\n"
        "void print(int value)\n"
        "{\n"
        "    std::cout << \"Value: \" << value << std::endl;\n"
        "}\n",
        enabled);

    require(contains(converted.modernCode, "#include <format>"), "format conversion should add format include");
    require(contains(converted.modernCode, "std::format(\"Value: {}\\n\", value)"), "simple stream output should become std::format");
    require(hasAppliedRule(converted, "Stream formatting to std::format"), "format conversion should be tracked");
}

void testSafeReplacementDoesNotInjectGeneratedCodeIntoComments()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "void build(int count)\n"
        "{\n"
        "    // int* skipped = new int[count];\n"
        "    int* values = new int[count]; // allocate storage\n"
        "    values[0] = 3;\n"
        "    delete[] values;\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "// int* skipped = new int[count];"), "commented legacy allocation should remain a comment");
    require(contains(result.modernCode, "// allocate storage\n    std::vector<int> values(count);"),
            "trailing comment should be separated from generated vector declaration");
    require(!contains(result.modernCode, "// allocate storage std::vector"), "generated code should not be injected into a line comment");
}

void testAutoIteratorLoopModernizesStructurally()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(std::vector<int>& values)\n"
        "{\n"
        "    for (auto it = values.cbegin(); it != values.cend(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "for (const auto& value : values)"), "auto iterator loop should become range-based loop");
    require(contains(result.modernCode, "std::cout << value << '\\n';"), "all *it usages should be replaced");
    require(!contains(result.modernCode, "*it"), "dereferenced iterator should not remain after conversion");
    require(hasAppliedRule(result, "Explicit iterator loop to range-based for"), "auto iterator loop conversion should be tracked");
}

void testAiStyleLocalComputationBlockToLambda()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "bool isPalindrome(long long x)\n"
        "{\n"
        "    long long original{x};\n"
        "    long long temp{0};\n"
        "    while (x > 0)\n"
        "    {\n"
        "        temp = temp * 10 + x % 10;\n"
        "        x /= 10;\n"
        "    }\n"
        "    return original == temp;\n"
        "}\n",
        aiStyleOptions());

    require(contains(result.modernCode, "const auto computeTemp"), "AI-style mode should extract local computation logic to lambda");
    require(contains(result.modernCode, "return x == computeTemp(x);"), "AI-style lambda should be used in final comparison");
    require(result.rewriteLevel == "Offline Aggressive AI-like Rewrite", "AI-style mode should report rewrite level");
    for (const ConversionChange& change : result.changes) {
        require(!contains(change.ruleName, "reverse") && !contains(change.ruleName, "palindrome"),
                "regression: computation modernization must not use dedicated reverse/palindrome rule names");
    }
}

void testAiStyleHelperFunctionUsedOnceToLocalLambda()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "bool isEven(int value)\n"
        "{\n"
        "    return value % 2 == 0;\n"
        "}\n\n"
        "void printEvenNumbers(const std::vector<int>& values)\n"
        "{\n"
        "    for (int value : values)\n"
        "    {\n"
        "        if (isEven(value))\n"
        "        {\n"
        "            std::cout << value << std::endl;\n"
        "        }\n"
        "    }\n"
        "}\n",
        aiStyleOptions());

    require(!contains(result.modernCode, "bool isEven(int value)\n{"), "helper function should be moved into local context");
    require(contains(result.modernCode, "const auto isEven = [](int value)"), "helper should become local lambda");
    require(contains(result.modernCode, "std::cout << value << '\\n';"), "AI-style rewrite should prefer newline character over std::endl");
}

void testAiStyleFunctorPredicateToLambda()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct IsPositive\n"
        "{\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        return value > 0;\n"
        "    }\n"
        "};\n"
        "void countValues(std::vector<int>& values)\n"
        "{\n"
        "    std::count_if(values.begin(), values.end(), IsPositive());\n"
        "}\n",
        aiStyleOptions());

    require(!contains(result.modernCode, "struct IsPositive"), "predicate functor should be removed when safely inlined");
    require(contains(result.modernCode, "std::ranges::count_if(values"), "C++20 AI-style mode should use ranges count_if");
    require(contains(result.modernCode, "[](int value)"), "predicate should become lambda");
}

void testAiStyleIteratorLoopToRangesAlgorithm()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n",
        aiStyleOptions());

    require(contains(result.modernCode, "std::ranges::for_each(values"), "C++20 AI-style mode should use ranges for_each");
    require(contains(result.modernCode, "#include <ranges>"), "ranges algorithm should add ranges include");
    require(contains(result.modernCode, "#include <algorithm>"), "algorithm modernization should add algorithm include");
}

void testAiStyleIteratorLoopToStdForEachForCpp17()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n",
        aiStyleOptions(CppStandard::Cpp17));

    require(contains(result.modernCode, "std::for_each(values.begin(), values.end()"), "C++17 AI-style mode should use std::for_each");
    require(!contains(result.modernCode, "std::ranges::"), "C++17 AI-style mode should not use ranges");
}

void testAiStyleMixedSampleAggressivelyModernizes()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = aiStyleOptions();
    const ConversionResult result = converter.convert(readTextFile("tests/samples/legacy_ai_style_aggressive.cpp"), options);

    require(contains(result.modernCode, "const auto computeTemp"), "AI-style sample should contain extracted computation lambda");
    require(contains(result.modernCode, "const auto isEven = [](int value)"), "AI-style sample should contain local helper lambda");
    require(contains(result.modernCode, "std::make_unique"), "AI-style sample should preserve ownership modernization");
    require(contains(result.modernCode, "std::string name = input;"), "AI-style sample should preserve string modernization");
    require(contains(result.modernCode, "std::ranges::"), "AI-style sample should use ranges where safe");
    require(contains(result.modernCode, "#include <algorithm>"), "AI-style sample should add algorithm include");
    require(contains(result.modernCode, "#include <ranges>"), "AI-style sample should add ranges include");
    require(!result.changes.empty(), "AI-style sample should record applied changes");
}

void testAiStyleCompileVerificationRunsAndAutoFixesIncludes()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = aiStyleOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "void print(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "}\n",
        options);

    require(result.compileVerificationEnabled, "AI-style mode should run compile verification");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationAutoFixAttempted, "AI-style mode should attempt include auto-fix after missing include compile failure");
        require(contains(result.modernCode, "#include <iostream>"), "include auto-fix should add iostream for std::cout");
    }
}

void testAiStyleUnsafePatternRemainsSuggestionOnly()
{
    const RuleBasedConverterEngine converter;
    const std::string legacy =
        "void process(int* values, int count)\n"
        "{\n"
        "    for (int i = 0; i < count; ++i)\n"
        "    {\n"
        "        values[i] = values[i] + i;\n"
        "    }\n"
        "}\n";
    const ConversionResult result = converter.convert(legacy, aiStyleOptions());

    require(result.modernCode == legacy, "unsafe side-effect loop should remain unchanged");
    require(std::any_of(result.changes.begin(), result.changes.end(), [](const ConversionChange& change) {
        return !change.applied && contains(change.ruleName, "AI-style aggressive rewrite");
    }), "unsafe AI-style pattern should produce suggestion");
}

void testAiStyleDiverseSamplesUseGeneralPipeline()
{
    const RuleBasedConverterEngine converter;
    const std::vector<std::filesystem::path> samples{
        "tests/samples/legacy_memory.cpp",
        "tests/samples/legacy_strings.cpp",
        "tests/samples/legacy_loops.cpp",
        "tests/samples/legacy_functor_to_lambda.cpp",
        "tests/samples/legacy_helper_function_to_lambda.cpp",
        "tests/samples/legacy_callbacks.cpp",
        "tests/samples/legacy_stl_algorithms.cpp",
        "tests/samples/legacy_mixed_repository_sample.cpp",
    };

    for (const std::filesystem::path& sample : samples) {
        ModernizationOptions options = aiStyleOptions();
        options.compileVerificationEnabled = true;
        const ConversionResult result = converter.convert(readTextFile(sample), options);

        require(!result.changes.empty(), "aggressive sample should produce tracked changes: " + sample.string());
        require(result.compileVerificationEnabled, "aggressive sample should run compile verification: " + sample.string());
        const bool hasApplied = std::any_of(result.changes.begin(), result.changes.end(), [](const ConversionChange& change) {
            return change.applied;
        });
        require(hasApplied, "aggressive sample should contain applied changes, not only suggestions: " + sample.string());
    }
}

void testAiStyleClassMemberOwnershipModernizesToUniquePtr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = aiStyleOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "class Resource\n"
        "{\n"
        "public:\n"
        "    void use() {}\n"
        "};\n\n"
        "class ManagedOwner\n"
        "{\n"
        "public:\n"
        "    ManagedOwner()\n"
        "    {\n"
        "        resource = new Resource();\n"
        "    }\n\n"
        "    ~ManagedOwner()\n"
        "    {\n"
        "        delete resource;\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::unique_ptr<Resource> resource;"), "owning class member should become unique_ptr");
    require(contains(result.modernCode, "resource = std::make_unique<Resource>();"), "constructor should use make_unique for owned member");
    require(!contains(result.modernCode, "~ManagedOwner()"), "cleanup-only destructor should be removed");
    require(!contains(result.modernCode, "delete resource;"), "manual member delete should be removed");
    require(contains(result.modernCode, "#include <memory>"), "class member ownership conversion should add memory include");
    require(hasAppliedRule(result, "Class member raw pointer to std::unique_ptr"), "class member conversion should be tracked");
    require(hasAppliedRule(result, "Constructor allocation to std::make_unique"), "constructor allocation conversion should be tracked");
    require(hasAppliedRule(result, "Rule of Zero destructor removal"), "cleanup-only destructor removal should be tracked");
    require(result.compileVerificationEnabled, "class member ownership test should run compile verification");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "converted class ownership sample should pass syntax verification");
    }
}

void testAiStyleDestructorWithExtraLogicKeepsDestructor()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "class Resource {};\n\n"
        "class ManagedOwner\n"
        "{\n"
        "public:\n"
        "    ManagedOwner()\n"
        "    {\n"
        "        resource = new Resource();\n"
        "    }\n\n"
        "    ~ManagedOwner()\n"
        "    {\n"
        "        std::cout << \"cleanup\\n\";\n"
        "        delete resource;\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        aiStyleOptions());

    require(contains(result.modernCode, "~ManagedOwner()"), "destructor with extra logic should be kept");
    require(contains(result.modernCode, "std::cout << \"cleanup\\n\";"), "non-cleanup destructor logic should remain");
    require(!contains(result.modernCode, "delete resource;"), "redundant delete should be removed from destructor with extra logic");
    require(contains(result.modernCode, "std::unique_ptr<Resource> resource;"), "owned member should still become unique_ptr");
    require(hasAppliedRule(result, "Remove redundant manual delete"), "delete removal should be tracked");
    require(hasSuggestionRule(result, "Rule of Zero destructor removal"), "remaining destructor should produce Rule of Zero review suggestion");
}

void testAiStyleBorrowedRawPointerMemberRemainsSuggestionOnly()
{
    const RuleBasedConverterEngine converter;
    const std::string legacy =
        "class Resource {};\n\n"
        "class BorrowedView\n"
        "{\n"
        "public:\n"
        "    explicit BorrowedView(Resource* resource)\n"
        "        : resource(resource)\n"
        "    {\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n";

    const ConversionResult result = converter.convert(legacy, aiStyleOptions());

    require(contains(result.modernCode, "Resource* resource;"), "borrowed member should remain raw pointer when ownership is unclear");
    require(!contains(result.modernCode, "std::unique_ptr<Resource> resource;"), "borrowed member must not be converted automatically");
    require(hasSuggestionRule(result, "Class member raw pointer to std::unique_ptr"), "borrowed member should produce ownership review suggestion");
}

void testAiStyleAliasedPointerOwnershipModernizesToSharedPtr()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class Resource\n"
        "{\n"
        "public:\n"
        "    explicit Resource(int) {}\n"
        "    void use() {}\n"
        "};\n\n"
        "void run()\n"
        "{\n"
        "    Resource* owner = new Resource(7);\n"
        "    Resource* alias = owner;\n"
        "    alias->use();\n"
        "    delete owner;\n"
        "}\n",
        aiStyleOptions());

    require(contains(result.modernCode, "auto owner = std::make_shared<Resource>(7);"), "shared alias allocation should use make_shared");
    require(contains(result.modernCode, "auto alias = owner;"), "alias should copy the shared_ptr");
    require(!contains(result.modernCode, "delete owner;"), "manual delete should be removed for shared ownership");
    require(contains(result.modernCode, "#include <memory>"), "shared ownership conversion should add memory include");
    require(hasAppliedRule(result, "Aliased raw pointer ownership to std::shared_ptr"), "shared ownership conversion should be tracked");
    require(hasAppliedRule(result, "Remove redundant manual delete"), "shared ownership delete removal should be tracked");
}

void testAiStyleAmbiguousAliasingRemainsSuggestionOnly()
{
    const RuleBasedConverterEngine converter;
    const std::string legacy =
        "class Resource {};\n\n"
        "Resource* leakAlias()\n"
        "{\n"
        "    Resource* owner = new Resource();\n"
        "    Resource* alias = owner;\n"
        "    return alias;\n"
        "}\n";

    const ConversionResult result = converter.convert(legacy, aiStyleOptions());

    require(contains(result.modernCode, "Resource* owner = new Resource();"), "ambiguous aliased owner should remain unchanged");
    require(contains(result.modernCode, "Resource* alias = owner;"), "ambiguous alias should remain unchanged");
    require(!contains(result.modernCode, "std::make_shared<Resource>"), "ambiguous aliasing should not be converted automatically");
    require(hasSuggestionRule(result, "Dangling pointer risk detected"), "ambiguous aliasing should produce dangling-risk warning");
}

void testAiStyleStringViewParameterExplicitlyOwnsStringMember()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class TextRecord\n"
        "{\n"
        "public:\n"
        "    explicit TextRecord(std::string_view label)\n"
        "        : label(label)\n"
        "    {\n"
        "    }\n\n"
        "private:\n"
        "    std::string label;\n"
        "};\n",
        aiStyleOptions());

    require(contains(result.modernCode, "label(std::string{label})"), "string member should explicitly own string_view input");
    require(contains(result.modernCode, "#include <string>"), "owned string conversion should add string include");
    require(contains(result.modernCode, "#include <string_view>"), "string_view parameter should add string_view include");
    require(!contains(result.modernCode, "std::string_view label;"), "owning class text member should not become string_view");
    require(hasAppliedRule(result, "Explicit string ownership from string_view"), "explicit string ownership conversion should be tracked");
}

void testRepositoryModeUsesImprovedOfflinePipeline()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_deep_ownership");
    writeTextFile(root / "src" / "owner.cpp",
        "class Resource {};\n\n"
        "class ManagedOwner\n"
        "{\n"
        "public:\n"
        "    ManagedOwner()\n"
        "    {\n"
        "        resource = new Resource();\n"
        "    }\n\n"
        "    ~ManagedOwner()\n"
        "    {\n"
        "        delete resource;\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/deep-ownership";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::AiStyleAggressiveRewrite;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "owner.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository deep ownership test should scan one source file");
    require(result.filesModified == 1, "repository deep ownership test should modify the source file");
    require(contains(modernized, "std::unique_ptr<Resource> resource;"), "repository mode should use class member ownership modernization");
    require(!contains(modernized, "delete resource;"), "repository mode should remove redundant member delete");
    require(contains(report, "Class member raw pointer to std::unique_ptr"), "repository report should include class member ownership change");
    require(std::filesystem::exists(root / "src" / "owner.cpp.legacy_backup"), "repository mode should create backup before modification");
}

void testRepositoryModeUsesStructuralModernizationPipeline()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_structural_pipeline");
    writeTextFile(root / "src" / "legacy.cpp",
        "#include <cstring>\n"
        "#ifndef nullptr\n"
        "#define nullptr NULL\n"
        "#endif\n"
        "#define BUFFER_LIMIT 32\n"
        "typedef struct\n"
        "{\n"
        "    int id;\n"
        "} Record;\n\n"
        "class TextStore\n"
        "{\n"
        "public:\n"
        "    void assign(const char* input)\n"
        "    {\n"
        "        std::strcpy(name, input);\n"
        "    }\n\n"
        "private:\n"
        "    char name[32];\n"
        "};\n\n"
        "class NumberStore\n"
        "{\n"
        "public:\n"
        "    explicit NumberStore(int count)\n"
        "        : count(count), values(nullptr)\n"
        "    {\n"
        "        values = new int[count];\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            values[0] = 0;\n"
        "        }\n"
        "    }\n\n"
        "    void append(int value)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            int newCapacity = capacity * 2;\n"
        "            int* grown = new int[newCapacity];\n"
        "            for (auto i = 0; i != count; ++i)\n"
        "            {\n"
        "                grown[i] = values[i];\n"
        "            }\n"
        "            delete[] values;\n"
        "            values = grown;\n"
        "            capacity = newCapacity;\n"
        "        }\n"
        "        values[count] = value;\n"
        "        ++count;\n"
        "    }\n\n"
        "    ~NumberStore()\n"
        "    {\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            delete[] values;\n"
        "        }\n"
        "        values = nullptr;\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    int capacity;\n"
        "    int* values;\n"
        "};\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/structural-modernization";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = true;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "legacy.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository structural pipeline should scan fixture file");
    require(result.filesModified == 1, "repository structural pipeline should modify fixture file");
    require(contains(modernized, "inline constexpr auto BUFFER_LIMIT = 32;"), "repository mode should convert constant macros");
    require(contains(modernized, "struct Record"), "repository mode should modernize typedef struct");
    require(contains(modernized, "std::string name;"), "repository mode should modernize char buffer members");
    require(contains(modernized, "std::vector<int> values;"), "repository mode should modernize dynamic array members");
    require(!contains(modernized, "new int["), "repository dependency cleanup should remove new[] leftovers");
    require(!contains(modernized, "delete[] values"), "repository dependency cleanup should remove delete[] leftovers");
    require(!contains(modernized, "grown"), "repository orphan temp-buffer cleanup should remove growth temp references");
    require(!contains(modernized, "newCapacity"), "repository orphan temp-buffer cleanup should remove temp capacity references");
    require(!contains(modernized, "values = grown"), "repository growth cleanup should remove raw pointer assignment");
    require(contains(modernized, "values.push_back(value);"), "repository growth cleanup should use push_back");
    require(!contains(modernized, "values != nullptr"), "repository dependency cleanup should remove vector null checks");
    require(!contains(modernized, "#ifndef nullptr"), "repository mode should remove obsolete preprocessor blocks");
    require(contains(report, "Constant macro to constexpr"), "repository report should include macro conversion");
    require(contains(report, "C-style typedef struct to C++ struct"), "repository report should include typedef struct conversion");
    require(contains(report, "Char buffer member to std::string"), "repository report should include char buffer conversion");
    require(contains(report, "Value-type pointer operation scanner"), "repository report should include value-type pointer cleanup scanner");
    require(contains(report, "Vector growth emulation cleanup"), "repository report should include vector growth cleanup");
    require(contains(report, "Vector cascade cleanup"), "repository report should include dependent vector cleanup");
    require(std::filesystem::exists(root / "src" / "legacy.cpp.legacy_backup"), "repository structural pipeline should create backup");
}

void testOfflineModeStillWorks()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult result = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::OfflineRuleBased);

    require(contains(result.result.modernCode, "nullptr"), "offline mode should still use local converter");
    require(!backendRaw->convertCalled, "offline mode should not call backend");
    require(result.effectiveMode == ConversionMode::OfflineRuleBased, "offline mode should remain effective mode");
    require(result.result.conversionSource == "Offline Rule-Based", "offline mode should stamp conversion source");
    require(result.result.backendStatus == "Not used", "offline mode should stamp backend status");
}

void testBackendHealthCheckParsing()
{
    const BackendClient client;
    require(client.deserializeHealthResponse(R"({"status":"ok"})"), "health parser should accept ok status");
    require(!client.deserializeHealthResponse(R"({"status":"down"})"), "health parser should reject non-ok status");
}

void testBackendUnavailableFallback()
{
    auto backend = std::make_unique<FakeBackendClient>();
    backend->available = false;
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult result = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::OnlineAiAssisted);

    require(result.backendUnavailable, "online mode should mark backend unavailable when health fails");
    require(result.effectiveMode == ConversionMode::OfflineRuleBased, "online mode should fall back to offline");
    require(contains(result.result.modernCode, "nullptr"), "fallback should preserve offline conversion behavior");
    require(result.result.conversionSource == "Offline Fallback after AI Failure", "fallback should stamp fallback source");
    require(result.result.fallbackUsed, "fallback should be marked in result metadata");
}

void testHybridModeExecutionPath()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    backend->response.ok = true;
    backend->response.result.explanation = "Mock AI review explanation.";
    backend->response.result.changes.push_back({"Mock AI review", "local result", "review", "Hybrid path executed.", false});
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult result = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::HybridOfflineAiReview);

    require(backendRaw->convertCalled, "hybrid mode should call backend after local conversion");
    require(backendRaw->lastMode == ConversionMode::HybridOfflineAiReview, "hybrid mode should pass hybrid mode to backend");
    require(contains(result.result.modernCode, "nullptr"), "hybrid mode should keep local converted code");
    require(contains(result.result.explanation, "AI Review"), "hybrid mode should append AI review explanation");
    require(result.result.conversionSource == "Hybrid Offline + AI Review", "hybrid mode should stamp conversion source");
}

void testOnlineModeCanReturnAppliedAiChanges()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    backend->response.ok = true;
    backend->response.result.modernCode = "auto tool = std::make_unique<DiagnosticTool>();";
    backend->response.result.explanation = "AI converted complex ownership.";
    backend->response.result.changes.push_back({
        "Mock AI direct ownership modernization",
        "DiagnosticTool* tool = new DiagnosticTool();",
        "auto tool = std::make_unique<DiagnosticTool>();",
        "Safe direct AI modernization.",
        true,
    });
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult result = coordinator.convert(
        "DiagnosticTool* tool = new DiagnosticTool();\ndelete tool;\n",
        ModernizationOptions{},
        ConversionMode::OnlineAiAssisted);

    require(backendRaw->convertCalled, "online mode should call backend");
    require(result.effectiveMode == ConversionMode::OnlineAiAssisted, "online mode should stay online when backend succeeds");
    require(contains(result.result.modernCode, "std::make_unique"), "online mode should display AI-modernized code");
    require(!result.result.changes.empty() && result.result.changes.front().applied, "online mode should display applied AI changes");
    require(result.result.conversionSource == "Online AI-Assisted", "online mode should stamp conversion source");
}

void testBackendUnavailableDoesNotPermanentlyDisableOnlineChecks()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    backend->available = false;
    backend->response.ok = true;
    backend->response.result.modernCode = "int* value = nullptr;\n";
    backend->response.result.explanation = "Recovered backend response.";
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult fallback = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::OnlineAiAssisted);
    backendRaw->available = true;
    const CoordinatedConversionResult recovered = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::OnlineAiAssisted);

    require(fallback.backendUnavailable, "first online conversion should fall back when backend is unavailable");
    require(!recovered.backendUnavailable, "later online conversion should recover when backend becomes available");
    require(recovered.effectiveMode == ConversionMode::OnlineAiAssisted, "recovered conversion should use online mode");
    require(backendRaw->healthChecks >= 2, "backend availability should be checked again after a failure");
}

void testBackendClientSerialization()
{
    const BackendClient client;
    ModernizationOptions options;
    options.customInstruction = "Prefer RAII.";
    const QByteArray payload = client.serializeConversionRequest("int* p = NULL;", options, ConversionMode::OnlineAiAssisted, nullptr);
    const QJsonDocument document = QJsonDocument::fromJson(payload);

    require(document.isObject(), "backend request should serialize to JSON object");
    require(document.object().value("mode").toString().toStdString() == "online", "backend request should include online mode");
    require(document.object().value("aggressivenessLevel").toString().toStdString() == "balanced",
            "online backend request should include balanced aggressiveness");
    require(document.object().value("options").toObject().value("customInstruction").toString().toStdString() == "Prefer RAII.",
            "backend request should include modernization options");
}

void testBackendClientDeserialization()
{
    const BackendClient client;
    const BackendConversionResponse response = client.deserializeConversionResponse(
        R"({"ok":true,"backendStatus":"Connected","aiProvider":"openai","aiModel":"gpt-4.1-mini","modernCode":"int* p = nullptr;","explanation":"Mock explanation","warnings":["syntax check skipped"],"changes":[{"ruleName":"Mock AI","before":"NULL","after":"nullptr","reason":"mock","applied":true,"skipped":false}]})");

    require(response.ok, "backend response should deserialize ok payload");
    require(contains(response.result.modernCode, "nullptr"), "backend response should include modern code");
    require(response.result.changes.size() == 1, "backend response should include changes");
    require(response.result.changes.front().applied, "backend response should parse applied status");
    require(response.result.backendStatus == "Connected", "backend response should parse backend status");
    require(response.result.aiProvider == "openai", "backend response should parse provider metadata");
    require(response.result.aiModel == "gpt-4.1-mini", "backend response should parse model metadata");
    require(!response.result.diagnosticMessages.empty(), "backend response should preserve warnings as diagnostics");
}

void testMockAiResponseShape()
{
    const BackendClient client;
    const BackendConversionResponse response = client.deserializeConversionResponse(
        R"({"ok":true,"modernCode":"auto p = std::make_unique<T>();","explanation":"Mock AI explanation: code was reviewed by the backend service.","changes":[{"ruleName":"Mock AI-assisted modernization","before":"T* p = new T();","after":"auto p = std::make_unique<T>();","reason":"Mock backend returned a realistic modernization response without calling any AI provider.","applied":true}]})");

    require(response.ok, "mock AI response should parse");
    require(contains(response.result.explanation, "Mock AI explanation"), "mock AI response should include explanation");
    require(contains(response.result.changes.front().ruleName, "Mock AI"), "mock AI response should include mock change");
}

void testRepositoryGitHubUrlValidation()
{
    require(RepositoryCloneService::isValidGitHubUrl("https://github.com/example/project"), "github https URL should be accepted");
    require(RepositoryCloneService::isValidGitHubUrl("https://github.com/example/project.git"), "github https URL with .git should be accepted");
    require(!RepositoryCloneService::isValidGitHubUrl("git@github.com:example/project.git"), "ssh URL should be rejected for now");
    require(!RepositoryCloneService::isValidGitHubUrl("https://evil.example.com/example/project"), "non-github URL should be rejected");
    require(!RepositoryCloneService::isSafeBranchName("main; rm -rf /"), "suspicious branch characters should be rejected");
}

void testRepositoryScannerIgnoresFoldersAndFindsCppFiles()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_scan");
    writeTextFile(root / "src" / "main.cpp", "int main() { return 0; }\n");
    writeTextFile(root / "include" / "main.hpp", "#pragma once\n");
    writeTextFile(root / "vendor" / "legacy.cpp", "int vendor = 0;\n");
    writeTextFile(root / "cmake-build-debug" / "generated.cpp", "int generated = 0;\n");
    writeTextFile(root / ".git" / "ignored.cpp", "int ignored = 0;\n");

    const RepositoryScanner scanner;
    const std::vector<std::filesystem::path> files = scanner.scanCppFiles(root);

    require(files.size() == 2, "scanner should find only supported files outside ignored folders");
    for (const auto& file : files) {
        require(!contains(file.string(), "vendor"), "scanner should ignore vendor folder");
        require(!contains(file.string(), "cmake-build"), "scanner should ignore cmake build folders");
        require(!contains(file.string(), ".git"), "scanner should ignore git folder");
    }
}

void testRepositoryBackupCreation()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_backup");
    const std::filesystem::path file = root / "main.cpp";
    writeTextFile(file, "int* value = NULL;\n");

    const RepositoryBackupService backupService;
    const RepositoryBackupResult result = backupService.createBackup(file);

    require(result.success, "backup should be created");
    require(std::filesystem::exists(file.string() + ".legacy_backup"), "legacy backup file should exist");
}

void testRepositoryModernizationAndReports()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_modernize");
    writeTextFile(root / "src" / "main.cpp",
        "#define NULL 0\n"
        "#include <cstring>\n"
        "typedef unsigned long Size;\n"
        "class Tool {};\n"
        "void run(const char* input)\n"
        "{\n"
        "    Tool* tool = new Tool();\n"
        "    delete tool;\n"
        "    char name[50];\n"
        "    std::strncpy(name, input, sizeof(name));\n"
        "}\n");
    writeTextFile(root / "vendor" / "ignored.cpp", "int* value = NULL;\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/project";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "main.cpp");
    const std::string ignored = readTextFile(root / "vendor" / "ignored.cpp");

    require(result.filesScanned == 1, "repository modernization should scan only non-ignored C++ files");
    require(result.filesModified == 1, "repository modernization should modify legacy file");
    require(result.totalAppliedChanges > 0, "repository report should count applied changes");
    require(contains(modernized, "std::make_unique"), "repository modernization should apply ownership conversion");
    require(contains(modernized, "std::string name = input;"), "repository modernization should apply string conversion");
    require(contains(ignored, "NULL"), "ignored folders should not be modified");
    require(std::filesystem::exists(root / "src" / "main.cpp.legacy_backup"), "modified file should have backup");
    require(std::filesystem::exists(root / "modernization_report.txt"), "text report should be generated");
    require(std::filesystem::exists(root / "modernization_report.json"), "json report should be generated");
    require(contains(readTextFile(root / "modernization_report.txt"), "File:"), "text report should include per-file entries");
    require(contains(readTextFile(root / "modernization_report.json"), "\"files\""), "json report should include files array");
}

void testRepositoryCloneDoesNotOverwriteWithoutConfirmation()
{
    const std::filesystem::path workspace = makeTempDirectory("moderncpp_repo_clone");
    const std::filesystem::path existing = workspace / "project";
    std::filesystem::create_directories(existing);

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/project";
    options.outputWorkspaceFolder = workspace;
    options.allowOverwrite = false;

    const RepositoryCloneService service;
    const RepositoryCloneResult result = service.cloneRepository(options);

    require(!result.success, "clone should not overwrite existing folder without confirmation");
    require(result.message == "Target clone folder already exists.", "clone should report existing folder");
}

void testRepositorySyntaxVerificationFallback()
{
    FileModernizationResult file;
    RepositoryVerificationService verifier;
    verifier.verifyFile(file, "int main() { return 0; }\n");

    require(file.compileVerificationEnabled, "repository syntax verification should be enabled");
    require(file.compileVerificationPassed || file.compilerUsed.empty(),
            "syntax verification should pass or gracefully report missing compiler");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testNullConversion();
    testTypedefConversion();
    testSuggestionGeneration();
    testExplanationGeneration();
    testStandaloneExplanationGenerator();
    testOptionEnabledAppliesRule();
    testOptionDisabledSkipsRule();
    testUnsafeSelectedRuleCreatesSuggestion();
    testDefaultSafeOptionsWork();
    testExplanationReflectsSelectedOptions();
    testCodeRepresentations();
    testDependencyValidation();
    testNullMacroRemoval();
    testNullptrMacroWorkaroundRemoval();
    testStrncpyToStringConversion();
    testIteratorLoopToRangeBasedLoop();
    testIndexLoopToRangeBasedLoop();
    testConvertedLegacySampleCompiles();
    testConvertsSimpleNewDeleteToMakeUnique();
    testConvertsNewWithConstructorArgsToMakeUnique();
    testOwnershipConversionDoesNotDuplicateSuggestions();
    testDoesNotConvertEscapingPointer();
    testDoesNotConvertReturnedPointer();
    testDoesNotConvertAmbiguousOwnership();
    testStringViewAppliesOnlyWhenEnabledAndSafe();
    testGeneratedOwnershipSample();
    testOfflineHelperComputationExtractedToLambda();
    testOfflineFunctorToLambda();
    testOfflineAutoAndConstexprUsage();
    testOfflineOldStyleCastConversion();
    testOfflineOverrideAnnotation();
    testOfflineStructuredBindingAggressiveSafe();
    testOfflineRequiredIncludesAreNotDuplicated();
    testOfflineCompileVerificationPassesForSimpleSample();
    testCompileVerifierHandlesUnavailableCompilerGracefully();
    testOfflineSampleFilesModernize();
    testTokenBasedStructureAnalyzerFindsReusableBlocks();
    testStructuralPreprocessorCleanupAndConstantMacros();
    testStructuralTypedefStructModernization();
    testStructuralCharBufferMemberModernizesToString();
    testStructuralRawDynamicArrayModernizesToVector();
    testStructuralLocalDynamicArrayModernizesToVector();
    testStructuralIteratorAndIndexLoopsModernize();
    testStructuralPreprocessorBalanceValidationRemovesDanglingEndif();
    testDependentVectorCleanupUpdatesAllUsages();
    testDependentStringCleanupUpdatesAllUsages();
    testNestedStringMemberCascadeCleanup();
    testCompilerDiagnosticCleanupFixesKnownLeftovers();
    testValueTypePointerOperationScannerRemovesPointerLeftovers();
    testValueTypeNullptrEqualityBecomesValueStateCheck();
    testEmptyCleanupBlockAfterValueTypeModernizationIsRemoved();
    testExplicitMutableIteratorLoopModernizes();
    testIteratorLoopWithEraseIsPreservedWithWarning();
    testVectorGrowthEmulationCleanupModernizesAppend();
    testVectorGrowthPassFlagsAmbiguousRawAssignment();
    testVectorEmulationEliminatesFieldAppendGrowth();
    testVectorModernizationRemovesCleanupOnlyCopySpecialMembers();
    testVectorAppendPreservesLogicalMaxCapacity();
    testVectorAppendPassConvertsReserveToResizeForFixedIndexWrites();
    testVectorEmulationRemovesOrphanedTempGrowthReferences();
    testOrphanedGrowthSymbolCleanupUsesCompilerDiagnostics();
    testOrphanedGrowthCleanupRemovesPartiallyCleanedCapacityFallout();
    testOrphanedTempBufferLoopCleanupRemovesFullLoop();
    testCompilerDiagnosticCleanupRemovesOrphanedTempBufferLoop();
    testConverterRemovesManualGrowthFragmentsAfterVectorModernization();
    testVectorParadigmRewriteComposesGrowthAndAppendPasses();
    testUnscopedEnumModernizesWhenSafe();
    testUnscopedEnumSuggestionWhenIntegerConversionIsUsed();
    testStdFormatConversionIsOptional();
    testSafeReplacementDoesNotInjectGeneratedCodeIntoComments();
    testAutoIteratorLoopModernizesStructurally();
    testAiStyleLocalComputationBlockToLambda();
    testAiStyleHelperFunctionUsedOnceToLocalLambda();
    testAiStyleFunctorPredicateToLambda();
    testAiStyleIteratorLoopToRangesAlgorithm();
    testAiStyleIteratorLoopToStdForEachForCpp17();
    testAiStyleMixedSampleAggressivelyModernizes();
    testAiStyleCompileVerificationRunsAndAutoFixesIncludes();
    testAiStyleUnsafePatternRemainsSuggestionOnly();
    testAiStyleDiverseSamplesUseGeneralPipeline();
    testAiStyleClassMemberOwnershipModernizesToUniquePtr();
    testAiStyleDestructorWithExtraLogicKeepsDestructor();
    testAiStyleBorrowedRawPointerMemberRemainsSuggestionOnly();
    testAiStyleAliasedPointerOwnershipModernizesToSharedPtr();
    testAiStyleAmbiguousAliasingRemainsSuggestionOnly();
    testAiStyleStringViewParameterExplicitlyOwnsStringMember();
    testRepositoryModeUsesImprovedOfflinePipeline();
    testRepositoryModeUsesStructuralModernizationPipeline();
    testOfflineModeStillWorks();
    testBackendHealthCheckParsing();
    testBackendUnavailableFallback();
    testHybridModeExecutionPath();
    testOnlineModeCanReturnAppliedAiChanges();
    testBackendUnavailableDoesNotPermanentlyDisableOnlineChecks();
    testBackendClientSerialization();
    testBackendClientDeserialization();
    testMockAiResponseShape();
    testRepositoryGitHubUrlValidation();
    testRepositoryScannerIgnoresFoldersAndFindsCppFiles();
    testRepositoryBackupCreation();
    testRepositoryModernizationAndReports();
    testRepositoryCloneDoesNotOverwriteWithoutConfirmation();
    testRepositorySyntaxVerificationFallback();

    std::cout << "All converter tests passed.\n";
    return EXIT_SUCCESS;
}
