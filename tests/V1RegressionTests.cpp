#include "converter/RuleBasedConverterEngine.h"
#include "models/RepositoryModernizationModels.h"
#include "repository/RepositoryModernizationService.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

bool hasSuggestionRule(const ConversionResult& result, const std::string& ruleName)
{
    return std::any_of(result.changes.begin(), result.changes.end(), [&ruleName](const ConversionChange& change) {
        return !change.applied && contains(change.ruleName, ruleName);
    });
}

void requireCompileExpectation(const ConversionResult& result, const std::string& label)
{
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed,
                label + " should pass syntax-only compile verification\nCompiler output:\n"
                    + result.compilerOutput + "\nConverted code:\n" + result.modernCode);
    }
}

ModernizationOptions v1Options()
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

ModernizationOptions v1StringViewOptions()
{
    ModernizationOptions options = v1Options();
    options.applyStringViewWhenSafe = true;
    options.useStringView = true;
    return options;
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

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.good(), "test file should be readable: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void testRawDynamicArrayToVector()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstring>\n"
        "struct Record { int id; };\n"
        "class Store\n"
        "{\n"
        "    Record* records;\n"
        "    int count;\n"
        "    int capacity;\n"
        "public:\n"
        "    Store()\n"
        "    {\n"
        "        count = 0;\n"
        "        capacity = 2;\n"
        "        records = new Record[capacity];\n"
        "    }\n"
        "    ~Store() { delete[] records; }\n"
        "    void add(int id)\n"
        "    {\n"
        "        if (count >= capacity) {\n"
        "            int newCapacity = capacity * 2;\n"
        "            Record* temp = new Record[newCapacity];\n"
        "            for (int i = 0; i < count; ++i) { temp[i] = records[i]; }\n"
        "            delete[] records;\n"
        "            records = temp;\n"
        "            capacity = newCapacity;\n"
        "        }\n"
        "        records[count].id = id;\n"
        "        ++count;\n"
        "    }\n"
        "    int size() const { return count; }\n"
        "};\n",
        v1Options());

    require(contains(result.modernCode, "std::vector<Record> records"),
            "raw dynamic array storage should become std::vector\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "records.push_back"),
            "append-style indexed writes should become push_back\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "records.size()"),
            "count getter or maintenance should use vector size\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete[] records")
                && !contains(result.modernCode, "new Record[")
                && !contains(result.modernCode, "newCapacity"),
            "manual vector growth fragments should not remain\nConverted code:\n" + result.modernCode);
    requireCompileExpectation(result, "raw dynamic array to vector");
}

void testCharBuffersBecomeStringsAndCapiCleanup()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult stringResult = converter.convert(
        "#include <cstring>\n"
        "class Label\n"
        "{\n"
        "    char* text;\n"
        "public:\n"
        "    explicit Label(const char* input)\n"
        "    {\n"
        "        text = new char[std::strlen(input) + 1];\n"
        "        std::strcpy(text, input);\n"
        "    }\n"
        "    ~Label() { delete[] text; }\n"
        "    const char* c_str() const { return text; }\n"
        "};\n",
        v1Options());

    require(contains(stringResult.modernCode, "std::string text"),
            "owning char* text buffer should become std::string\nConverted code:\n" + stringResult.modernCode);
    require(contains(stringResult.modernCode, "text.c_str()"),
            "const char* compatibility getter should use string::c_str()\nConverted code:\n" + stringResult.modernCode);
    require(!contains(stringResult.modernCode, "delete[] text")
                && !contains(stringResult.modernCode, "new char")
                && !contains(stringResult.modernCode, "std::strcpy(text"),
            "manual char ownership should not remain after std::string conversion\nConverted code:\n"
                + stringResult.modernCode);
    requireCompileExpectation(stringResult, "char* to std::string");

    const ConversionResult cleanupResult = converter.convert(
        "#include <cstring>\n"
        "#include <string>\n"
        "struct Entry { std::string name; };\n"
        "bool update(Entry& entry, const char* suffix)\n"
        "{\n"
        "    std::strcpy(entry.name, \"core\");\n"
        "    std::strcat(entry.name, suffix);\n"
        "    return std::strlen(entry.name) > 0 && std::strcmp(entry.name, suffix) != 0;\n"
        "}\n",
        v1Options());

    require(contains(cleanupResult.modernCode, "entry.name = \"core\";"),
            "strcpy targeting std::string should become assignment\nConverted code:\n" + cleanupResult.modernCode);
    require(contains(cleanupResult.modernCode, "entry.name += suffix;"),
            "strcat targeting std::string should become append\nConverted code:\n" + cleanupResult.modernCode);
    require(contains(cleanupResult.modernCode, "entry.name.size()"),
            "strlen on std::string should become size()\nConverted code:\n" + cleanupResult.modernCode);
    require(contains(cleanupResult.modernCode, "entry.name != suffix"),
            "strcmp != 0 with std::string should become string comparison\nConverted code:\n" + cleanupResult.modernCode);
    require(!contains(cleanupResult.modernCode, "std::strcpy")
                && !contains(cleanupResult.modernCode, "std::strcat")
                && !contains(cleanupResult.modernCode, "std::strlen(entry.name")
                && !contains(cleanupResult.modernCode, "std::strcmp(entry.name"),
            "C-string APIs should not remain in invalid std::string form\nConverted code:\n"
                + cleanupResult.modernCode);
    requireCompileExpectation(cleanupResult, "C-string API cleanup");
}

void testUniquePtrOwnershipAndObserverGet()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "struct Resource { void use() {} };\n"
        "void observe(Resource*) {}\n"
        "void run()\n"
        "{\n"
        "    Resource* owner = new Resource();\n"
        "    Resource* alias = owner;\n"
        "    observe(owner);\n"
        "    observe(alias);\n"
        "    owner->use();\n"
        "    delete owner;\n"
        "}\n",
        v1Options());

    require(contains(result.modernCode, "std::make_unique<Resource>()"),
            "clear new/delete ownership should use make_unique\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "Resource* alias = owner.get();"),
            "non-owning observer alias should remain raw and observe owner.get()\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "observe(owner.get());"),
            "raw-pointer sink should receive owner.get()\nConverted code:\n" + result.modernCode);
    require(contains(result.modernCode, "observe(alias);"),
            "raw observer alias should still be passed as a raw pointer\nConverted code:\n" + result.modernCode);
    require(!contains(result.modernCode, "delete owner")
                && !contains(result.modernCode, "auto alias = owner")
                && !contains(result.modernCode, "std::shared_ptr<Resource> alias"),
            "observer aliases must not become accidental owners\nConverted code:\n" + result.modernCode);
    requireCompileExpectation(result, "new/delete to unique_ptr with observer preservation");
}

void testScopedEnumPropagationAndOutput()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult usageResult = converter.convert(
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
        v1Options());

    require(contains(usageResult.modernCode, "enum class Status"),
            "legacy enum should become enum class\nConverted code:\n" + usageResult.modernCode);
    require(contains(usageResult.modernCode, "return Status::Ready;")
                && contains(usageResult.modernCode, "status == Status::Busy")
                && contains(usageResult.modernCode, "status = Status::Ready"),
            "enum class values should be scoped in returns, comparisons, and assignments\nConverted code:\n"
                + usageResult.modernCode);
    require(contains(usageResult.modernCode, "static_cast<std::underlying_type_t<Status>>(status)"),
            "streamed enum class variable should be cast to an output-safe value\nConverted code:\n"
                + usageResult.modernCode);
    requireCompileExpectation(usageResult, "scoped enum propagation and stream output");

    const ConversionResult formatResult = converter.convert(
        "#include <string>\n"
        "namespace std { template <typename... Args> string format(const char*, Args&&...) { return {}; } }\n"
        "enum Level : unsigned char { Low, High };\n"
        "std::string render()\n"
        "{\n"
        "    return std::format(\"{}\", High);\n"
        "}\n",
        v1Options());

    require(contains(formatResult.modernCode, "enum class Level : unsigned char"),
            "explicit enum underlying type should be preserved\nConverted code:\n" + formatResult.modernCode);
    require(contains(formatResult.modernCode, "std::format(\"{}\", static_cast<unsigned char>(Level::High))"),
            "std::format enum argument should be cast to the explicit underlying type\nConverted code:\n"
                + formatResult.modernCode);
    requireCompileExpectation(formatResult, "scoped enum format output");
}

void testFileIoRangeLoopsAndMapPairSafety()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult fileResult = converter.convert(
        "#include <cstdio>\n"
        "void save(const char* path)\n"
        "{\n"
        "    FILE* file = fopen(path, \"w\");\n"
        "    if (file != NULL) {\n"
        "        fprintf(file, \"ok\\n\");\n"
        "    }\n"
        "    fclose(file);\n"
        "}\n",
        v1Options());

    require(contains(fileResult.modernCode, "std::ofstream file(path);"),
            "simple FILE* write should become std::ofstream\nConverted code:\n" + fileResult.modernCode);
    require(contains(fileResult.modernCode, "if (file)"),
            "FILE* non-null check should become stream truthiness\nConverted code:\n" + fileResult.modernCode);
    require(!contains(fileResult.modernCode, "fprintf(file")
                && !contains(fileResult.modernCode, "fclose(file)")
                && !contains(fileResult.modernCode, "file != NULL"),
            "stream object should not retain C FILE APIs or nullptr-style comparisons\nConverted code:\n"
                + fileResult.modernCode);
    requireCompileExpectation(fileResult, "FILE* to std::ofstream");

    const ConversionResult rangeResult = converter.convert(
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
        v1Options());

    require(contains(rangeResult.modernCode, "for (const auto& value : values)"),
            "safe explicit iterator loop should become range-based for\nConverted code:\n" + rangeResult.modernCode);
    requireCompileExpectation(rangeResult, "range-based loop modernization");

    const ConversionResult mapResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void print(const std::map<int, int>& values)\n"
        "{\n"
        "    for (const auto& item : values) {\n"
        "        std::cout << item.first << ':' << item.second << '\\n';\n"
        "    }\n"
        "}\n",
        v1Options());

    require(contains(mapResult.modernCode, "for (const auto& [key, value] : values)"),
            "map range loop accessing first/second should become structured binding\nConverted code:\n"
                + mapResult.modernCode);
    require(!contains(mapResult.modernCode, "item.first")
                && !contains(mapResult.modernCode, "item.second"),
            "stale pair member references should not remain after structured binding\nConverted code:\n"
                + mapResult.modernCode);
    requireCompileExpectation(mapResult, "map structured bindings");

    const ConversionResult pairResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "template <class T>\n"
        "void printValues(const T& values)\n"
        "{\n"
        "    for (typename T::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << \"Value: \" << *it << '\\n';\n"
        "    }\n"
        "}\n"
        "void run()\n"
        "{\n"
        "    std::vector<int> numbers{1, 2, 3};\n"
        "    std::map<int, std::string> names;\n"
        "    printValues(numbers);\n"
        "    printValues(names);\n"
        "}\n",
        v1Options());

    require(!contains(pairResult.modernCode, "std::cout << \"Value: \" << *it"),
            "generic map printer should not stream std::pair directly\nConverted code:\n" + pairResult.modernCode);
    require(contains(pairResult.modernCode, "ModernCppConverterPairLike"),
            "mixed vector/map generic printer should use pair-aware safety\nConverted code:\n" + pairResult.modernCode);
    requireCompileExpectation(pairResult, "std::pair streaming safety");
}

void testStringViewSafety()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult safeResult = converter.convert(
        "#include <iostream>\n"
        "#include <string>\n"
        "void show(const std::string& name)\n"
        "{\n"
        "    std::cout << name << '\\n';\n"
        "}\n",
        v1StringViewOptions());

    require(contains(safeResult.modernCode, "std::string_view name"),
            "read-only string parameter should become string_view when safe\nConverted code:\n"
                + safeResult.modernCode);
    require(contains(safeResult.modernCode, "#include <string_view>"),
            "string_view conversion should add include\nConverted code:\n" + safeResult.modernCode);
    requireCompileExpectation(safeResult, "safe string_view modernization");

    const ConversionResult unsafeResult = converter.convert(
        "#include <cstdio>\n"
        "#include <string>\n"
        "FILE* openPath(const std::string& path)\n"
        "{\n"
        "    return std::fopen(path.c_str(), \"r\");\n"
        "}\n",
        v1StringViewOptions());

    require(contains(unsafeResult.modernCode, "const std::string& path"),
            "null-terminated C API boundary should preserve const std::string&\nConverted code:\n"
                + unsafeResult.modernCode);
    require(!contains(unsafeResult.modernCode, "std::string_view path"),
            "unsafe c_str path should not become string_view\nConverted code:\n" + unsafeResult.modernCode);
    require(hasSuggestionRule(unsafeResult, "std::string_view")
                || contains(unsafeResult.modernCode, "path.c_str()"),
            "unsafe string_view case should be left compatible or reported for review");
    requireCompileExpectation(unsafeResult, "unsafe string_view rollback/preservation");
}

void testRepositoryModeSmoke()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_v1_repo");
    writeTextFile(root / "src" / "legacy.cpp",
                  "#include <iostream>\n"
                  "#define LIMIT 2\n"
                  "typedef struct _Entry { int value; } Entry;\n"
                  "int* build()\n"
                  "{\n"
                  "    int* value = new int(7);\n"
                  "    return value;\n"
                  "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "local-v1-regression";
    options.branch = "main";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = true;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "legacy.cpp");

    require(result.filesScanned == 1, "repository smoke should scan the legacy file");
    require(result.filesModified == 1, "repository smoke should modify the legacy file");
    require(std::filesystem::exists(root / "modernization_report.txt"),
            "repository smoke should write text report");
    require(std::filesystem::exists(root / "modernization_report.json"),
            "repository smoke should write JSON report");
    require(contains(modernized, "using Entry = struct _Entry")
                || contains(modernized, "struct Entry")
                || contains(modernized, "struct _Entry"),
            "repository smoke should preserve/modernize struct declaration\nConverted file:\n" + modernized);
    require(contains(modernized, "constexpr") || contains(modernized, "const"),
            "repository smoke should apply basic modernization to constants\nConverted file:\n" + modernized);
    require(!result.files.empty() && !result.files.front().diagnosticMessages.empty(),
            "repository smoke should keep per-file diagnostics");
}

} // namespace

int main()
{
    testRawDynamicArrayToVector();
    testCharBuffersBecomeStringsAndCapiCleanup();
    testUniquePtrOwnershipAndObserverGet();
    testScopedEnumPropagationAndOutput();
    testFileIoRangeLoopsAndMapPairSafety();
    testStringViewSafety();
    testRepositoryModeSmoke();

    std::cout << "All v1 regression tests passed.\n";
    return EXIT_SUCCESS;
}
