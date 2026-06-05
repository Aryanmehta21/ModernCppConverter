#include "app/ConversionCoordinator.h"
#include "backend/BackendClient.h"
#include "backend/IBackendClient.h"
#include "converter/IConverterEngine.h"
#include "converter/ModernCppExplanationGenerator.h"
#include "converter/AstRepresentation.h"
#include "converter/RawTextRepresentation.h"
#include "converter/RuleBasedConverterEngine.h"
#include "converter/TokenRepresentation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

class FakeBackendClient final : public IBackendClient
{
public:
    bool available = true;
    mutable bool convertCalled = false;
    mutable ConversionMode lastMode = ConversionMode::OfflineRuleBased;
    BackendConversionResponse response;

    [[nodiscard]] bool isAvailable() const override
    {
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

void testOfflineModeStillWorks()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    const CoordinatedConversionResult result = coordinator.convert("int* value = NULL;\n", ModernizationOptions{}, ConversionMode::OfflineRuleBased);

    require(contains(result.result.modernCode, "nullptr"), "offline mode should still use local converter");
    require(!backendRaw->convertCalled, "offline mode should not call backend");
    require(result.effectiveMode == ConversionMode::OfflineRuleBased, "offline mode should remain effective mode");
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
    require(document.object().value("options").toObject().value("customInstruction").toString().toStdString() == "Prefer RAII.",
            "backend request should include modernization options");
}

void testBackendClientDeserialization()
{
    const BackendClient client;
    const BackendConversionResponse response = client.deserializeConversionResponse(
        R"({"ok":true,"modernCode":"int* p = nullptr;","explanation":"Mock explanation","changes":[{"ruleName":"Mock AI","before":"NULL","after":"nullptr","reason":"mock","applied":true,"skipped":false}]})");

    require(response.ok, "backend response should deserialize ok payload");
    require(contains(response.result.modernCode, "nullptr"), "backend response should include modern code");
    require(response.result.changes.size() == 1, "backend response should include changes");
    require(response.result.changes.front().applied, "backend response should parse applied status");
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
    testOfflineModeStillWorks();
    testBackendHealthCheckParsing();
    testBackendUnavailableFallback();
    testHybridModeExecutionPath();
    testBackendClientSerialization();
    testBackendClientDeserialization();
    testMockAiResponseShape();

    std::cout << "All converter tests passed.\n";
    return EXIT_SUCCESS;
}
