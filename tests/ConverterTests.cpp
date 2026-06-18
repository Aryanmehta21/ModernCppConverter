#include "app/ConversionCoordinator.h"
#include "backend/BackendClient.h"
#include "backend/IBackendClient.h"
#include "converter/CompilerDiagnosticCleanupPass.h"
#include "converter/CompileVerifier.h"
#include "converter/ContainerModernizationCleanupPass.h"
#include "converter/CrossScopeTypePropagationPass.h"
#include "converter/ImpactCascadingCleanupPass.h"
#include "converter/IConverterEngine.h"
#include "converter/ModernCppExplanationGenerator.h"
#include "converter/OrphanedGrowthSymbolCleanupPass.h"
#include "converter/OrphanedTempBufferLoopCleanupPass.h"
#include "converter/OwnershipGraphAnalyzer.h"
#include "converter/OwnershipSanityScanner.h"
#include "converter/ReturnTypePropagationPass.h"
#include "converter/ScopeAwareSymbolTable.h"
#include "converter/ScopeLeakValidationPass.h"
#include "converter/SemanticTypeValidationPass.h"
#include "converter/SmartPointerCollectionPropagationPass.h"
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
#include <numeric>
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

bool diagnosticsContain(const ConversionResult& result, const std::string& needle)
{
    return std::any_of(result.diagnosticMessages.begin(),
                       result.diagnosticMessages.end(),
                       [&needle](const std::string& diagnostic) {
                           return contains(diagnostic, needle);
                       });
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

ModernizationOptions structuralOptions();

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

void testFunctionPointerTypedefAndLocalCallbackModernization()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "typedef int (*Transform)(int);\n"
        "int increment(int value) { return value + 1; }\n"
        "void run()\n"
        "{\n"
        "    int (*callback)(int) = &increment;\n"
        "    std::cout << callback(1) << '\\n';\n"
        "}\n",
        options);

    require(contains(result.modernCode, "using Transform = int (*)(int);"),
            "function pointer typedef should become a using alias");
    require(contains(result.modernCode, "auto callback = &increment;"),
            "local function pointer initialized from a callable should use auto");
    require(!contains(result.modernCode, "typedef int (*Transform)(int);"),
            "legacy function pointer typedef should be removed");
    require(hasAppliedRule(result, "Function pointer typedef to using"),
            "function pointer typedef modernization should be tracked");
    require(hasAppliedRule(result, "Function pointer local callback to auto"),
            "local function pointer auto modernization should be tracked");
    require(result.compileVerificationPassed,
            "function pointer typedef/local callback modernization should pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testStoredCallbackUsesStdFunctionConservatively()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "void consume(int) {}\n"
        "class CallbackStore\n"
        "{\n"
        "public:\n"
        "    CallbackStore() : callback(nullptr) {}\n"
        "    void setCallback(void (*next)(int))\n"
        "    {\n"
        "        callback = next;\n"
        "    }\n"
        "    void notify(int value)\n"
        "    {\n"
        "        if (callback != nullptr)\n"
        "        {\n"
        "            callback(value);\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    void (*callback)(int);\n"
        "};\n"
        "void run()\n"
        "{\n"
        "    CallbackStore store;\n"
        "    store.setCallback(&consume);\n"
        "    store.notify(3);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <functional>"),
            "stored callback modernization should add <functional>");
    require(contains(result.modernCode, "std::function<void(int)> callback;"),
            "private stored callback should become std::function");
    require(contains(result.modernCode, "void setCallback(std::function<void(int)> next)"),
            "visible local setter feeding callback storage should accept std::function");
    require(contains(result.modernCode, "if (callback)"),
            "converted std::function callback should use boolean emptiness checks");
    require(!contains(result.modernCode, "void (*callback)(int)"),
            "stored raw callback field should be removed");
    require(!contains(result.modernCode, "void (*next)(int)"),
            "stored callback setter parameter should be updated");
    require(hasAppliedRule(result, "Stored callback pointer to std::function"),
            "stored callback modernization should be tracked");
    require(result.compileVerificationPassed,
            "stored callback std::function modernization should pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testRawFunctionPointerParameterPreservedWhenNotStored()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "void apply(void (*callback)(int))\n"
        "{\n"
        "    callback(7);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "void apply(void (*callback)(int))"),
            "non-stored raw callback parameter should remain a function pointer");
    require(!contains(result.modernCode, "std::function"),
            "std::function should not be introduced for simple observing callback parameters");
    require(hasSuggestionRule(result, "Function pointer parameter preserved"),
            "preserved raw function pointer parameter should produce a conservative suggestion");
    require(result.compileVerificationPassed,
            "preserved raw function pointer parameter sample should still pass syntax verification\n"
            + result.compilerOutput);
}

void testPrintfSimpleTextModernizesToCout()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void greet()\n"
        "{\n"
        "    printf(\"ready\\n\");\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <iostream>"),
            "printf text modernization should add iostream");
    require(contains(result.modernCode, "std::cout << \"ready\\n\";"),
            "simple printf text should become std::cout output");
    require(!contains(result.modernCode, "printf("), "converted printf should be removed");
    require(!contains(result.modernCode, "#include <cstdio>"), "cstdio include should be removed when no C stdio APIs remain");
    require(hasAppliedRule(result, "printf-family output to iostream"), "printf text modernization should be tracked");
    require(result.compileVerificationPassed,
            "simple printf text modernization should pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testPrintfValuesModernizeToIostreamChain()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void report(const char* name, int count, unsigned long total)\n"
        "{\n"
        "    std::printf(\"Name: %s count=%d total=%lu\\n\", name, count, total);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::cout << \"Name: \" << name << \" count=\" << count << \" total=\" << total << \"\\n\";"),
            "simple printf format values should become an iostream chain\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::printf"), "converted std::printf should be removed");
    require(hasAppliedRule(result, "printf-family output to iostream"), "printf value modernization should be tracked");
    require(result.compileVerificationPassed,
            "printf value modernization should pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testPrintfValuesCanUseStdFormatWhenEnabled()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.useStdFormatForStreams = true;
    options.targetStandard = CppStandard::Cpp20;
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void report(int count)\n"
        "{\n"
        "    printf(\"count=%d\\n\", count);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <format>"), "std::format printf modernization should add format include");
    require(contains(result.modernCode, "std::cout << std::format(\"count={}\\n\", count);"),
            "printf format values should use std::format when the C++20 format option is enabled");
    require(hasAppliedRule(result, "printf-family output to std::format"), "printf std::format modernization should be tracked");
    require(result.compilerUsed.empty() || result.compileVerificationPassed,
            "printf std::format modernization should compile where compiler supports <format>\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testFprintfStdoutAndStderrModernizeToStreams()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void report(const char* message, int code)\n"
        "{\n"
        "    fprintf(stdout, \"message: %s\\n\", message);\n"
        "    std::fprintf(stderr, \"error: %d\\n\", code);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::cout << \"message: \" << message << \"\\n\";"),
            "fprintf(stdout, ...) should become std::cout");
    require(contains(result.modernCode, "std::cerr << \"error: \" << code << \"\\n\";"),
            "fprintf(stderr, ...) should become std::cerr");
    require(!contains(result.modernCode, "fprintf(stdout"), "converted stdout fprintf should be removed");
    require(!contains(result.modernCode, "std::fprintf(stderr"), "converted stderr fprintf should be removed");
    require(result.compileVerificationPassed,
            "fprintf stdout/stderr modernization should pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
}

void testUnsafePrintfFormatsRemainSuggestions()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void report(int value, double ratio, FILE* file)\n"
        "{\n"
        "    printf(\"value=%04d\\n\", value);\n"
        "    printf(\"ratio=%.2f\\n\", ratio);\n"
        "    fprintf(file, \"value=%d\\n\", value);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "printf(\"value=%04d\\n\", value);"),
            "width-formatted printf should remain unchanged");
    require(contains(result.modernCode, "printf(\"ratio=%.2f\\n\", ratio);"),
            "precision/floating printf should remain unchanged");
    require(contains(result.modernCode, "fprintf(file, \"value=%d\\n\", value);"),
            "arbitrary FILE* fprintf should remain unchanged for FILE I/O pass/manual review");
    require(hasSuggestionRule(result, "printf-family output modernization suggestion"),
            "unsafe printf-family calls should produce suggestions");
    require(result.compileVerificationPassed,
            "unsafe printf preservation sample should still pass syntax verification\n"
            + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
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
    require(contains(result.modernCode, "std::cout << value << '\\n';"), "iterator dereference should become element variable");
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

void testConvertsTemplatedLocalNewDeleteToMakeUnique()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <string>\n"
        "void run()\n"
        "{\n"
        "    std::string* label = new std::string(\"ready\");\n"
        "    delete label;\n"
        "}\n");

    require(contains(result.modernCode, "auto label = std::make_unique<std::string>(\"ready\");"),
            "local templated/qualified new/delete ownership should convert to make_unique\nConverted code:\n"
                + result.modernCode);
    require(!contains(result.modernCode, "delete label"),
            "delete for converted qualified local owner should be removed\nConverted code:\n" + result.modernCode);
}

void testConvertsBasePointerNewDerivedToUniquePtrAndGet()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "class Base\n"
        "{\n"
        "public:\n"
        "    virtual ~Base() = default;\n"
        "    virtual void touch() {}\n"
        "};\n"
        "class Derived : public Base {};\n"
        "void observe(Base* value)\n"
        "{\n"
        "    if (value != nullptr)\n"
        "    {\n"
        "        value->touch();\n"
        "    }\n"
        "}\n"
        "void run()\n"
        "{\n"
        "    Base* value = new Derived();\n"
        "    observe(value);\n"
        "    delete value;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::unique_ptr<Base> value = std::make_unique<Derived>();"),
            "base pointer owning a derived allocation should become unique_ptr<Base>/make_unique<Derived>\nConverted code:\n"
                + result.modernCode);
    require(contains(result.modernCode, "observe(value.get());"),
            "base unique_ptr owner passed to raw observer should use .get()\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete value"),
            "delete for base/derived unique ownership should be removed\nConverted code:\n" + result.modernCode);
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "base/derived unique_ptr ownership propagation sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testUniquePtrOwnerPassedToRawObserverUsesGet()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "class Device\n"
        "{\n"
        "public:\n"
        "    void ping() {}\n"
        "};\n\n"
        "void inspect(Device* device)\n"
        "{\n"
        "    if (device != nullptr)\n"
        "    {\n"
        "        device->ping();\n"
        "    }\n"
        "}\n\n"
        "void run()\n"
        "{\n"
        "    Device* device = new Device();\n"
        "    inspect(device);\n"
        "    delete device;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "auto device = std::make_unique<Device>();"),
            "local new/delete owner should become unique_ptr");
    require(contains(result.modernCode, "inspect(device.get());"),
            "unique_ptr owner passed to visible raw observer should use .get()\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "inspect(device);"),
            "unique_ptr should not be passed directly to a raw pointer sink");
    require(!contains(result.modernCode, "delete device"), "manual delete should be removed");
    require(hasAppliedRule(result, "Smart pointer sink propagation"), "raw sink propagation should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "unique_ptr raw observer propagation should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testClassRawPointerMemberDeletesCopyAndDefaultsMove()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "struct Resource\n"
        "{\n"
        "    int value = 0;\n"
        "};\n\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    Owner()\n"
        "        : resource(new Resource())\n"
        "    {\n"
        "    }\n\n"
        "    Owner(const Owner& other)\n"
        "        : resource(new Resource(*other.resource))\n"
        "    {\n"
        "    }\n\n"
        "    Owner& operator=(const Owner& other)\n"
        "    {\n"
        "        if (this != &other)\n"
        "        {\n"
        "            delete resource;\n"
        "            resource = new Resource(*other.resource);\n"
        "        }\n"
        "        return *this;\n"
        "    }\n\n"
        "    ~Owner()\n"
        "    {\n"
        "        delete resource;\n"
        "        resource = nullptr;\n"
        "    }\n\n"
        "    Resource* getResource() const\n"
        "    {\n"
        "        return resource;\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::unique_ptr<Resource> resource;"),
            "owned raw pointer member should become unique_ptr\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "resource(std::make_unique<Resource>())"),
            "constructor initializer new should become make_unique");
    require(contains(result.modernCode, "Owner(const Owner&) = delete;"),
            "copy constructor should be deleted for unique ownership");
    require(contains(result.modernCode, "Owner& operator=(const Owner&) = delete;"),
            "copy assignment should be deleted for unique ownership");
    require(contains(result.modernCode, "Owner(Owner&&) noexcept = default;"),
            "move constructor should be defaulted for unique ownership");
    require(contains(result.modernCode, "Owner& operator=(Owner&&) noexcept = default;"),
            "move assignment should be defaulted for unique ownership");
    require(contains(result.modernCode, "return resource.get();"),
            "raw observer getter should return unique_ptr::get()");
    require(!contains(result.modernCode, "delete resource"), "manual member delete should be removed");
    require(!contains(result.modernCode, "~Owner()"), "cleanup-only destructor should be removed");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "member unique_ptr copy/move modernization should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testMallocSingleObjectModernizesToUniquePtr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstdlib>\n"
        "struct Record\n"
        "{\n"
        "    int value = 0;\n"
        "};\n\n"
        "int readValue()\n"
        "{\n"
        "    Record* record = (Record*)malloc(sizeof(Record));\n"
        "    record->value = 7;\n"
        "    free(record);\n"
        "    return 7;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <memory>"), "malloc single-object conversion should add memory include");
    require(contains(result.modernCode, "auto record = std::make_unique<Record>();"),
            "single-object malloc/free should become make_unique");
    require(!contains(result.modernCode, "malloc("), "converted single-object malloc should be removed");
    require(!contains(result.modernCode, "free(record)"), "converted single-object free should be removed");
    require(!contains(result.modernCode, "#include <cstdlib>"), "cstdlib include should be removed when malloc/free are gone");
    require(hasAppliedRule(result, "malloc/free ownership to std::unique_ptr"),
            "single-object malloc/free conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "single-object malloc/free conversion should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testMallocArrayModernizesToVector()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstdlib>\n"
        "int sum(int count)\n"
        "{\n"
        "    int* values = (int*)malloc(count * sizeof(int));\n"
        "    values[0] = 1;\n"
        "    values[count - 1] = 2;\n"
        "    free(values);\n"
        "    return count;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <vector>"), "malloc array conversion should add vector include");
    require(contains(result.modernCode, "std::vector<int> values(count);"), "array malloc/free should become vector sized by element count");
    require(!contains(result.modernCode, "malloc("), "converted array malloc should be removed");
    require(!contains(result.modernCode, "free(values)"), "converted array free should be removed");
    require(hasAppliedRule(result, "malloc/free array ownership to std::vector"),
            "array malloc/free conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "array malloc/free conversion should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testCallocAndByteBufferModernizeToVector()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstdlib>\n"
        "void build(int count, int byteCount)\n"
        "{\n"
        "    int* values = static_cast<int*>(calloc(count, sizeof(int)));\n"
        "    unsigned char* bytes = (unsigned char*)malloc(byteCount);\n"
        "    values[0] = 3;\n"
        "    bytes[0] = 4;\n"
        "    free(values);\n"
        "    free(bytes);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values(count);"), "calloc array should become vector");
    require(contains(result.modernCode, "std::vector<unsigned char> bytes(byteCount);"), "raw byte malloc should become byte vector");
    require(!contains(result.modernCode, "calloc("), "converted calloc should be removed");
    require(!contains(result.modernCode, "malloc("), "converted byte malloc should be removed");
    require(!contains(result.modernCode, "free(values)"), "converted calloc free should be removed");
    require(!contains(result.modernCode, "free(bytes)"), "converted byte free should be removed");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "calloc and byte-buffer conversion should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testMallocMemberCleanupDestructorModernizesToRuleOfZero()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstdlib>\n"
        "struct Resource\n"
        "{\n"
        "    int value = 0;\n"
        "};\n\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    Owner()\n"
        "    {\n"
        "        resource = (Resource*)malloc(sizeof(Resource));\n"
        "    }\n\n"
        "    ~Owner()\n"
        "    {\n"
        "        if (resource != nullptr)\n"
        "        {\n"
        "            free(resource);\n"
        "            resource = nullptr;\n"
        "        }\n"
        "    }\n\n"
        "    int value() const\n"
        "    {\n"
        "        return resource->value;\n"
        "    }\n\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::unique_ptr<Resource> resource;"),
            "malloc-owned member should become unique_ptr\nConverted code:\n" + result.modernCode
                + "\nDiagnostics:\n"
                + std::accumulate(result.diagnosticMessages.begin(), result.diagnosticMessages.end(), std::string{}, [](std::string acc, const std::string& message) {
                      return std::move(acc) + message + "\n";
                  }));
    require(contains(result.modernCode, "resource = std::make_unique<Resource>();"),
            "member malloc allocation should become make_unique assignment");
    require(!contains(result.modernCode, "free(resource)"), "member free cleanup should be removed");
    require(!contains(result.modernCode, "~Owner()"), "cleanup-only destructor should be removed after malloc/free RAII conversion");
    require(hasAppliedRule(result, "malloc/free member ownership to std::unique_ptr"),
            "member malloc/free conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "member malloc/free conversion should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testMallocReallocAndEscapingOwnershipRemainUnchanged()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    const ConversionResult result = converter.convert(
        "#include <cstdlib>\n"
        "extern void consume(void*);\n"
        "void resizeAndConsume(int count)\n"
        "{\n"
        "    int* values = (int*)malloc(count * sizeof(int));\n"
        "    values = (int*)realloc(values, count * 2 * sizeof(int));\n"
        "    free(values);\n"
        "    int* external = (int*)malloc(sizeof(int));\n"
        "    consume(external);\n"
        "    free(external);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "realloc(values"), "realloc-heavy malloc ownership should remain unchanged");
    require(contains(result.modernCode, "consume(external);"), "malloc pointer passed to unknown C API should remain unchanged");
    require(!contains(result.modernCode, "std::vector<int> values"), "realloc candidate should not become vector");
    require(!contains(result.modernCode, "std::make_unique<int>()"), "escaping malloc candidate should not become unique_ptr");
    require(hasSuggestionRule(result, "malloc/free ownership modernization"),
            "unsafe malloc/free ownership should emit a review suggestion");
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

void testOwnershipGraphAnalyzerClassifiesPointerCollection()
{
    const std::string legacy =
        "struct Node {};\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    explicit Owner(int count)\n"
        "    {\n"
        "        nodes = new Node*[count];\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            nodes[i] = new Node();\n"
        "        }\n"
        "    }\n"
        "    ~Owner()\n"
        "    {\n"
        "        for (int i = 0; i < 4; ++i)\n"
        "        {\n"
        "            delete nodes[i];\n"
        "        }\n"
        "        delete[] nodes;\n"
        "    }\n"
        "private:\n"
        "    Node** nodes;\n"
        "};\n";

    const OwnershipGraphAnalyzer analyzer;
    const std::vector<OwnershipGraphNode> nodes = analyzer.analyze(legacy);

    require(std::any_of(nodes.begin(), nodes.end(), [](const OwnershipGraphNode& node) {
                return node.storageName == "nodes"
                    && node.elementType == "Node"
                    && node.classification == OwnershipClassification::SequentialCollectionOwnership
                    && node.isPointerToPointer;
            }),
            "ownership analyzer should classify pointer-to-pointer ownership collections");
}

void testPointerToPointerCollectionModernizesToVectorUniquePtr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "struct Node\n"
        "{\n"
        "    int value{};\n"
        "};\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    explicit Owner(int count)\n"
        "        : count(count)\n"
        "    {\n"
        "        nodes = new Node*[count];\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            nodes[i] = new Node();\n"
        "        }\n"
        "    }\n"
        "    ~Owner()\n"
        "    {\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            delete nodes[i];\n"
        "        }\n"
        "        delete[] nodes;\n"
        "    }\n"
        "private:\n"
        "    int count;\n"
        "    Node** nodes;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<std::unique_ptr<Node>> nodes;"),
            "pointer-to-pointer storage should become vector<unique_ptr>");
    require(contains(result.modernCode, "nodes.reserve(count);"), "outer pointer array allocation should become vector reserve");
    require(contains(result.modernCode, "nodes.push_back(std::make_unique<Node>());"),
            "inner allocations should become make_unique push_back");
    require(!contains(result.modernCode, "Node** nodes"), "raw pointer-to-pointer member should be removed");
    require(!contains(result.modernCode, "delete nodes[i]"), "nested delete loop should be removed");
    require(!contains(result.modernCode, "delete[] nodes"), "outer delete[] should be removed");
    require(!contains(result.modernCode, "~Owner()"), "cleanup-only destructor should be removed after ownership graph modernization");
    require(hasAppliedRule(result, "Pointer-to-pointer ownership collection to std::vector<std::unique_ptr>"),
            "pointer-to-pointer ownership conversion should be tracked");
    require(hasAppliedRule(result, "Nested delete loop elimination"), "nested delete loop removal should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "pointer-to-pointer ownership sample should pass syntax verification");
    }
}

void testFixedPointerArrayModernizesToArrayUniquePtr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "struct Widget {};\n"
        "class FixedOwner\n"
        "{\n"
        "public:\n"
        "    FixedOwner()\n"
        "    {\n"
        "        for (int i = 0; i < 3; ++i)\n"
        "        {\n"
        "            slots[i] = new Widget();\n"
        "        }\n"
        "    }\n"
        "    ~FixedOwner()\n"
        "    {\n"
        "        for (int i = 0; i < 3; ++i)\n"
        "        {\n"
        "            delete slots[i];\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    Widget* slots[3];\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::array<std::unique_ptr<Widget>, 3> slots;"),
            "fixed owning pointer array should become array<unique_ptr>");
    require(contains(result.modernCode, "slots[i] = std::make_unique<Widget>();"),
            "fixed pointer array element allocation should become make_unique");
    require(!contains(result.modernCode, "Widget* slots[3]"), "raw fixed pointer array should be removed");
    require(!contains(result.modernCode, "delete slots[i]"), "fixed array delete loop should be removed");
    require(hasAppliedRule(result, "Fixed pointer array ownership to std::array<std::unique_ptr>"),
            "fixed array ownership conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "fixed pointer array ownership sample should pass syntax verification");
    }
}

void testOwningRawPointerVectorModernizesToVectorUniquePtr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "struct Entry {};\n"
        "class EntrySet\n"
        "{\n"
        "public:\n"
        "    void add()\n"
        "    {\n"
        "        entries.push_back(new Entry());\n"
        "    }\n"
        "    ~EntrySet()\n"
        "    {\n"
        "        for (auto entry : entries)\n"
        "        {\n"
        "            delete entry;\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    std::vector<Entry*> entries;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<std::unique_ptr<Entry>> entries;"),
            "vector of owning raw pointers should become vector<unique_ptr>");
    require(contains(result.modernCode, "entries.push_back(std::make_unique<Entry>());"),
            "new object insertion should become make_unique");
    require(!contains(result.modernCode, "std::vector<Entry*>"), "raw pointer vector type should be removed");
    require(!contains(result.modernCode, "delete entry"), "delete loop should be removed");
    require(hasAppliedRule(result, "Owning raw pointer container to std::vector<std::unique_ptr>"),
            "owning raw pointer vector conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "owning raw pointer vector sample should pass syntax verification");
    }
}

void testStringLikeOwningClassProducesSuggestionOnly()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class TextValue\n"
        "{\n"
        "public:\n"
        "    explicit TextValue(const char* input)\n"
        "    {\n"
        "        data = new char[std::strlen(input) + 1];\n"
        "        std::strcpy(data, input);\n"
        "    }\n"
        "    ~TextValue()\n"
        "    {\n"
        "        delete[] data;\n"
        "    }\n"
        "    const char* c_str() const { return data; }\n"
        "private:\n"
        "    char* data;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string data;"),
            "internally owned text buffer should become std::string\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "data = input;"),
            "strcpy initialization should become string assignment\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return data.c_str();"),
            "C-string compatibility getter should return std::string::c_str()\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "char* data;"), "raw char* text member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[] data;"), "manual string buffer cleanup should be removed after conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::strcpy"), "strcpy should be removed after string conversion\nConverted code:\n" + result.modernCode);
    require(hasAppliedRule(result, "Class raw char buffer to std::string"), "string ownership class conversion should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "string-like owning class modernization should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testOwnershipSanityScannerRemovesPartialSmartCollectionCleanup()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "Item**",
        "std::vector<std::unique_ptr<Item>>",
        "Owner",
        true,
        "Pointer-to-pointer ownership collection to std::vector<std::unique_ptr>",
        {"remove nested delete loops"},
        {},
        false,
    });

    const std::string code =
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item {};\n"
        "struct Owner\n"
        "{\n"
        "    std::vector<std::unique_ptr<Item>> items;\n"
        "    ~Owner()\n"
        "    {\n"
        "        for (int i = 0; i < 4; ++i)\n"
        "        {\n"
        "            delete items[i];\n"
        "        }\n"
        "        delete[] items;\n"
        "    }\n"
        "};\n";

    std::vector<ConversionChange> changes;
    const OwnershipSanityScanner scanner;
    const std::string fixed = scanner.rewrite(code, context, changes);

    require(!contains(fixed, "delete items[i]"), "sanity scanner should remove nested delete for smart collection");
    require(!contains(fixed, "delete[] items"), "sanity scanner should remove delete[] for smart collection");
    require(hasAppliedRule(changes, "Ownership sanity scanner"), "ownership sanity cleanup should be tracked");
}

void testUniquePtrCollectionTraversalPreservesIndexSafely()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item { void print() const {} };\n"
        "void printItems(const std::vector<std::unique_ptr<Item>>& items)\n"
        "{\n"
        "    for (int i = 0; i < items.size(); ++i)\n"
        "    {\n"
        "        if (items[i] != nullptr)\n"
        "        {\n"
        "            std::cout << i << std::endl;\n"
        "            items[i]->print();\n"
        "        }\n"
        "    }\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::size_t index = 0;"), "unique_ptr traversal should preserve index when it is used");
    require(contains(result.modernCode, "for (const auto& item : items)"), "unique_ptr traversal should become range-based loop");
    require(contains(result.modernCode, "if (item)"), "nullptr check should become smart pointer truthiness");
    require(contains(result.modernCode, "item->print();"), "element access should use the unique_ptr loop variable");
    require(!contains(result.modernCode, "items[i]"), "indexed smart pointer access should be removed");
    require(hasAppliedRule(result, "Unique_ptr collection loop to range-based for"), "unique_ptr loop polish should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "unique_ptr traversal polish should pass syntax verification");
    }
}

void testUniquePtrCollectionPushBackDoesNotBecomeInitializerList()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item {};\n"
        "void makeItems()\n"
        "{\n"
        "    std::vector<std::unique_ptr<Item>> items;\n"
        "    items.push_back(std::make_unique<Item>());\n"
        "    items.push_back(std::make_unique<Item>());\n"
        "}\n",
        structuralOptions());

    require(contains(result.modernCode, "std::vector<std::unique_ptr<Item>> items;"),
            "unique_ptr vector declaration should remain as a normal declaration");
    require(contains(result.modernCode, "items.push_back(std::make_unique<Item>());"),
            "unique_ptr insertions should remain push_back(make_unique)");
    require(!contains(result.modernCode, "std::vector<std::unique_ptr<Item>> items{"),
            "move-only unique_ptr collection should not become initializer-list construction");
}

void testSmartPointerCollectionPropagationFixesRawNewAndGets()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <array>\n"
        "#include <memory>\n"
        "struct Base\n"
        "{\n"
        "    virtual void run();\n"
        "};\n"
        "struct Derived : public Base\n"
        "{\n"
        "    void run();\n"
        "};\n"
        "void inspect(Base* item);\n"
        "void makeItems()\n"
        "{\n"
        "    std::array<std::unique_ptr<Base>, 2> items;\n"
        "    items[0] = std::make_unique<Derived>();\n"
        "    items[1] = new Derived();\n"
        "    inspect(items[1]);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "items[1] = std::make_unique<Derived>();"),
            "raw new assigned to unique_ptr array element should become make_unique");
    require(!contains(result.modernCode, "items[1] = new Derived"), "raw new assignment should not remain");
    require(contains(result.modernCode, "inspect(items[1].get());"), "raw pointer callsite should receive .get()");
    require(contains(result.modernCode, "virtual ~Base() = default;"), "polymorphic base should gain virtual destructor");
    require(contains(result.modernCode, "void run() override;"), "derived override should be marked");
    require(hasAppliedRule(result, "Smart pointer collection raw allocation to make_unique"),
            "smart pointer allocation propagation should be tracked");
    require(hasAppliedRule(result, "Smart pointer collection raw pointer callsite to get"),
            "raw pointer callsite propagation should be tracked");
    require(hasAppliedRule(result, "Add virtual destructor for polymorphic base"),
            "virtual destructor modernization should be tracked");
    require(hasAppliedRule(result, "Add override"),
            "override modernization should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "smart pointer collection propagation sample should pass syntax verification");
    }
}

void testSmartPointerVectorAppendAndPredicateUseMakeUniqueAndGet()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item { bool ready() const { return true; } };\n"
        "bool isReady(Item* item) { return item != nullptr && item->ready(); }\n"
        "void build(std::vector<std::unique_ptr<Item>>& items)\n"
        "{\n"
        "    items.push_back(new Item());\n"
        "    if (isReady(items[0]))\n"
        "    {\n"
        "    }\n"
        "}\n",
        options);

    require(contains(result.modernCode, "items.push_back(std::make_unique<Item>());"),
            "raw pointer append into unique_ptr vector should use make_unique");
    require(!contains(result.modernCode, "push_back(new Item"), "raw pointer append should not remain");
    require(contains(result.modernCode, "isReady(items[0].get())"), "helper predicate expecting raw pointer should receive .get()");
    require(hasAppliedRule(result, "Smart pointer collection raw allocation to make_unique"),
            "vector append make_unique propagation should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "unique_ptr vector append propagation should pass syntax verification");
    }
}

void testUniquePtrCollectionCountLoopBecomesCountIf()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.useLambdas = true;
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item { bool enabled() const { return true; } };\n"
        "bool isEnabled(Item* item) { return item != nullptr && item->enabled(); }\n"
        "int countEnabled(const std::vector<std::unique_ptr<Item>>& items)\n"
        "{\n"
        "    int enabled = 0;\n"
        "    for (std::size_t i = 0; i < items.size(); ++i)\n"
        "    {\n"
        "        if (isEnabled(items[i]))\n"
        "        {\n"
        "            ++enabled;\n"
        "        }\n"
        "    }\n"
        "    return enabled;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <algorithm>"), "count_if modernization should add algorithm include");
    require(contains(result.modernCode, "auto enabled = std::count_if(items.begin(), items.end(), [](const auto& item)"),
            "simple count loop over unique_ptr collection should become count_if");
    require(contains(result.modernCode, "return isEnabled(item.get());"), "count_if predicate should pass raw pointer view to helper");
    require(!contains(result.modernCode, "items[i]"), "indexing should be removed from count loop");
    require(hasAppliedRule(result, "Smart pointer collection count loop to count_if"),
            "count loop modernization should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "count_if unique_ptr sample should pass syntax verification");
    }
}

void testFilePointerWriteModernizesToOfstream()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void writeText(const char* path)\n"
        "{\n"
        "    FILE* file = fopen(path, \"w\");\n"
        "    if (file == nullptr) { return; }\n"
        "    fprintf(file, \"ok\\n\");\n"
        "    fclose(file);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "#include <fstream>"), "FILE* RAII modernization should add fstream include");
    require(contains(result.modernCode, "std::ofstream file(path);"), "simple write FILE* should become ofstream");
    require(contains(result.modernCode, "if (!file) { return; }"), "null check should become stream state check");
    require(contains(result.modernCode, "file << \"ok\\n\";"), "simple fprintf literal should become stream output");
    require(!contains(result.modernCode, "fclose(file)"), "manual fclose should be removed");
    require(hasAppliedRule(result, "FILE pointer to fstream RAII"), "FILE* RAII change should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "FILE* RAII sample should pass syntax verification");
    }
}

void testRepositoryModeUsesSmartPointerPropagationPass()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_smart_propagation");
    writeTextFile(root / "src" / "smart.cpp",
        "#include <vector>\n"
        "struct Item {};\n"
        "void observe(Item* item);\n"
        "void build()\n"
        "{\n"
        "    std::vector<std::unique_ptr<Item>> items;\n"
        "    items.push_back(new Item());\n"
        "    observe(items[0]);\n"
        "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/smart-propagation";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "smart.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository smart propagation should scan one source file");
    require(result.filesModified == 1, "repository smart propagation should modify the source file");
    require(contains(modernized, "items.push_back(std::make_unique<Item>());"),
            "repository mode should propagate unique_ptr vector append to make_unique");
    require(contains(modernized, "observe(items[0].get());"),
            "repository mode should propagate raw pointer observer calls with .get()");
    require(contains(report, "Smart pointer collection raw allocation to make_unique"),
            "repository report should include smart pointer allocation propagation");
    require(contains(report, "Smart pointer collection raw pointer callsite to get"),
            "repository report should include .get() callsite propagation");
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

void testCompileVerifierDoesNotMergeMultipleMainSnippets()
{
    const CompileVerificationResult result = CompileVerifier::verifySyntaxOnly(
        "int main() { return 0; }\n\n"
        "int main() { return 1; }\n");
    require(result.verificationEnabled, "multiple-main verification should still report verification metadata");
    if (result.compilerFound) {
        require(!result.passed, "combined snippets with multiple main functions should not be accepted as one translation unit");
        require(contains(result.output, "Multiple main functions detected"),
                "multiple-main diagnostic should tell callers to verify snippets separately");
    }
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
    require(contains(result.modernCode, "constexpr auto SCALE_VALUE"), "simple function-like macro should become a constexpr function");
    require(hasAppliedRule(result, "Remove obsolete preprocessor workaround block"), "obsolete preprocessor removal should be tracked");
    require(hasAppliedRule(result, "Constant macro to constexpr"), "constant macro conversion should be tracked");
    require(hasAppliedRule(result, "Function-like macro to constexpr function"), "function-like macro conversion should be tracked");
}

void testFunctionLikeMacroModernization()
{
    RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult safeResult = converter.convert(
        "#define SQUARE(x) ((x) * (x))\n"
        "#define ADD(left, right) ((left) + (right))\n"
        "int compute(int value)\n"
        "{\n"
        "    return SQUARE(value) + ADD(value, 1);\n"
        "}\n",
        options);

    require(!contains(safeResult.modernCode, "#define SQUARE"), "safe expression macro should be removed after constexpr conversion");
    require(!contains(safeResult.modernCode, "#define ADD"), "safe multi-parameter expression macro should be removed after constexpr conversion");
    require(contains(safeResult.modernCode, "template <typename T0>"), "single-parameter macro should become a function template");
    require(contains(safeResult.modernCode, "constexpr auto SQUARE(") && contains(safeResult.modernCode, "x)"),
            "macro name and parameter should be preserved in constexpr function");
    require(contains(safeResult.modernCode, "constexpr auto ADD(")
                && contains(safeResult.modernCode, "left")
                && contains(safeResult.modernCode, "right"),
            "multi-parameter macro should become constexpr template");
    require(contains(safeResult.modernCode, "return SQUARE(value) + ADD(value, 1);"), "call sites should remain ordinary function calls");
    require(hasAppliedRule(safeResult, "Function-like macro to constexpr function"), "function-like macro conversion should be recorded");
    if (safeResult.compileVerificationEnabled) {
        require(safeResult.compileVerificationPassed, "safe function-like macro conversion should pass syntax verification");
    }

    const ConversionResult unsafeResult = converter.convert(
        "#define JOIN(left, right) left ## right\n"
        "#define TO_TEXT(value) #value\n"
        "#define SET_FLAG(value) do { (value) = 1; } while (0)\n"
        "#define TWICE(value) ((value) + (value))\n"
        "int mutate(int value)\n"
        "{\n"
        "    return TWICE(++value);\n"
        "}\n",
        options);

    require(contains(unsafeResult.modernCode, "#define JOIN(left, right)"), "token-pasting macro should remain unchanged");
    require(contains(unsafeResult.modernCode, "#define TO_TEXT(value)"), "stringification macro should remain unchanged");
    require(contains(unsafeResult.modernCode, "#define SET_FLAG(value)"), "multi-statement macro should remain unchanged");
    require(contains(unsafeResult.modernCode, "#define TWICE(value)"), "macro with side-effect-sensitive visible call should remain unchanged");
    require(!contains(unsafeResult.modernCode, "constexpr auto JOIN"), "unsafe macro should not get constexpr replacement");
    require(hasSuggestionRule(unsafeResult, "Function-like macro to constexpr function"), "unsafe function-like macros should produce suggestions");
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

void testClassMultipleOwnedCharTextMembersModernizeTogether()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class LabelPair\n"
        "{\n"
        "public:\n"
        "    LabelPair(const char* leftInput, const char* rightInput)\n"
        "    {\n"
        "        left = new char[std::strlen(leftInput) + 1];\n"
        "        std::strcpy(left, leftInput);\n"
        "        right = new char[std::strlen(rightInput) + 1];\n"
        "        std::strcpy(right, rightInput);\n"
        "    }\n\n"
        "    LabelPair(const LabelPair& other)\n"
        "    {\n"
        "        left = new char[std::strlen(other.left) + 1];\n"
        "        std::strcpy(left, other.left);\n"
        "        right = new char[std::strlen(other.right) + 1];\n"
        "        std::strcpy(right, other.right);\n"
        "    }\n\n"
        "    LabelPair& operator=(const LabelPair& other)\n"
        "    {\n"
        "        if (this != &other)\n"
        "        {\n"
        "            delete[] left;\n"
        "            left = new char[std::strlen(other.left) + 1];\n"
        "            std::strcpy(left, other.left);\n"
        "            delete[] right;\n"
        "            right = new char[std::strlen(other.right) + 1];\n"
        "            std::strcpy(right, other.right);\n"
        "        }\n"
        "        return *this;\n"
        "    }\n\n"
        "    ~LabelPair()\n"
        "    {\n"
        "        delete[] left;\n"
        "        delete[] right;\n"
        "    }\n\n"
        "    void appendRight(const char* suffix)\n"
        "    {\n"
        "        std::strcat(right, suffix);\n"
        "    }\n\n"
        "    const char* leftText() const { return left; }\n"
        "    const char* rightText() const { return right; }\n\n"
        "private:\n"
        "    char* left;\n"
        "    char* right;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string left;"), "first owned text member should become std::string\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "std::string right;"), "second owned text member should become std::string\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "right += suffix;"), "strcat on converted text member should become +=\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return left.c_str();"), "compatibility getter should return c_str for first string");
    require(contains(result.modernCode, "return right.c_str();"), "compatibility getter should return c_str for second string");
    require(!contains(result.modernCode, "char* left;"), "first raw char* member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "char* right;"), "second raw char* member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "new char["), "manual text allocation should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[]"), "manual text cleanup should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::strcpy"), "strcpy should be removed after string conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::strcat"), "strcat should be removed after string conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "~LabelPair()"), "cleanup-only destructor should be removed after all text buffers convert");
    require(!contains(result.modernCode, "LabelPair(const LabelPair& other)"), "cleanup-only copy constructor should be removed after all text buffers convert");
    require(!contains(result.modernCode, "operator=(const LabelPair& other)"), "cleanup-only copy assignment should be removed after all text buffers convert");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed when no C-string APIs remain");
    require(hasAppliedRule(result, "Class raw char buffer to std::string"), "class string buffer conversion should be tracked");
    require(hasAppliedRule(result, "Rule of Zero after string buffer modernization"), "safe Rule of Zero cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "multi-member char* to string modernization should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testRuleOfZeroPreservesCleanupWhenRawCharOwnershipRemains()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class MixedBuffers\n"
        "{\n"
        "public:\n"
        "    MixedBuffers(const char* input, int size)\n"
        "    {\n"
        "        title = new char[std::strlen(input) + 1];\n"
        "        std::strcpy(title, input);\n"
        "        bytes = new char[size];\n"
        "    }\n\n"
        "    ~MixedBuffers()\n"
        "    {\n"
        "        delete[] title;\n"
        "        delete[] bytes;\n"
        "    }\n\n"
        "    const char* titleText() const { return title; }\n"
        "private:\n"
        "    char* title;\n"
        "    char* bytes;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string title;"), "clear text buffer should become std::string\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "char* bytes;"), "unclear raw buffer should be preserved\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "~MixedBuffers()"), "destructor should remain while raw owning buffer remains\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "delete[] bytes;"), "cleanup for preserved raw buffer should remain\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[] title;"), "cleanup for converted string buffer should be removed\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return title.c_str();"), "getter for converted string should preserve C-string API");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "mixed converted/preserved char buffer class should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testStringModernizationRemovesTemporaryConcatBuffer()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class TextLog\n"
        "{\n"
        "public:\n"
        "    explicit TextLog(const char* input)\n"
        "    {\n"
        "        text = new char[std::strlen(input) + 1];\n"
        "        std::strcpy(text, input);\n"
        "    }\n\n"
        "    ~TextLog()\n"
        "    {\n"
        "        delete[] text;\n"
        "    }\n\n"
        "    void append(const char* suffix)\n"
        "    {\n"
        "        char* combined = new char[std::strlen(text) + std::strlen(suffix) + 1];\n"
        "        std::strcpy(combined, text);\n"
        "        std::strcat(combined, suffix);\n"
        "        delete[] text;\n"
        "        text = combined;\n"
        "    }\n\n"
        "    const char* c_str() const { return text; }\n\n"
        "private:\n"
        "    char* text;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string text;"),
            "owned text member should become std::string\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "text += suffix;"),
            "temporary concatenation buffer should become std::string append\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return text.c_str();"),
            "C-string compatibility getter should return c_str()\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "combined"), "temporary concat buffer should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "new char["), "temporary char allocation should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::strcpy"), "strcpy should be removed after string conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "std::strcat"), "strcat should be removed after string conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[] text"), "delete[] for converted string member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed when no C-string APIs remain");
    require(hasAppliedRule(result, "Temporary C-string concat buffer to std::string append"),
            "temporary concat buffer cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "temporary concat buffer cleanup should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
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

void testLoopModernizationSafetyBoundaries()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult memberAccessResult = converter.convert(
        "#include <cstddef>\n"
        "#include <vector>\n"
        "struct Item\n"
        "{\n"
        "    int count = 0;\n"
        "    int read() const { return count; }\n"
        "    void bump() { ++count; }\n"
        "};\n"
        "template <typename Container>\n"
        "int readAll(const Container& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (typename Container::const_iterator it = values.begin(); it != values.end(); ++it) {\n"
        "        total += it->read();\n"
        "    }\n"
        "    return total;\n"
        "}\n"
        "void bumpAll(std::vector<Item>& items)\n"
        "{\n"
        "    for (std::vector<Item>::iterator it = items.begin(); it != items.end(); ++it) {\n"
        "        it->bump();\n"
        "        it->count += 1;\n"
        "    }\n"
        "}\n"
        "int sumIndex(const std::vector<int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::size_t index = 0; index < values.size(); ++index) {\n"
        "        total += values[index];\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);

    require(contains(memberAccessResult.modernCode, "for (const auto& value : values)"),
            "typename const_iterator loop should become const range-for");
    require(contains(memberAccessResult.modernCode, "total += value.read();"),
            "iterator member access should be rewritten to element member access");
    require(contains(memberAccessResult.modernCode, "for (auto& item : items)"),
            "mutable iterator member-call loop should become auto& range-for");
    require(contains(memberAccessResult.modernCode, "item.bump();"), "iterator method call should use range variable");
    require(contains(memberAccessResult.modernCode, "item.count += 1;"), "iterator member assignment should use range variable");
    require(contains(memberAccessResult.modernCode, "for (const auto& value : values)"),
            "same-line opening brace index loop should become range-for");
    require(!contains(memberAccessResult.modernCode, "it->"), "converted iterator loops should not leave iterator arrow access");
    if (memberAccessResult.compileVerificationEnabled) {
        require(memberAccessResult.compileVerificationPassed, "member-access loop modernization should pass syntax verification");
    }

    const ConversionResult unsafeResult = converter.convert(
        "#include <vector>\n"
        "void unsafeLoops(std::vector<int>& values, const std::vector<int>& other)\n"
        "{\n"
        "    for (auto it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        if (*it == 0)\n"
        "        {\n"
        "            values.erase(it);\n"
        "        }\n"
        "    }\n"
        "    for (auto it = values.rbegin(); it != values.rend(); ++it)\n"
        "    {\n"
        "        *it += 1;\n"
        "    }\n"
        "    for (std::size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        values[index] += other[index];\n"
        "    }\n"
        "}\n",
        options);

    require(contains(unsafeResult.modernCode, "values.erase(it);"),
            "erase iterator loop should remain explicit");
    require(contains(unsafeResult.modernCode, "values.rbegin()"),
            "reverse iterator loop should remain explicit");
    require(contains(unsafeResult.modernCode, "other[index]"),
            "multi-container index loop should remain explicit");
    require(hasSuggestionRule(unsafeResult, "Explicit iterator loop to range-based for"),
            "unsafe erase loop should emit loop modernization suggestion");
    if (unsafeResult.compileVerificationEnabled) {
        require(unsafeResult.compileVerificationPassed, "unsafe loop preservation sample should still compile");
    }
}

void testManualGrowthPipelineConverges()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "class BufferStore\n"
        "{\n"
        "public:\n"
        "    BufferStore()\n"
        "        : values(nullptr), count(0), capacity(2)\n"
        "    {\n"
        "        values = new int[capacity];\n"
        "    }\n\n"
        "    BufferStore(const BufferStore& other)\n"
        "        : values(new int[other.capacity]), count(other.count), capacity(other.capacity)\n"
        "    {\n"
        "        for (int index = 0; index < count; ++index)\n"
        "        {\n"
        "            values[index] = other.values[index];\n"
        "        }\n"
        "    }\n\n"
        "    ~BufferStore()\n"
        "    {\n"
        "        if (values != nullptr)\n"
        "        {\n"
        "            delete[] values;\n"
        "            values = nullptr;\n"
        "        }\n"
        "    }\n\n"
        "    void append(int value)\n"
        "    {\n"
        "        if (count == capacity)\n"
        "        {\n"
        "            int nextCapacity = capacity * 2;\n"
        "            int* grown = new int[nextCapacity];\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                grown[index] = values[index];\n"
        "            }\n"
        "            delete[] values;\n"
        "            values = grown;\n"
        "            capacity = nextCapacity;\n"
        "        }\n"
        "        values[count] = value;\n"
        "        ++count;\n"
        "    }\n\n"
        "    int size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "private:\n"
        "    int* values;\n"
        "    int count;\n"
        "    int capacity;\n"
        "};\n\n"
        "int main()\n"
        "{\n"
        "    BufferStore store;\n"
        "    store.append(1);\n"
        "    store.append(2);\n"
        "    store.append(3);\n"
        "    return store.size();\n"
        "}\n",
        options);

    require(diagnosticsContain(result, "VectorParadigmRewritePass"),
            "pass tracing should include vector paradigm cleanup for manual growth fixture");
    require(diagnosticsContain(result, "hash_before=") && diagnosticsContain(result, "hash_after="),
            "pass tracing should include before/after hashes");
    require(!diagnosticsContain(result, "iteration-limit-exceeded"),
            "manual growth modernization should converge without hitting iteration guard");
    require(contains(result.modernCode, "std::vector<int> values;"),
            "manual dynamic array should modernize to vector storage");
    require(!contains(result.modernCode, "grown[index]"),
            "manual growth copy loop should not leave orphaned temp buffer references");
    require(!contains(result.modernCode, "delete[] values"),
            "vector modernization should remove manual delete[] cleanup");
    require(contains(result.modernCode, "values.push_back(value);"),
            "append logic should converge to vector push_back");
    if (result.compileVerificationEnabled) {
        require(result.compileVerificationPassed,
                "manual growth convergence fixture should pass syntax verification\nCompiler output:\n" + result.compilerOutput
                    + "\nConverted code:\n" + result.modernCode);
    }
}

void testCanBufferManualGrowthReproTerminates()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const auto started = std::chrono::steady_clock::now();
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <cstring>\n\n"
        "#define INITIAL_MAX 2\n\n"
        "typedef struct _CanFrame {\n"
        "    unsigned long arbitrationId;\n"
        "    char dataPayload[8];\n"
        "} CanFrame;\n\n"
        "class CanBufferManager {\n"
        "private:\n"
        "    CanFrame* backingStore;\n"
        "    int count;\n"
        "    int capacity;\n\n"
        "public:\n"
        "    CanBufferManager() {\n"
        "        count = 0;\n"
        "        capacity = INITIAL_MAX;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "    }\n\n"
        "    CanBufferManager(const CanBufferManager& other) {\n"
        "        count = other.count;\n"
        "        capacity = other.capacity;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "        for (int i = 0; i < count; ++i) {\n"
        "            backingStore[i] = other.backingStore[i];\n"
        "        }\n"
        "    }\n\n"
        "    ~CanBufferManager() {\n"
        "        if (backingStore != NULL) {\n"
        "            delete[] backingStore;\n"
        "            backingStore = NULL;\n"
        "        }\n"
        "    }\n\n"
        "    bool appendFrame(unsigned long id, const char* bytes) {\n"
        "        if (count >= capacity) {\n"
        "            int newCap = capacity * 2;\n"
        "            CanFrame* newStore = new CanFrame[newCap];\n"
        "            for (int i = 0; i < count; ++i) {\n"
        "                newStore[i] = backingStore[i];\n"
        "            }\n"
        "            delete[] backingStore;\n"
        "            backingStore = newStore;\n"
        "            capacity = newCap;\n"
        "        }\n\n"
        "        backingStore[count].arbitrationId = id;\n"
        "        std::strncpy(backingStore[count].dataPayload, bytes, 7);\n"
        "        backingStore[count].dataPayload[7] = '\\0';\n\n"
        "        count++;\n"
        "        return true;\n"
        "    }\n\n"
        "    int getFrameCount() const { return count; }\n"
        "    CanFrame* getFrame(int index) { return &backingStore[index]; }\n"
        "};\n\n"
        "int main() {\n"
        "    CanBufferManager buffer;\n"
        "    buffer.appendFrame(0x7DF, \"\\x02\\x01\\x0D\\x00\\x00\\x00\\x00\");\n"
        "    buffer.appendFrame(0x7E8, \"\\x03\\x41\\x0D\\x32\\x00\\x00\\x00\");\n\n"
        "    std::cout << \"Buffered Frames: \" << buffer.getFrameCount() << std::endl;\n"
        "    return 0;\n"
        "}\n",
        options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    require(elapsed.count() < 15000,
            "CanBuffer manual-growth repro should terminate quickly; elapsed_ms=" + std::to_string(elapsed.count()));
    require(diagnosticsContain(result, "START PASS VectorParadigmRewritePass"),
            "pass tracing should print START PASS for vector paradigm cleanup");
    require(diagnosticsContain(result, "END PASS VectorParadigmRewritePass"),
            "pass tracing should print END PASS for vector paradigm cleanup");
    require(diagnosticsContain(result, "hash_before=") && diagnosticsContain(result, "hash_after="),
            "pass tracing should include AST/code hashes around passes");
    require(!diagnosticsContain(result, "iteration-limit-exceeded"),
            "CanBuffer repro should converge without hitting the iteration guard");
    require(!contains(result.modernCode, "newStore[i]"),
            "manual growth temp buffer loop should not remain after conversion");
    require(!contains(result.modernCode, "backingStore = newStore"),
            "raw temp buffer assignment to converted storage should not remain");
    require(result.compileVerificationEnabled, "compile verification should run for CanBuffer repro");
    require(result.compileVerificationPassed,
            "CanBuffer repro should pass syntax verification\nCompiler output:\n" + result.compilerOutput
                + "\nConverted code:\n" + result.modernCode);
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
    require(!contains(result.modernCode, "ArrayOwner(const ArrayOwner&) = default;"),
            "manual copy constructor should be removed once vector copy is enough");
    require(contains(result.modernCode, "ArrayOwner& operator=(const ArrayOwner&) = default;"), "manual copy assignment should become defaulted");
    require(!contains(result.modernCode, "~ArrayOwner()"), "cleanup-only destructor should be removed after dependent cleanup");
    require(hasAppliedRule(result, "Replace raw array allocation with vector resize"), "dependent vector allocation cleanup should be tracked");
    require(hasAppliedRule(result, "Remove nullptr check after vector modernization"), "dependent vector nullptr cleanup should be tracked");
    require(hasAppliedRule(result, "Remove obsolete copy constructor after vector modernization"), "copy constructor cleanup should be tracked");
    require(hasAppliedRule(result, "Post-vector Rule of Zero copy constructor removal"), "post-vector copy constructor polish should be tracked");
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

void testDependentStringCleanupRewritesConcatAndSymmetricCompare()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "class TextBuffer\n"
        "{\n"
        "public:\n"
        "    void reset(const char* input)\n"
        "    {\n"
        "        std::strcpy(text, input);\n"
        "    }\n\n"
        "    void append(const char* suffix)\n"
        "    {\n"
        "        std::strcat(text, suffix);\n"
        "    }\n\n"
        "    bool sameAs(const char* input) const\n"
        "    {\n"
        "        return std::strcmp(input, text) == 0;\n"
        "    }\n\n"
        "private:\n"
        "    char text[64];\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::string text;"), "char buffer should become std::string for C-string concat/compare usage");
    require(contains(result.modernCode, "text = input;"), "strcpy destination should become string assignment");
    require(contains(result.modernCode, "text += suffix;"), "strcat destination should become string append");
    require(contains(result.modernCode, "return input == text;"), "strcmp with converted string as second argument should become symmetric string comparison");
    require(!contains(result.modernCode, "strcat"), "strcat should not remain against converted std::string");
    require(!contains(result.modernCode, "strcmp"), "strcmp should not remain against converted std::string");
    require(!contains(result.modernCode, "strcpy"), "strcpy should not remain against converted std::string");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed when C-string APIs are gone");
    require(hasAppliedRule(result, "Replace C-string concatenation with string append"), "strcat cleanup should be tracked");
    require(hasAppliedRule(result, "Replace C-string comparison with string comparison"), "symmetric strcmp cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "dependent string concat/compare cleanup sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testStringCleanupHandlesConcatOnlyTextUsage()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "struct Message\n"
        "{\n"
        "    char text[48];\n"
        "};\n\n"
        "void append(Message& message, const char* suffix)\n"
        "{\n"
        "    strcat(message.text, suffix);\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::string text;"), "strcat-only text storage should still become std::string");
    require(contains(result.modernCode, "message.text += suffix;"), "unqualified strcat should become std::string append");
    require(!contains(result.modernCode, "strcat"), "unqualified strcat should be removed after string conversion");
    require(!contains(result.modernCode, "#include <cstring>"), "cstring include should be removed after strcat cleanup");
    require(hasAppliedRule(result, "Replace C-string concatenation with string append"), "strcat-only cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "strcat-only string conversion should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
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

void testVectorMemberGetterCascadesToContainerReference()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "struct Item\n"
        "{\n"
        "    int value;\n"
        "};\n\n"
        "class Store\n"
        "{\n"
        "public:\n"
        "    explicit Store(int capacity)\n"
        "        : count(capacity)\n"
        "    {\n"
        "        items = new Item[count];\n"
        "    }\n\n"
        "    const Item* getItems() const { return items; }\n"
        "    int getCount() const { return count; }\n\n"
        "    ~Store()\n"
        "    {\n"
        "        delete[] items;\n"
        "    }\n\n"
        "private:\n"
        "    int count;\n"
        "    Item* items;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<Item> items;"), "array member should become vector");
    require(contains(result.modernCode, "const std::vector<Item>& getItems() const { return items; }"),
            "whole-collection getter should return const vector reference");
    require(contains(result.modernCode, "std::size_t getCount() const { return items.size(); }"),
            "count getter should cascade to vector size");
    require(!contains(result.modernCode, "const Item* getItems()"), "getter should not expose old array pointer type");
    require(!contains(result.modernCode, "~Store()"), "cleanup-only destructor should be removed");
    require(hasAppliedRule(result, "Vector getter return type cascade"), "vector getter cascade should be tracked");
    require(hasAppliedRule(result, "Count getter to vector size"), "count getter cascade should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "vector member getter cascade sample should pass syntax verification");
    }
}

void testLocalCollectionDoesNotLeakIntoUnrelatedClassGetter()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "class Status\n"
        "{\n"
        "public:\n"
        "    int getLength() const { return length; }\n"
        "private:\n"
        "    int length;\n"
        "};\n\n"
        "void build(int count)\n"
        "{\n"
        "    int* values = new int[count];\n"
        "    values[0] = 1;\n"
        "    delete[] values;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "int getLength() const { return length; }"),
            "unrelated class getter should not reference a local vector from another function");
    require(!contains(result.modernCode, "return values.size();"),
            "local collection should not leak into unrelated class API");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "scope leak prevention sample should pass syntax verification");
    }
}

void testLengthGetterForIndependentMemberIsPreserved()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "struct Item {};\n"
        "class Store\n"
        "{\n"
        "public:\n"
        "    explicit Store(int count)\n"
        "        : length(count)\n"
        "    {\n"
        "        items = new Item[count];\n"
        "    }\n"
        "    int getLength() const { return length; }\n"
        "    ~Store()\n"
        "    {\n"
        "        delete[] items;\n"
        "    }\n"
        "private:\n"
        "    int length;\n"
        "    Item* items;\n"
        "};\n",
        structuralOptions());

    require(contains(result.modernCode, "int getLength() const { return length; }"),
            "independent length member getter should not be replaced with vector.size()");
    require(!contains(result.modernCode, "getLength() const { return items.size(); }"),
            "member API cascade should not guess that length mirrors vector storage");
}

void testScopeLeakValidatorRepairsWrongInClassSymbolWhenSafe()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "items",
        "Item*",
        "std::vector<Item>",
        "Store",
        true,
        "Raw dynamic array to std::vector",
        {"member API cascade"},
        {},
        false,
    });

    const std::string code =
        "#include <vector>\n"
        "struct Item {};\n"
        "class Store\n"
        "{\n"
        "public:\n"
        "    std::size_t size() const { return localItems.size(); }\n"
        "private:\n"
        "    std::vector<Item> items;\n"
        "};\n"
        "void unrelated()\n"
        "{\n"
        "    std::vector<Item> localItems;\n"
        "}\n";

    std::vector<ConversionChange> changes;
    const ScopeLeakValidationPass validator;
    const std::string fixed = validator.validate(code,
                                                 context,
                                                 "error: use of undeclared identifier 'localItems'",
                                                 changes);

    require(contains(fixed, "return items.size();"), "scope leak validator should repair to visible member when unambiguous");
    require(!contains(fixed, "return localItems.size();"), "out-of-scope local reference should be removed");
    require(hasAppliedRule(changes, "Scope leak validation"), "scope leak repair should be tracked");
}

void testScopeAwareSymbolTableTracksClassMembers()
{
    const ScopeAwareSymbolTable table = ScopeAwareSymbolTable::build(
        "class Store\n"
        "{\n"
        "public:\n"
        "    void method()\n"
        "    {\n"
        "        int localValue = 0;\n"
        "    }\n"
        "private:\n"
        "    int count;\n"
        "};\n");

    require(table.hasClassMember("Store", "count"), "scope table should track class data members");
    require(!table.hasClassMember("Store", "localValue"), "scope table should not treat method locals as class members");
}

void testScopeAwareSymbolTableTracksFunctionLocals()
{
    const ScopeAwareSymbolTable table = ScopeAwareSymbolTable::build(
        "int globalCounter;\n"
        "void first()\n"
        "{\n"
        "    int localCount = 0;\n"
        "    double ratio;\n"
        "}\n"
        "void second()\n"
        "{\n"
        "    int other = 1;\n"
        "}\n");

    require(table.hasFunctionLocal("first", "localCount"), "scope table should track function-local initialized values");
    require(table.hasFunctionLocal("first", "ratio"), "scope table should track function-local declarations");
    require(!table.hasFunctionLocal("second", "localCount"), "function-local symbols should not leak across functions");
    const std::vector<SymbolInfo> visible = table.visibleSymbols("first");
    require(std::any_of(visible.begin(), visible.end(), [](const SymbolInfo& symbol) {
                return symbol.name == "globalCounter" && symbol.scopeKind == SymbolScopeKind::Global;
            }),
            "visible lookup should include globals");
    require(std::any_of(visible.begin(), visible.end(), [](const SymbolInfo& symbol) {
                return symbol.name == "localCount" && symbol.scopeKind == SymbolScopeKind::FunctionLocal;
            }),
            "visible lookup should include locals for the requested function");
}

void testVectorRawBufferGetterUsesDataOnlyWhenIntentIsClear()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class Buffer\n"
        "{\n"
        "public:\n"
        "    explicit Buffer(int capacity)\n"
        "    {\n"
        "        values = new int[capacity];\n"
        "    }\n\n"
        "    int* rawData() { return values; }\n\n"
        "    ~Buffer()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "private:\n"
        "    int* values;\n"
        "};\n",
        structuralOptions());

    require(contains(result.modernCode, "std::vector<int> values;"), "raw buffer storage should become vector");
    require(contains(result.modernCode, "int* rawData() { return values.data(); }"),
            "raw-intent getter should use vector.data()");
    require(hasAppliedRule(result, "Vector raw buffer getter cascade"), "raw buffer getter cascade should be tracked");
}

void testRuntimeVectorGetterIsNotConstexpr()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "class Numbers\n"
        "{\n"
        "public:\n"
        "    constexpr int getCount() const { return values.size(); }\n"
        "private:\n"
        "    std::vector<int> values;\n"
        "};\n",
        options);

    require(!contains(result.modernCode, "constexpr int getCount()"), "runtime vector size getter should not remain constexpr");
    require(contains(result.modernCode, "int getCount() const { return values.size(); }")
                || contains(result.modernCode, "std::size_t getCount() const { return values.size(); }"),
            "getter should remain available after constexpr cleanup");
    require(hasAppliedRule(result, "Constexpr correctness cleanup"), "constexpr cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, "constexpr cleanup sample should pass syntax verification");
    }
}

void testMalformedEmptyDestructorBlocksAreRemoved()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "class EmptyCleanup\n"
        "{\n"
        "public:\n"
        "    ~EmptyCleanup()\n"
        "    {\n"
        "        {\n"
        "        }\n"
        "    }\n"
        "};\n",
        structuralOptions());

    require(!contains(result.modernCode, "~EmptyCleanup()"), "malformed empty destructor should be removed");
    require(hasAppliedRule(result, "Rule of Zero destructor cleanup"), "empty destructor cleanup should be tracked");
}

void testContainerPolishModernizesInitializerAndMapInsert()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.useStructuredBindings = true;
    const ConversionResult result = converter.convert(
        "#include <map>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "void configure(std::map<int, std::string>& names)\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    values.push_back(1);\n"
        "    values.push_back(2);\n"
        "    values.push_back(3);\n"
        "    names.insert(std::pair<int, std::string>(1, \"one\"));\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values{1, 2, 3};"),
            "consecutive push_back calls should become vector initializer list");
    require(contains(result.modernCode, "names.emplace(1, \"one\");"), "pair insertion should become emplace");
    require(hasAppliedRule(result, "Repeated push_back to initializer list"), "initializer-list polish should be tracked");
    require(hasAppliedRule(result, "Map pair insert to emplace"), "map insertion polish should be tracked");
}

void testMapIteratorLoopUsesStructuredBindingAndNewline()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.useStructuredBindings = true;
    const ConversionResult result = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void print(const std::map<int, int>& values)\n"
        "{\n"
        "    for (std::map<int, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << it->first << it->second << std::endl;\n"
        "    }\n"
        "}\n",
        options);

    require(contains(result.modernCode, "for (const auto& [key, value] : values)"),
            "map iterator loop should become structured binding range loop");
    require(contains(result.modernCode, "std::cout << key << value << '\\n';"),
            "simple std::endl output should become newline character");
    require(!contains(result.modernCode, "std::endl"), "simple endl should not remain");
    require(hasAppliedRule(result, "Map iterator loop to structured binding"), "map iterator rewrite should be tracked");
    require(hasAppliedRule(result, "Stream newline cleanup") || hasAppliedRule(result, "Map iterator loop to structured binding"),
            "newline cleanup should be tracked by polish or loop rewrite");
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
    require(contains(result.modernCode, "values.reserve(2);"), "allocation-only capacity should collapse to reserve literal");
    require(contains(result.modernCode, "values.push_back(value);"), "indexed append should become push_back");
    require(contains(result.modernCode, "return values.size();"), "count mirror getter should become vector size");
    require(!contains(result.modernCode, "std::size_t count;"), "stale count mirror should be removed");
    require(!contains(result.modernCode, "std::size_t capacity;"), "stale allocation capacity should be removed");
    require(!contains(result.modernCode, "count(0)"), "stale count initializer should be removed");
    require(!contains(result.modernCode, "capacity(2)"), "stale capacity initializer should be removed");
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

void testVectorPostIncrementAppendBecomesPushBack()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "class PacketLog\n"
        "{\n"
        "public:\n"
        "    PacketLog()\n"
        "        : values(nullptr), count(0), capacity(2)\n"
        "    {\n"
        "        values = new int[capacity];\n"
        "    }\n\n"
        "    void append(int value)\n"
        "    {\n"
        "        if (count >= capacity)\n"
        "        {\n"
        "            int expanded = capacity * 2;\n"
        "            int* replacement = new int[expanded];\n"
        "            for (int index = 0; index < count; ++index)\n"
        "            {\n"
        "                replacement[index] = values[index];\n"
        "            }\n"
        "            delete[] values;\n"
        "            values = replacement;\n"
        "            capacity = expanded;\n"
        "        }\n"
        "        values[count++] = value;\n"
        "    }\n\n"
        "    int size() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "    ~PacketLog()\n"
        "    {\n"
        "        delete[] values;\n"
        "    }\n\n"
        "private:\n"
        "    int* values;\n"
        "    int count;\n"
        "    int capacity;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"), "post-increment append storage should become vector");
    require(contains(result.modernCode, "values.reserve(2);"), "append-style allocation capacity should collapse to reserve literal");
    require(contains(result.modernCode, "values.push_back(value);"), "post-increment indexed append should become push_back");
    require(contains(result.modernCode, "return values.size();"), "count mirror getter should become vector size");
    require(!contains(result.modernCode, "int count;"), "stale count mirror member should be removed");
    require(!contains(result.modernCode, "int capacity;"), "stale allocation capacity member should be removed");
    require(!contains(result.modernCode, "count(0)"), "stale count initializer should be removed");
    require(!contains(result.modernCode, "capacity(2)"), "stale capacity initializer should be removed");
    require(!contains(result.modernCode, "values[count++]"), "post-increment vector indexing should not remain");
    require(!contains(result.modernCode, "replacement"), "manual growth temp buffer should be removed");
    require(!contains(result.modernCode, "expanded"), "manual growth capacity temporary should be removed");
    require(!contains(result.modernCode, "delete[] values"), "delete[] for vector storage should be removed");
    require(!contains(result.modernCode, "~PacketLog()"), "cleanup-only destructor should be removed");
    require(hasAppliedRule(result, "Indexed append to vector push_back"), "post-increment push_back rewrite should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "post-increment append vector modernization should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testLocalRawArrayAppendUsesReserveAndPushBack()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "int buildValues(int limit)\n"
        "{\n"
        "    int* values = new int[limit];\n"
        "    int count = 0;\n"
        "    values[count++] = 3;\n"
        "    values[count++] = 5;\n"
        "    delete[] values;\n"
        "    return count;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "std::vector<int> values;"),
            "local append array should become an empty vector\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "values.reserve(limit);"), "local append array capacity should become reserve");
    require(contains(result.modernCode, "values.push_back(3);"), "first local append should become push_back");
    require(contains(result.modernCode, "values.push_back(5);"), "second local append should become push_back");
    require(contains(result.modernCode, "return values.size();"), "local count mirror return should become vector size");
    require(!contains(result.modernCode, "std::vector<int> values(limit);"), "append-style vector should not be pre-sized");
    require(!contains(result.modernCode, "values[count++]"), "local post-increment append indexing should not remain");
    require(!contains(result.modernCode, "delete[] values"), "local delete[] should be removed");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "local post-increment append vector modernization should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
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

void testContainerModernizationCleanupRemovesGenericGrowthSystem()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct Slot\n"
        "{\n"
        "    int id;\n"
        "    int level;\n"
        "};\n\n"
        "class SlotLog\n"
        "{\n"
        "public:\n"
        "    SlotLog()\n"
        "        : records(nullptr), used(0), allocationLimit(2)\n"
        "    {\n"
        "        records = new Slot[allocationLimit];\n"
        "    }\n\n"
        "    SlotLog(const SlotLog& other)\n"
        "        : records(new Slot[other.allocationLimit]), used(other.used), allocationLimit(other.allocationLimit)\n"
        "    {\n"
        "        for (int cursor = 0; cursor < used; ++cursor)\n"
        "        {\n"
        "            records[cursor] = other.records[cursor];\n"
        "        }\n"
        "    }\n\n"
        "    ~SlotLog()\n"
        "    {\n"
        "        if (records != nullptr)\n"
        "        {\n"
        "            delete[] records;\n"
        "            records = nullptr;\n"
        "        }\n"
        "    }\n\n"
        "    void add(int id, int level)\n"
        "    {\n"
        "        if (used >= allocationLimit)\n"
        "        {\n"
        "            int scratchLimit = allocationLimit * 2;\n"
        "            Slot* scratchStore = new Slot[scratchLimit];\n"
        "            for (int cursor = 0; cursor < used; ++cursor)\n"
        "            {\n"
        "                scratchStore[cursor] = records[cursor];\n"
        "            }\n"
        "            delete[] records;\n"
        "            records = scratchStore;\n"
        "            allocationLimit = scratchLimit;\n"
        "        }\n"
        "        records[used].id = id;\n"
        "        records[used].level = level;\n"
        "        used++;\n"
        "    }\n\n"
        "    int size() const\n"
        "    {\n"
        "        return used;\n"
        "    }\n\n"
        "    Slot* get(int index)\n"
        "    {\n"
        "        return &records[index];\n"
        "    }\n\n"
        "private:\n"
        "    Slot* records;\n"
        "    int used;\n"
        "    int allocationLimit;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<Slot> records;"),
            "raw array member should become std::vector\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "Slot appendedItem{};"), "field append should create a local value object");
    require(contains(result.modernCode, "records.push_back(appendedItem);"), "field append should use vector push_back");
    require(contains(result.modernCode, "return records.size();"), "count getter should return vector.size()");
    require(contains(result.modernCode, "return &records[index];")
                || contains(result.modernCode, "return &records[static_cast<std::size_t>(index)];"),
            "index accessor should return address of vector element\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "scratchStore"), "temporary growth buffer should be removed");
    require(!contains(result.modernCode, "scratchLimit"), "temporary capacity variable should be removed");
    require(!contains(result.modernCode, "new Slot["), "raw new[] should be removed");
    require(!contains(result.modernCode, "delete[] records"), "delete[] for vector storage should be removed");
    require(!contains(result.modernCode, "records = scratchStore"), "raw buffer assignment to vector should be removed");
    require(!contains(result.modernCode, "scratchStore[cursor] = records[cursor]"), "manual growth copy loop should be removed");
    require(!contains(result.modernCode, "records[used]"), "append-style vector indexing should not remain");
    require(!contains(result.modernCode, "~SlotLog()"), "cleanup-only destructor should be removed");
    require(!contains(result.modernCode, "records[cursor] = other.records[cursor]"), "manual copy constructor loop should be removed/defaulted");
    require(hasAppliedRule(result, "Container modernization cleanup"),
            "container cleanup consistency pass should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "container modernization cleanup consistency sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testContainerModernizationCleanupRemovesSameLineGrowthFragments()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct SensorRecord\n"
        "{\n"
        "    int channel;\n"
        "    int reading;\n"
        "};\n\n"
        "class MeasurementStore\n"
        "{\n"
        "public:\n"
        "    MeasurementStore() : entries(nullptr), count(0), capacity(2)\n"
        "    {\n"
        "        entries = new SensorRecord[capacity];\n"
        "    }\n\n"
        "    ~MeasurementStore()\n"
        "    {\n"
        "        delete[] entries;\n"
        "    }\n\n"
        "    bool add(int channel, int reading)\n"
        "    {\n"
        "        if (count >= capacity) {\n"
        "            int newCapacity = capacity * 2;\n"
        "            SensorRecord* temp = new SensorRecord[newCapacity];\n"
        "            for (int i = 0; i < count; ++i) {\n"
        "                temp[i] = entries[i];\n"
        "            }\n"
        "            delete[] entries;\n"
        "            entries = temp;\n"
        "            capacity = newCapacity;\n"
        "        }\n"
        "        entries[count].channel = channel;\n"
        "        entries[count].reading = reading;\n"
        "        count++;\n"
        "        return true;\n"
        "    }\n\n"
        "    int getCount() const\n"
        "    {\n"
        "        return count;\n"
        "    }\n\n"
        "private:\n"
        "    SensorRecord* entries;\n"
        "    int count;\n"
        "    int capacity;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<SensorRecord> entries;"),
            "raw SensorRecord array member should become vector\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "entries.push_back("),
            "same-line growth fixture should append with vector push_back\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return entries.size();"),
            "count getter should return vector.size() after cleanup\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "newCapacity"), "temporary capacity variable should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "newCap"), "newCap-style capacity fragments should be absent\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "SensorRecord* temp"), "temporary raw growth buffer should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "temp = new"), "raw temp allocation should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "new SensorRecord["), "raw new[] should be removed after vector conversion\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[] entries"), "delete[] for converted vector should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "temp[i] = entries[i]"), "manual copy loop into temp buffer should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "capacity = newCapacity"), "stale capacity update should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "for (int i = 0; i < count; ++i) {\n        }"),
            "empty manual copy loop should not remain\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "entries[count]"), "append-style vector indexing should not remain\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "~MeasurementStore()"), "cleanup-only destructor should be removed");
    require(hasAppliedRule(result, "Container modernization cleanup"),
            "container modernization cleanup should report stale growth fragment cleanup");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "same-line growth fragment cleanup sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testContainerModernizationCleanupPolishesVectorBackedClass()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct TelemetryRecord\n"
        "{\n"
        "    int id;\n"
        "    int value;\n"
        "};\n\n"
        "class TelemetryStore\n"
        "{\n"
        "public:\n"
        "    TelemetryStore()\n"
        "        : readings(nullptr), itemCount(0), reserveSlots(4)\n"
        "    {\n"
        "        readings = new TelemetryRecord[reserveSlots];\n"
        "    }\n\n"
        "    TelemetryStore(const TelemetryStore& other)\n"
        "        : readings(new TelemetryRecord[other.reserveSlots]), itemCount(other.itemCount), reserveSlots(other.reserveSlots)\n"
        "    {\n"
        "        for (int i = 0; i < itemCount; ++i)\n"
        "        {\n"
        "            readings[i] = other.readings[i];\n"
        "        }\n"
        "    }\n\n"
        "    ~TelemetryStore()\n"
        "    {\n"
        "        delete[] readings;\n"
        "    }\n\n"
        "    void append(int id, int value)\n"
        "    {\n"
        "        readings[itemCount].id = id;\n"
        "        readings[itemCount].value = value;\n"
        "        itemCount++;\n"
        "    }\n\n"
        "    int getCount() const\n"
        "    {\n"
        "        return itemCount;\n"
        "    }\n\n"
        "    TelemetryRecord* getRecord(int index)\n"
        "    {\n"
        "        return &readings[index];\n"
        "    }\n\n"
        "private:\n"
        "    TelemetryRecord* readings;\n"
        "    int itemCount;\n"
        "    int reserveSlots;\n"
        "};\n",
        options);

    require(contains(result.modernCode, "std::vector<TelemetryRecord> readings;"),
            "raw telemetry storage should become vector\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "readings.reserve(4);"),
            "pure preallocation capacity member should collapse to reserve literal\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "readings.push_back("),
            "append should use vector push_back\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "std::size_t getCount() const { return readings.size(); }"),
            "count getter should be std::size_t and return vector.size()\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "const TelemetryRecord* getRecord(std::size_t index) const"),
            "record getter should be const and std::size_t-based\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "if (index < readings.size())"),
            "record getter should check vector bounds\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "return nullptr;"),
            "bounds-safe getter should return nullptr for out-of-range access\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "int itemCount;"), "stale count mirror member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "int reserveSlots;"), "stale allocation capacity member should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "itemCount(0)"), "stale count initializer should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "reserveSlots(4)"), "stale capacity initializer should be removed\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "TelemetryStore(const TelemetryStore&) = default;"),
            "explicit default copy constructor should be removed when vector copy is enough\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "\n\n\n"), "cleanup should not leave excessive blank lines\nConverted code:\n" + result.modernCode);
    require(hasAppliedRule(result, "Post-vector cleanup polish"),
            "post-vector cleanup polish should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "post-vector cleanup polish sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testPostVectorCleanupRemovesUnusedCapacityWithoutReserve()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "records",
        "Record*",
        "std::vector<Record>",
        "RecordStore",
        true,
        "Raw dynamic array to std::vector",
        {},
        {},
        true,
    });

    std::vector<ConversionChange> changes;
    const ContainerModernizationCleanupPass cleanupPass;
    const std::string updated = cleanupPass.rewrite(
        "#include <vector>\n"
        "#include <cstddef>\n"
        "struct Record\n"
        "{\n"
        "    int value;\n"
        "};\n\n"
        "class RecordStore\n"
        "{\n"
        "public:\n"
        "    RecordStore()\n"
        "        : allocationCapacity(8)\n"
        "    {\n"
        "    }\n\n"
        "    std::size_t getCount() const { return records.size(); }\n\n"
        "private:\n"
        "    std::vector<Record> records;\n"
        "    int allocationCapacity;\n"
        "};\n",
        context,
        changes);

    require(!contains(updated, "allocationCapacity"),
            "unused allocation-capacity member should be removed even when reserve already disappeared\nConverted code:\n"
                + updated);
    require(hasAppliedRule(changes, "Post-vector stale capacity member removal"),
            "stale capacity removal should be tracked");
}

void testSizeReturningGetterUpdatesSignedLoopIndex()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "#include <cstddef>\n"
        "struct Item\n"
        "{\n"
        "    int value;\n"
        "};\n\n"
        "class ItemStore\n"
        "{\n"
        "public:\n"
        "    std::size_t getCount() const { return items.size(); }\n"
        "private:\n"
        "    std::vector<Item> items;\n"
        "};\n\n"
        "int countItems(const ItemStore& store)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (int index = 0; index < store.getCount(); ++index)\n"
        "    {\n"
        "        total += 1;\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "for (std::size_t index = 0; index < store.getCount(); ++index)"),
            "loop index should follow std::size_t count getter after type propagation\nConverted code:\n"
                + result.modernCode);
    require(hasAppliedRule(result, "Signed loop index to std::size_t"),
            "signed loop cleanup should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "signed loop cleanup sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testCrossScopePropagationAdaptsVectorToRawArrayCallsite()
{
    TransformationContext context;
    context.registerTypeChange(TypeChangeRecord{
        "values",
        "int*",
        "std::vector<int>",
        "run",
        false,
        "Raw dynamic array to std::vector",
        {},
        {},
        true,
    });

    std::vector<ConversionChange> changes;
    const CrossScopeTypePropagationPass pass;
    const ModernizationOptions options = structuralOptions();
    const std::string fixed = pass.rewrite(
        "#include <vector>\n"
        "void consume(int* data, int count)\n"
        "{\n"
        "    if (count > 0)\n"
        "    {\n"
        "        data[0] = 1;\n"
        "    }\n"
        "}\n"
        "void run()\n"
        "{\n"
        "    std::vector<int> values;\n"
        "    values.push_back(0);\n"
        "    consume(values, 1);\n"
        "}\n",
        options,
        context,
        changes);

    require(contains(fixed, "consume(values.data(), static_cast<int>(values.size()));"),
            "converted vector should adapt to visible raw pointer/count APIs with data() and size()\nConverted code:\n"
                + fixed);
    require(!contains(fixed, "consume(values, 1);"),
            "stale direct vector-to-raw-array callsite should not remain\nConverted code:\n" + fixed);
    require(hasAppliedRule(changes, "Cross-scope vector raw-array callsite adaptation"),
            "vector raw-array callsite adaptation should be tracked");

    const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(fixed);
    require(verification.compilerUsed.empty() || verification.passed,
            "cross-scope vector/raw-array adaptation should pass syntax verification\nCompiler output:\n"
                + verification.output + "\nConverted code:\n" + fixed);
}

void testFinalFormatterCleansTransformedBlocks()
{
    const SemanticTypeValidationPass validationPass;
    ModernizationOptions options = structuralOptions();
    std::vector<ConversionChange> changes{
        ConversionChange{
            "Synthetic applied transformation",
            "before",
            "after",
            "test marker",
            true,
            false,
        },
    };

    const std::string formatted = validationPass.validateAndRepair(
        "struct Fixture{\n"
        "void run() override{\n"
        "if (true){\n"
        "return;\n"
        "}\n"
        "}\n"
        "};\n",
        options,
        changes);

    require(contains(formatted, "struct Fixture {"), "formatter should add space before class/struct brace\nOutput:\n" + formatted);
    require(contains(formatted, "    void run() override {"), "formatter should indent class members and fix override brace spacing\nOutput:\n" + formatted);
    require(contains(formatted, "        if (true) {"), "formatter should indent if blocks and add space before brace\nOutput:\n" + formatted);
    require(contains(formatted, "            return;"), "formatter should indent statements inside nested blocks\nOutput:\n" + formatted);
    require(!contains(formatted, "override{"), "formatter should not leave override glued to brace\nOutput:\n" + formatted);
    require(hasAppliedRule(changes, "Final transformation formatting cleanup"), "formatting cleanup should be tracked");
}

void testConstReturnPropagationUpdatesDirectReceivers()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;
    const ConversionResult result = converter.convert(
        "#include <cstddef>\n"
        "struct Entry\n"
        "{\n"
        "    int id;\n"
        "};\n\n"
        "class EntryStore\n"
        "{\n"
        "public:\n"
        "    EntryStore()\n"
        "        : entries(nullptr), entryCount(1)\n"
        "    {\n"
        "        entries = new Entry[entryCount];\n"
        "    }\n\n"
        "    ~EntryStore()\n"
        "    {\n"
        "        delete[] entries;\n"
        "    }\n\n"
        "    Entry* getEntry(int index)\n"
        "    {\n"
        "        return &entries[index];\n"
        "    }\n"
        "private:\n"
        "    Entry* entries;\n"
        "    int entryCount;\n"
        "};\n\n"
        "int readFirst(EntryStore& store)\n"
        "{\n"
        "    Entry* entry = store.getEntry(0);\n"
        "    if (entry != nullptr)\n"
        "    {\n"
        "        return entry->id;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        options);

    require(contains(result.modernCode, "const Entry* getEntry(std::size_t index) const"),
            "vector-backed getter should become a const pointer observer\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "const Entry* entry = store.getEntry(0);"),
            "direct receiver should follow the getter's const pointer return type\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "\n    Entry* entry = store.getEntry(0);"),
            "old mutable pointer receiver should not remain after const return propagation\nConverted code:\n" + result.modernCode);
    require(hasAppliedRule(result, "Const return receiver propagation"),
            "const receiver propagation should be tracked");
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                "const return receiver propagation sample should pass syntax verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

void testConstReturnPropagationHandlesReferencesAndSmartPointerRefs()
{
    const std::string code =
        "#include <memory>\n"
        "struct Item\n"
        "{\n"
        "    int value;\n"
        "};\n\n"
        "class Holder\n"
        "{\n"
        "public:\n"
        "    const Item& current() const\n"
        "    {\n"
        "        static Item item{};\n"
        "        return item;\n"
        "    }\n\n"
        "    const std::unique_ptr<Item>& owned() const\n"
        "    {\n"
        "        static std::unique_ptr<Item> item;\n"
        "        return item;\n"
        "    }\n"
        "};\n\n"
        "void inspect(Holder& holder)\n"
        "{\n"
        "    Item& item = holder.current();\n"
        "    std::unique_ptr<Item>& pointer = holder.owned();\n"
        "    (void)item.value;\n"
        "    (void)pointer;\n"
        "}\n";

    std::vector<ConversionChange> changes;
    const ReturnTypePropagationPass pass;
    const std::string fixed = pass.rewrite(code, changes);

    require(contains(fixed, "const Item& item = holder.current();"),
            "reference receiver should follow const reference return type\nConverted code:\n" + fixed);
    require(contains(fixed, "const std::unique_ptr<Item>& pointer = holder.owned();"),
            "smart pointer reference receiver should follow const reference return type\nConverted code:\n" + fixed);
    require(!contains(fixed, "\n    Item& item = holder.current();"),
            "old mutable reference receiver should not remain\nConverted code:\n" + fixed);
    require(!contains(fixed, "\n    std::unique_ptr<Item>& pointer = holder.owned();"),
            "old mutable smart pointer reference receiver should not remain\nConverted code:\n" + fixed);
    require(hasAppliedRule(changes, "Const return receiver propagation"),
            "reference receiver propagation should be tracked");

    const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(fixed);
    require(verification.compilerUsed.empty() || verification.passed,
            "reference receiver propagation sample should pass syntax verification\nCompiler output:\n"
                + verification.output + "\nConverted code:\n" + fixed);
}

void testReferenceReturnPropagationRepairsStaleContainerReceiver()
{
    const std::string code =
        "#include <vector>\n"
        "struct Item {};\n"
        "class Holder\n"
        "{\n"
        "public:\n"
        "    const std::vector<Item>& items() const\n"
        "    {\n"
        "        return values;\n"
        "    }\n"
        "private:\n"
        "    std::vector<Item> values;\n"
        "};\n"
        "void inspect(const Holder& holder)\n"
        "{\n"
        "    std::vector<Item*> stale = holder.items();\n"
        "    (void)stale;\n"
        "}\n";

    std::vector<ConversionChange> changes;
    const ReturnTypePropagationPass pass;
    const std::string fixed = pass.rewrite(code, changes);

    require(contains(fixed, "const auto& stale = holder.items();"),
            "stale explicit receiver should become a const reference to the visible getter result\nConverted code:\n"
                + fixed);
    require(!contains(fixed, "std::vector<Item*> stale = holder.items();"),
            "old incompatible receiver type should not remain\nConverted code:\n" + fixed);
    require(hasAppliedRule(changes, "Reference return receiver propagation"),
            "reference return receiver propagation should be tracked");

    const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(fixed);
    require(verification.compilerUsed.empty() || verification.passed,
            "reference return receiver propagation should pass syntax verification\nCompiler output:\n"
                + verification.output + "\nConverted code:\n" + fixed);
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
        "#define DOUBLE_VALUE(x) ((x) * 2)\n"
        "typedef struct\n"
        "{\n"
        "    int id;\n"
        "} Record;\n\n"
        "int scale(int value)\n"
        "{\n"
        "    return DOUBLE_VALUE(value);\n"
        "}\n\n"
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
    require(contains(modernized, "constexpr auto DOUBLE_VALUE"), "repository mode should convert safe function-like macros");
    require(!contains(modernized, "#define DOUBLE_VALUE"), "repository mode should remove converted function-like macro definitions");
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
    require(contains(report, "Function-like macro to constexpr function"), "repository report should include function-like macro conversion");
    require(contains(report, "C-style typedef struct to C++ struct"), "repository report should include typedef struct conversion");
    require(contains(report, "Char buffer member to std::string"), "repository report should include char buffer conversion");
    require(contains(report, "Value-type pointer operation scanner"), "repository report should include value-type pointer cleanup scanner");
    require(contains(report, "Vector growth emulation cleanup"), "repository report should include vector growth cleanup");
    require(contains(report, "Vector cascade cleanup"), "repository report should include dependent vector cleanup");
    require(std::filesystem::exists(root / "src" / "legacy.cpp.legacy_backup"), "repository structural pipeline should create backup");
}

void testRepositoryModeUsesFunctionPointerModernizationPass()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_function_pointer");
    writeTextFile(root / "src" / "callbacks.cpp",
        "typedef int (*Transform)(int);\n"
        "int increment(int value) { return value + 1; }\n"
        "class CallbackStore\n"
        "{\n"
        "public:\n"
        "    CallbackStore() : callback(nullptr) {}\n"
        "    void setCallback(void (*next)(int))\n"
        "    {\n"
        "        callback = next;\n"
        "    }\n"
        "    int notify(int value)\n"
        "    {\n"
        "        if (callback != nullptr)\n"
        "        {\n"
        "            return callback(value);\n"
        "        }\n"
        "        return value;\n"
        "    }\n"
        "private:\n"
        "    int (*callback)(int);\n"
        "};\n"
        "void run()\n"
        "{\n"
        "    int (*local)(int) = &increment;\n"
        "    CallbackStore store;\n"
        "    store.setCallback(local);\n"
        "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/function-pointer-modernization";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = true;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "callbacks.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository function pointer test should scan one source file");
    require(result.filesModified == 1, "repository function pointer test should modify the source file");
    require(contains(modernized, "using Transform = int (*)(int);"),
            "repository mode should modernize function pointer typedefs");
    require(contains(modernized, "auto local = &increment;"),
            "repository mode should modernize local callback variables to auto");
    require(contains(modernized, "std::function<int(int)> callback;"),
            "repository mode should modernize private stored callback fields to std::function");
    require(contains(modernized, "int notify(int value)"),
            "repository mode should preserve unrelated callback wrapper behavior");
    require(contains(report, "Function pointer typedef to using"),
            "repository report should include function pointer typedef modernization");
    require(contains(report, "Stored callback pointer to std::function"),
            "repository report should include stored callback modernization");
    require(std::filesystem::exists(root / "src" / "callbacks.cpp.legacy_backup"),
            "repository function pointer modernization should create backup");
}

void testRepositoryModeUsesPrintfModernizationPass()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_printf");
    writeTextFile(root / "src" / "output.cpp",
        "#include <cstdio>\n"
        "void report(const char* label, int value)\n"
        "{\n"
        "    printf(\"%s=%d\\n\", label, value);\n"
        "    fprintf(stderr, \"error=%d\\n\", value);\n"
        "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/printf-modernization";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = true;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "output.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository printf test should scan one source file");
    require(result.filesModified == 1, "repository printf test should modify the source file");
    require(contains(modernized, "std::cout << label << \"=\" << value << \"\\n\";"),
            "repository mode should modernize printf output to std::cout");
    require(contains(modernized, "std::cerr << \"error=\" << value << \"\\n\";"),
            "repository mode should modernize stderr fprintf output to std::cerr");
    require(!contains(modernized, "printf("), "repository mode should remove converted printf");
    require(contains(report, "printf-family output to iostream"),
            "repository report should include printf modernization");
    require(std::filesystem::exists(root / "src" / "output.cpp.legacy_backup"),
            "repository printf modernization should create backup");
}

void testRepositoryModeUsesOwnershipGraphPipeline()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_ownership_graph");
    writeTextFile(root / "src" / "ownership.cpp",
        "struct Node {};\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    explicit Owner(int count)\n"
        "        : count(count)\n"
        "    {\n"
        "        nodes = new Node*[count];\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            nodes[i] = new Node();\n"
        "        }\n"
        "    }\n"
        "    ~Owner()\n"
        "    {\n"
        "        for (int i = 0; i < count; ++i)\n"
        "        {\n"
        "            delete nodes[i];\n"
        "        }\n"
        "        delete[] nodes;\n"
        "    }\n"
        "private:\n"
        "    int count;\n"
        "    Node** nodes;\n"
        "};\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/ownership-graph";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "ownership.cpp");
    const std::string report = readTextFile(root / "modernization_report.txt");

    require(result.filesScanned == 1, "repository ownership graph test should scan one source file");
    require(result.filesModified == 1, "repository ownership graph test should modify the source file");
    require(contains(modernized, "std::vector<std::unique_ptr<Node>> nodes;"),
            "repository ownership graph pipeline should convert pointer-to-pointer storage");
    require(contains(modernized, "nodes.push_back(std::make_unique<Node>());"),
            "repository ownership graph pipeline should convert owned element allocations");
    require(!contains(modernized, "delete nodes[i]"), "repository ownership graph pipeline should remove nested delete loop");
    require(!contains(modernized, "delete[] nodes"), "repository ownership graph pipeline should remove outer delete[]");
    require(contains(report, "Ownership graph modernization"), "repository report should include ownership graph modernization");
    require(contains(report, "Nested delete loop elimination"), "repository report should include nested delete loop elimination");
    require(std::filesystem::exists(root / "src" / "ownership.cpp.legacy_backup"),
            "repository ownership graph pipeline should create backup");
}

void testRepositoryModeAppliesScopeLeakValidation()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_repo_scope_validation");
    writeTextFile(root / "src" / "scope.cpp",
        "class Metrics\n"
        "{\n"
        "public:\n"
        "    int getLength() const { return length; }\n"
        "private:\n"
        "    int length;\n"
        "};\n"
        "void build(int count)\n"
        "{\n"
        "    int* values = new int[count];\n"
        "    values[0] = 1;\n"
        "    delete[] values;\n"
        "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/scope-validation";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "scope.cpp");

    require(result.filesScanned == 1, "repository scope validation should scan one source file");
    require(contains(modernized, "int getLength() const { return length; }"),
            "repository scope validation should preserve unrelated class getter");
    require(!contains(modernized, "return values.size();"),
            "repository scope validation should prevent local vector leakage into class getter");
    require(contains(modernized, "std::vector<int> values"), "repository scope fixture should still modernize local array");
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

void testCoordinatorCanBufferManualGrowthReproTerminates()
{
    auto backend = std::make_unique<FakeBackendClient>();
    auto* backendRaw = backend.get();
    ConversionCoordinator coordinator(std::make_unique<RuleBasedConverterEngine>(), std::move(backend));

    ModernizationOptions options = structuralOptions();
    options.compileVerificationEnabled = true;

    const auto started = std::chrono::steady_clock::now();
    const CoordinatedConversionResult result = coordinator.convert(
        "#include <iostream>\n"
        "#include <cstring>\n\n"
        "#define INITIAL_MAX 2\n\n"
        "typedef struct _CanFrame {\n"
        "    unsigned long arbitrationId;\n"
        "    char dataPayload[8];\n"
        "} CanFrame;\n\n"
        "class CanBufferManager {\n"
        "private:\n"
        "    CanFrame* backingStore;\n"
        "    int count;\n"
        "    int capacity;\n\n"
        "public:\n"
        "    CanBufferManager() {\n"
        "        count = 0;\n"
        "        capacity = INITIAL_MAX;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "    }\n\n"
        "    CanBufferManager(const CanBufferManager& other) {\n"
        "        count = other.count;\n"
        "        capacity = other.capacity;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "        for (int i = 0; i < count; ++i) {\n"
        "            backingStore[i] = other.backingStore[i];\n"
        "        }\n"
        "    }\n\n"
        "    ~CanBufferManager() {\n"
        "        if (backingStore != NULL) {\n"
        "            delete[] backingStore;\n"
        "            backingStore = NULL;\n"
        "        }\n"
        "    }\n\n"
        "    bool appendFrame(unsigned long id, const char* bytes) {\n"
        "        if (count >= capacity) {\n"
        "            int newCap = capacity * 2;\n"
        "            CanFrame* newStore = new CanFrame[newCap];\n"
        "            for (int i = 0; i < count; ++i) {\n"
        "                newStore[i] = backingStore[i];\n"
        "            }\n"
        "            delete[] backingStore;\n"
        "            backingStore = newStore;\n"
        "            capacity = newCap;\n"
        "        }\n\n"
        "        backingStore[count].arbitrationId = id;\n"
        "        std::strncpy(backingStore[count].dataPayload, bytes, 7);\n"
        "        backingStore[count].dataPayload[7] = '\\0';\n\n"
        "        count++;\n"
        "        return true;\n"
        "    }\n\n"
        "    int getFrameCount() const { return count; }\n"
        "    CanFrame* getFrame(int index) { return &backingStore[index]; }\n"
        "};\n\n"
        "int main() {\n"
        "    CanBufferManager buffer;\n"
        "    buffer.appendFrame(0x7DF, \"\\x02\\x01\\x0D\\x00\\x00\\x00\\x00\");\n"
        "    buffer.appendFrame(0x7E8, \"\\x03\\x41\\x0D\\x32\\x00\\x00\\x00\");\n\n"
        "    std::cout << \"Buffered Frames: \" << buffer.getFrameCount() << std::endl;\n"
        "    return 0;\n"
        "}\n",
        options,
        ConversionMode::OfflineRuleBased);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    require(elapsed.count() < 15000,
            "coordinator CanBuffer repro should terminate quickly; elapsed_ms=" + std::to_string(elapsed.count()));
    require(!backendRaw->convertCalled, "offline coordinator path should not call backend");
    require(result.effectiveMode == ConversionMode::OfflineRuleBased, "coordinator repro should stay offline");
    require(diagnosticsContain(result.result, "START PASS VectorParadigmRewritePass"),
            "coordinator path should preserve START PASS pipeline diagnostics for GUI details");
    require(diagnosticsContain(result.result, "END PASS VectorParadigmRewritePass"),
            "coordinator path should preserve END PASS pipeline diagnostics for GUI details");
    require(!diagnosticsContain(result.result, "iteration-limit-exceeded"),
            "coordinator CanBuffer repro should not hit convergence guard");
    require(contains(result.result.modernCode, "inline constexpr auto INITIAL_MAX"),
            "coordinator CanBuffer repro should convert constant macros");
    require(contains(result.result.modernCode, "struct CanFrame"),
            "coordinator CanBuffer repro should modernize typedef struct");
    require(contains(result.result.modernCode, "std::vector<CanFrame> backingStore"),
            "coordinator CanBuffer repro should convert raw array storage to vector");
    require(!contains(result.result.modernCode, "new CanFrame["),
            "coordinator CanBuffer repro should remove raw dynamic array allocation");
    require(!contains(result.result.modernCode, "delete[] backingStore"),
            "coordinator CanBuffer repro should remove manual delete[] cleanup");
    require(!contains(result.result.modernCode, "newStore"),
            "coordinator CanBuffer repro should remove manual growth temporaries");
    require(contains(result.result.modernCode, "backingStore.push_back"),
            "coordinator CanBuffer repro should rewrite append logic to vector push_back\nConverted code:\n" + result.result.modernCode);
    require(contains(result.result.modernCode, "return backingStore.size();"),
            "coordinator CanBuffer repro should cascade count getter to vector.size()");
    require(contains(result.result.modernCode, "'\\n'"),
            "coordinator CanBuffer repro should replace simple std::endl with newline output");
    require(result.result.compileVerificationEnabled, "coordinator CanBuffer repro should run compile verification");
    require(result.result.compileVerificationPassed,
            "coordinator CanBuffer repro should pass compile verification\nCompiler output:\n" + result.result.compilerOutput
                + "\nConverted code:\n" + result.result.modernCode);
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
    testFunctionPointerTypedefAndLocalCallbackModernization();
    testStoredCallbackUsesStdFunctionConservatively();
    testRawFunctionPointerParameterPreservedWhenNotStored();
    testPrintfSimpleTextModernizesToCout();
    testPrintfValuesModernizeToIostreamChain();
    testPrintfValuesCanUseStdFormatWhenEnabled();
    testFprintfStdoutAndStderrModernizeToStreams();
    testUnsafePrintfFormatsRemainSuggestions();
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
    testConvertsTemplatedLocalNewDeleteToMakeUnique();
    testConvertsBasePointerNewDerivedToUniquePtrAndGet();
    testUniquePtrOwnerPassedToRawObserverUsesGet();
    testClassRawPointerMemberDeletesCopyAndDefaultsMove();
    testMallocSingleObjectModernizesToUniquePtr();
    testMallocArrayModernizesToVector();
    testCallocAndByteBufferModernizeToVector();
    testMallocMemberCleanupDestructorModernizesToRuleOfZero();
    testMallocReallocAndEscapingOwnershipRemainUnchanged();
    testOwnershipConversionDoesNotDuplicateSuggestions();
    testDoesNotConvertEscapingPointer();
    testDoesNotConvertReturnedPointer();
    testDoesNotConvertAmbiguousOwnership();
    testStringViewAppliesOnlyWhenEnabledAndSafe();
    testGeneratedOwnershipSample();
    testOwnershipGraphAnalyzerClassifiesPointerCollection();
    testPointerToPointerCollectionModernizesToVectorUniquePtr();
    testFixedPointerArrayModernizesToArrayUniquePtr();
    testOwningRawPointerVectorModernizesToVectorUniquePtr();
    testStringLikeOwningClassProducesSuggestionOnly();
    testOwnershipSanityScannerRemovesPartialSmartCollectionCleanup();
    testUniquePtrCollectionTraversalPreservesIndexSafely();
    testUniquePtrCollectionPushBackDoesNotBecomeInitializerList();
    testSmartPointerCollectionPropagationFixesRawNewAndGets();
    testSmartPointerVectorAppendAndPredicateUseMakeUniqueAndGet();
    testUniquePtrCollectionCountLoopBecomesCountIf();
    testFilePointerWriteModernizesToOfstream();
    testOfflineHelperComputationExtractedToLambda();
    testOfflineFunctorToLambda();
    testOfflineAutoAndConstexprUsage();
    testOfflineOldStyleCastConversion();
    testOfflineOverrideAnnotation();
    testOfflineStructuredBindingAggressiveSafe();
    testOfflineRequiredIncludesAreNotDuplicated();
    testOfflineCompileVerificationPassesForSimpleSample();
    testCompileVerifierHandlesUnavailableCompilerGracefully();
    testCompileVerifierDoesNotMergeMultipleMainSnippets();
    testOfflineSampleFilesModernize();
    testTokenBasedStructureAnalyzerFindsReusableBlocks();
    testStructuralPreprocessorCleanupAndConstantMacros();
    testFunctionLikeMacroModernization();
    testStructuralTypedefStructModernization();
    testStructuralCharBufferMemberModernizesToString();
    testClassMultipleOwnedCharTextMembersModernizeTogether();
    testRuleOfZeroPreservesCleanupWhenRawCharOwnershipRemains();
    testStringModernizationRemovesTemporaryConcatBuffer();
    testStructuralRawDynamicArrayModernizesToVector();
    testStructuralLocalDynamicArrayModernizesToVector();
    testStructuralIteratorAndIndexLoopsModernize();
    testLoopModernizationSafetyBoundaries();
    testManualGrowthPipelineConverges();
    testCanBufferManualGrowthReproTerminates();
    testStructuralPreprocessorBalanceValidationRemovesDanglingEndif();
    testDependentVectorCleanupUpdatesAllUsages();
    testDependentStringCleanupUpdatesAllUsages();
    testDependentStringCleanupRewritesConcatAndSymmetricCompare();
    testStringCleanupHandlesConcatOnlyTextUsage();
    testNestedStringMemberCascadeCleanup();
    testCompilerDiagnosticCleanupFixesKnownLeftovers();
    testValueTypePointerOperationScannerRemovesPointerLeftovers();
    testValueTypeNullptrEqualityBecomesValueStateCheck();
    testEmptyCleanupBlockAfterValueTypeModernizationIsRemoved();
    testVectorMemberGetterCascadesToContainerReference();
    testLocalCollectionDoesNotLeakIntoUnrelatedClassGetter();
    testLengthGetterForIndependentMemberIsPreserved();
    testScopeLeakValidatorRepairsWrongInClassSymbolWhenSafe();
    testScopeAwareSymbolTableTracksClassMembers();
    testScopeAwareSymbolTableTracksFunctionLocals();
    testVectorRawBufferGetterUsesDataOnlyWhenIntentIsClear();
    testRuntimeVectorGetterIsNotConstexpr();
    testMalformedEmptyDestructorBlocksAreRemoved();
    testContainerPolishModernizesInitializerAndMapInsert();
    testMapIteratorLoopUsesStructuredBindingAndNewline();
    testExplicitMutableIteratorLoopModernizes();
    testIteratorLoopWithEraseIsPreservedWithWarning();
    testVectorGrowthEmulationCleanupModernizesAppend();
    testVectorPostIncrementAppendBecomesPushBack();
    testLocalRawArrayAppendUsesReserveAndPushBack();
    testVectorGrowthPassFlagsAmbiguousRawAssignment();
    testVectorEmulationEliminatesFieldAppendGrowth();
    testContainerModernizationCleanupRemovesGenericGrowthSystem();
    testContainerModernizationCleanupRemovesSameLineGrowthFragments();
    testContainerModernizationCleanupPolishesVectorBackedClass();
    testPostVectorCleanupRemovesUnusedCapacityWithoutReserve();
    testSizeReturningGetterUpdatesSignedLoopIndex();
    testCrossScopePropagationAdaptsVectorToRawArrayCallsite();
    testFinalFormatterCleansTransformedBlocks();
    testConstReturnPropagationUpdatesDirectReceivers();
    testConstReturnPropagationHandlesReferencesAndSmartPointerRefs();
    testReferenceReturnPropagationRepairsStaleContainerReceiver();
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
    testRepositoryModeUsesFunctionPointerModernizationPass();
    testRepositoryModeUsesPrintfModernizationPass();
    testRepositoryModeUsesOwnershipGraphPipeline();
    testRepositoryModeAppliesScopeLeakValidation();
    testRepositoryModeUsesSmartPointerPropagationPass();
    testOfflineModeStillWorks();
    testCoordinatorCanBufferManualGrowthReproTerminates();
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
