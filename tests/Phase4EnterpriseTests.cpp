#include "converter/RuleBasedConverterEngine.h"
#include "converter/ScopedEnumCastValidationPass.h"
#include "converter/ScopedEnumOutputPropagationPass.h"
#include "converter/ScopedEnumOutputValidator.h"
#include "repository/RepositoryModernizationService.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

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

bool hasSuggestionRule(const ConversionResult& result, const std::string& ruleName)
{
    return std::any_of(result.changes.begin(), result.changes.end(), [&ruleName](const ConversionChange& change) {
        return !change.applied && contains(change.ruleName, ruleName);
    });
}

bool hasAnyRule(const ConversionResult& result, const std::string& ruleName)
{
    return std::any_of(result.changes.begin(), result.changes.end(), [&ruleName](const ConversionChange& change) {
        return contains(change.ruleName, ruleName);
    });
}

ModernizationOptions enterpriseOptions()
{
    ModernizationOptions options;
    options.offlineModernizationLevel = OfflineModernizationLevel::Balanced;
    options.targetStandard = CppStandard::Cpp20;
    options.useSmartPointers = true;
    options.useMakeUnique = true;
    options.applySafeOwnershipModernization = true;
    options.useRangeBasedFor = true;
    options.useLambdas = true;
    options.useStructuredBindings = true;
    options.useAuto = true;
    options.useConstexpr = true;
    options.compileVerificationEnabled = true;
    return options;
}

ModernizationOptions qualitySprintOptions()
{
    ModernizationOptions options = enterpriseOptions();
    options.offlineModernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    options.useMoveSemantics = true;
    options.applyStringViewWhenSafe = true;
    options.useInlineVariables = true;
    return options;
}

std::filesystem::path makeTempDirectory(const std::string& prefix)
{
    return std::filesystem::temp_directory_path()
        / (prefix + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << text;
}

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireCompilePassIfCompilerAvailable(const ConversionResult& result, const std::string& message)
{
    if (!result.compilerUsed.empty()) {
        require(result.compileVerificationPassed, message + "\nCompiler output:\n" + result.compilerOutput);
    }
}

void runOwnershipTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <vector>\n"
        "struct Element {};\n"
        "class Collection\n"
        "{\n"
        "public:\n"
        "    void add()\n"
        "    {\n"
        "        elements.push_back(new Element());\n"
        "    }\n"
        "    ~Collection()\n"
        "    {\n"
        "        for (auto element : elements)\n"
        "        {\n"
        "            delete element;\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    std::vector<Element*> elements;\n"
        "};\n",
        enterpriseOptions());

    require(contains(result.modernCode, "std::vector<std::unique_ptr<Element>> elements;"),
            "owning raw pointer vector should become vector<unique_ptr>");
    require(contains(result.modernCode, "elements.push_back(std::make_unique<Element>());"),
            "owned insertions should use make_unique");
    require(!contains(result.modernCode, "delete element"), "manual cleanup loop should be removed");
    require(hasAppliedRule(result, "Owning raw pointer container to std::vector<std::unique_ptr>"),
            "ownership graph conversion should be tracked");
    requireCompilePassIfCompilerAvailable(result, "ownership modernization should compile");

    const ConversionResult memberGetterResult = converter.convert(
        "struct Resource\n"
        "{\n"
        "    int value() const { return 7; }\n"
        "    void touch() {}\n"
        "};\n"
        "class SingleOwner\n"
        "{\n"
        "public:\n"
        "    SingleOwner()\n"
        "        : resource(new Resource())\n"
        "    {\n"
        "    }\n"
        "    ~SingleOwner()\n"
        "    {\n"
        "        if (resource != nullptr)\n"
        "        {\n"
        "            delete resource;\n"
        "            resource = nullptr;\n"
        "        }\n"
        "    }\n"
        "    const Resource* getResource() const\n"
        "    {\n"
        "        return resource;\n"
        "    }\n"
        "    void use()\n"
        "    {\n"
        "        resource->touch();\n"
        "    }\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        enterpriseOptions());

    require(contains(memberGetterResult.modernCode, "std::unique_ptr<Resource> resource;"),
            "owning raw pointer member should become unique_ptr\nOutput:\n" + memberGetterResult.modernCode);
    require(contains(memberGetterResult.modernCode, "resource(std::make_unique<Resource>())"),
            "initializer-list raw allocation should become make_unique");
    require(contains(memberGetterResult.modernCode, "return resource.get();"),
            "raw observer getter should return unique_ptr::get()");
    require(!contains(memberGetterResult.modernCode, "delete resource"),
            "manual member delete should be removed");
    require(!contains(memberGetterResult.modernCode, "~SingleOwner()"),
            "cleanup-only destructor should be removed after unique_ptr conversion");
    require(hasAppliedRule(memberGetterResult, "Class member raw pointer to std::unique_ptr"),
            "member unique_ptr conversion should be tracked");
    require(hasAppliedRule(memberGetterResult, "Unique_ptr getter return to raw observer"),
            "getter cascading should be tracked");
    requireCompilePassIfCompilerAvailable(memberGetterResult, "member unique_ptr getter modernization should compile");

    const ConversionResult borrowedResult = converter.convert(
        "struct Resource {};\n"
        "class BorrowedView\n"
        "{\n"
        "public:\n"
        "    explicit BorrowedView(Resource* external)\n"
        "        : resource(external)\n"
        "    {\n"
        "    }\n"
        "    Resource* getResource() const\n"
        "    {\n"
        "        return resource;\n"
        "    }\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        enterpriseOptions());

    require(contains(borrowedResult.modernCode, "Resource* resource;"),
            "borrowed raw pointer member should remain raw");
    require(!contains(borrowedResult.modernCode, "std::unique_ptr<Resource> resource;"),
            "borrowed pointer should not be converted to unique_ptr");
    require(hasSuggestionRule(borrowedResult, "Class member raw pointer to std::unique_ptr"),
            "borrowed pointer should produce an ownership review suggestion");
    requireCompilePassIfCompilerAvailable(borrowedResult, "borrowed raw pointer sample should remain compilable");
}

void runPolymorphismTests()
{
    const RuleBasedConverterEngine converter;
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
        "void observe(Base* item);\n"
        "void build()\n"
        "{\n"
        "    std::array<std::unique_ptr<Base>, 1> items;\n"
        "    items[0] = new Derived();\n"
        "    observe(items[0]);\n"
        "}\n",
        enterpriseOptions());

    require(contains(result.modernCode, "items[0] = std::make_unique<Derived>();"),
            "unique_ptr<Base> element assignment should use make_unique<Derived>");
    require(contains(result.modernCode, "observe(items[0].get());"),
            "observer call should receive non-owning raw pointer via .get()");
    require(contains(result.modernCode, "virtual ~Base() = default;"),
            "polymorphic base should gain virtual destructor");
    require(contains(result.modernCode, "void run() override;"), "derived override should be annotated");
    requireCompilePassIfCompilerAvailable(result, "polymorphic modernization should compile");

    const ConversionResult nameResult = converter.convert(
        "struct GeneratedNameTarget\n"
        "{\n"
        "    // Nearby prose should never become a destructor name.\n"
        "    virtual void execute();\n"
        "};\n",
        enterpriseOptions());

    require(contains(nameResult.modernCode, "virtual ~GeneratedNameTarget() = default;"),
            "generated destructor name should exactly match enclosing class name\nOutput:\n" + nameResult.modernCode);
    require(!contains(nameResult.modernCode, "~Nearby") && !contains(nameResult.modernCode, "~prose"),
            "destructor generator must not use nearby comment/prose tokens");
    requireCompilePassIfCompilerAvailable(nameResult, "generated polymorphic destructor should compile");

    const ConversionResult wrongNameResult = converter.convert(
        "struct CorrectContext\n"
        "{\n"
        "    virtual void process();\n"
        "    ~WrongToken() = default;\n"
        "};\n",
        enterpriseOptions());

    require(contains(wrongNameResult.modernCode, "~CorrectContext()"),
            "wrong destructor identifier should be repaired to enclosing class name");
    require(!contains(wrongNameResult.modernCode, "~WrongToken()"),
            "wrong destructor identifier should not remain after validation");
    requireCompilePassIfCompilerAvailable(wrongNameResult, "repaired destructor name should compile");

    const ConversionResult destructorResult = converter.convert(
        "#include <iostream>\n"
        "struct AbstractThing\n"
        "{\n"
        "    virtual void run();\n"
        "    ~AbstractThing()\n"
        "    {\n"
        "        std::cout << \"cleanup\\n\";\n"
        "    }\n"
        "};\n",
        enterpriseOptions());

    require(contains(destructorResult.modernCode, "virtual ~AbstractThing()"),
            "existing polymorphic destructor should become virtual");
    require(contains(destructorResult.modernCode, "std::cout << \"cleanup\\n\";"),
            "existing destructor body logic should be preserved");
    require(!contains(destructorResult.modernCode, "virtual ~AbstractThing() = default;\n    virtual ~AbstractThing()"),
            "default destructor should not be duplicated when a destructor body exists");
    requireCompilePassIfCompilerAvailable(destructorResult, "polymorphic destructor preservation should compile");

    const ConversionResult derivedDestructorResult = converter.convert(
        "#include <iostream>\n"
        "struct BaseWithDestructor\n"
        "{\n"
        "    virtual void run();\n"
        "    virtual ~BaseWithDestructor() = default;\n"
        "};\n"
        "struct DerivedWithDestructor : public BaseWithDestructor\n"
        "{\n"
        "    virtual ~DerivedWithDestructor()\n"
        "    {\n"
        "        std::cout << \"derived cleanup\\n\";\n"
        "    }\n"
        "    void run();\n"
        "};\n",
        enterpriseOptions());

    require(contains(derivedDestructorResult.modernCode, "~DerivedWithDestructor() override"),
            "derived destructor with custom body should receive override");
    require(!contains(derivedDestructorResult.modernCode, "virtual ~DerivedWithDestructor()"),
            "redundant virtual should be removed from derived destructor when override is added");
    require(contains(derivedDestructorResult.modernCode, "std::cout << \"derived cleanup\\n\";"),
            "derived destructor body should be preserved");
    requireCompilePassIfCompilerAvailable(derivedDestructorResult, "derived destructor override should compile");
}

void runRuleOfZeroTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "struct Resource {};\n"
        "class Owner\n"
        "{\n"
        "public:\n"
        "    Owner()\n"
        "    {\n"
        "        resource = new Resource();\n"
        "    }\n"
        "    ~Owner()\n"
        "    {\n"
        "        delete resource;\n"
        "        resource = nullptr;\n"
        "    }\n"
        "private:\n"
        "    Resource* resource;\n"
        "};\n",
        enterpriseOptions());

    require(contains(result.modernCode, "std::unique_ptr<Resource> resource;"),
            "owned class member should become unique_ptr");
    require(!contains(result.modernCode, "delete resource"), "cleanup-only destructor logic should be removed");
    require(!contains(result.modernCode, "~Owner()"), "cleanup-only destructor should not remain after RAII modernization");
    requireCompilePassIfCompilerAvailable(result, "Rule of Zero modernization should compile");
}

void runAlgorithmModernizationTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult countResult = converter.convert(
        "#include <vector>\n"
        "int countPositive(const std::vector<int>& values)\n"
        "{\n"
        "    int count = 0;\n"
        "    for (const auto& value : values)\n"
        "    {\n"
        "        if (value > 0)\n"
        "        {\n"
        "            ++count;\n"
        "        }\n"
        "    }\n"
        "    return count;\n"
        "}\n",
        enterpriseOptions());

    require(contains(countResult.modernCode, "std::count_if(values.begin(), values.end()"),
            "simple predicate count loop should become count_if");
    require(contains(countResult.modernCode, "#include <algorithm>"), "algorithm modernization should add <algorithm>");
    requireCompilePassIfCompilerAvailable(countResult, "count_if modernization should compile");

    const ConversionResult findResult = converter.convert(
        "#include <vector>\n"
        "bool containsValue(const std::vector<int>& values, int target)\n"
        "{\n"
        "    auto found = values.end();\n"
        "    for (auto it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        if (*it == target)\n"
        "        {\n"
        "            found = it;\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    return found != values.end();\n"
        "}\n",
        enterpriseOptions());

    require(contains(findResult.modernCode, "std::find_if(values.begin(), values.end()"),
            "simple search loop should become find_if");
    require(contains(findResult.modernCode, "return item == target;"),
            "find_if lambda should preserve predicate semantics");
    requireCompilePassIfCompilerAvailable(findResult, "find_if modernization should compile");

    const ConversionResult functorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct Predicate\n"
        "{\n"
        "    bool operator()(int value) const { return value > 0; }\n"
        "};\n"
        "int countMatching(const std::vector<int>& values)\n"
        "{\n"
        "    return std::count_if(values.begin(), values.end(), Predicate());\n"
        "}\n",
        enterpriseOptions());

    require(contains(functorResult.modernCode, "[](const auto& item)"),
            "single-use predicate functor should become an inline lambda\nOutput:\n" + functorResult.modernCode);
    require(contains(functorResult.modernCode, "return item > 0;"),
            "functor predicate body should be preserved in lambda");
    require(!contains(functorResult.modernCode, "struct Predicate"),
            "obsolete single-use predicate functor should be removed");
    requireCompilePassIfCompilerAvailable(functorResult, "functor-to-lambda modernization should compile");

    const ConversionResult statefulFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct AboveLimit\n"
        "{\n"
        "    int threshold;\n"
        "    bool operator()(int value) const { return value > threshold; }\n"
        "};\n"
        "bool hasAbove(const std::vector<int>& values, int limit)\n"
        "{\n"
        "    return std::any_of(values.begin(), values.end(), AboveLimit(limit));\n"
        "}\n",
        enterpriseOptions());

    require(contains(statefulFunctorResult.modernCode, "[threshold = limit](const auto& item)"),
            "simple stateful predicate functor should become a captured lambda");
    require(contains(statefulFunctorResult.modernCode, "return item > threshold;"),
            "stateful functor condition should be preserved");
    require(!contains(statefulFunctorResult.modernCode, "struct AboveLimit"),
            "obsolete stateful single-use predicate functor should be removed");
    requireCompilePassIfCompilerAvailable(statefulFunctorResult, "stateful functor-to-lambda modernization should compile");
}

void runIteratorModernizationTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult vectorResult = converter.convert(
        "#include <vector>\n"
        "int sumValues(const std::vector<int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        total += *it;\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        enterpriseOptions());

    require(contains(vectorResult.modernCode, "for (const auto& value : values)"),
            "const_iterator traversal should become range-based for");
    require(!contains(vectorResult.modernCode, "::const_iterator"), "explicit iterator spelling should be removed");
    requireCompilePassIfCompilerAvailable(vectorResult, "iterator modernization should compile");

    const ConversionResult mapResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void printValues(const std::map<int, int>& values)\n"
        "{\n"
        "    for (std::map<int, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << it->first << it->second << std::endl;\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(mapResult.modernCode, "for (const auto& [key, value] : values)"),
            "map iterator traversal should become structured binding range-for");
    require(contains(mapResult.modernCode, "std::cout << key << value << '\\n';"),
            "map structured binding body should use key/value and newline cleanup");
    requireCompilePassIfCompilerAvailable(mapResult, "map iterator modernization should compile");
}

void runFileIoModernizationTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <cstdio>\n"
        "void writeText(const char* path)\n"
        "{\n"
        "    FILE* file = fopen(path, \"w\");\n"
        "    if (file == nullptr) { return; }\n"
        "    fprintf(file, \"ok\\n\");\n"
        "    fclose(file);\n"
        "}\n",
        enterpriseOptions());

    require(contains(result.modernCode, "std::ofstream file(path);"), "simple FILE* writer should become ofstream");
    require(contains(result.modernCode, "file << \"ok\\n\";"), "simple fprintf literal should become stream write");
    require(!contains(result.modernCode, "fclose(file)"), "manual fclose should be removed");
    requireCompilePassIfCompilerAvailable(result, "FILE* modernization should compile");

    const ConversionResult fputsResult = converter.convert(
        "#include <cstdio>\n"
        "void writeLine(const char* path)\n"
        "{\n"
        "    FILE* file = fopen(path, \"w\");\n"
        "    if (file == nullptr) { return; }\n"
        "    fputs(\"ok\\n\", file);\n"
        "    fclose(file);\n"
        "}\n",
        enterpriseOptions());

    require(contains(fputsResult.modernCode, "std::ofstream file(path);"), "simple FILE* fputs writer should become ofstream");
    require(contains(fputsResult.modernCode, "file << \"ok\\n\";"), "simple fputs literal should become stream write");
    require(!contains(fputsResult.modernCode, "fputs("), "manual fputs should be removed when converted safely");
    requireCompilePassIfCompilerAvailable(fputsResult, "FILE* fputs modernization should compile");
}

void runSemanticConsistencyTests()
{
    const RuleBasedConverterEngine converter;
    const ConversionResult result = converter.convert(
        "#include <memory>\n"
        "struct Item {};\n"
        "void transfer()\n"
        "{\n"
        "    std::auto_ptr<Item> first(new Item());\n"
        "    std::auto_ptr<Item> second;\n"
        "    second = first;\n"
        "}\n",
        enterpriseOptions());

    require(!contains(result.modernCode, "std::auto_ptr"), "auto_ptr should be eradicated");
    require(contains(result.modernCode, "std::make_unique<Item>()"), "auto_ptr new construction should become make_unique");
    require(contains(result.modernCode, "second = std::move(first);"), "auto_ptr transfer should become explicit std::move");
    requireCompilePassIfCompilerAvailable(result, "semantic auto_ptr modernization should compile");

    const ConversionResult sinkResult = converter.convert(
        "#include <memory>\n"
        "struct Node {};\n"
        "void inspect(const Node* node);\n"
        "void run()\n"
        "{\n"
        "    auto node = std::make_unique<Node>();\n"
        "    inspect(node);\n"
        "}\n",
        enterpriseOptions());

    require(contains(sinkResult.modernCode, "inspect(node.get());"),
            "unique_ptr passed to visible raw-pointer observer should use .get()");
    require(!contains(sinkResult.modernCode, "inspect(node);"),
            "unique_ptr should not remain directly passed to raw-pointer sink");
    requireCompilePassIfCompilerAvailable(sinkResult, "smart pointer sink propagation should compile");

    const ConversionResult sharedSinkResult = converter.convert(
        "#include <memory>\n"
        "struct Node {};\n"
        "void inspect(Node* node);\n"
        "void run()\n"
        "{\n"
        "    auto node = std::make_shared<Node>();\n"
        "    inspect(node);\n"
        "}\n",
        enterpriseOptions());

    require(contains(sharedSinkResult.modernCode, "inspect(node.get());"),
            "shared_ptr passed to visible raw-pointer observer should use .get()");
    requireCompilePassIfCompilerAvailable(sharedSinkResult, "shared pointer sink propagation should compile");

    const ConversionResult observerContainerResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "void run()\n"
        "{\n"
        "    std::vector<Node*> observers;\n"
        "    auto node = std::make_unique<Node>();\n"
        "    observers.push_back(node);\n"
        "}\n",
        enterpriseOptions());

    require(contains(observerContainerResult.modernCode, "observers.push_back(node.get());"),
            "raw observer vector should receive a non-owning pointer from smart pointer");
    requireCompilePassIfCompilerAvailable(observerContainerResult, "observer container sink propagation should compile");

    const ConversionResult contractResult = converter.convert(
        "struct Interface\n"
        "{\n"
        "    virtual void execute(int value);\n"
        "};\n"
        "struct Implementation : public Interface\n"
        "{\n"
        "    virtual void execute(int value);\n"
        "};\n",
        enterpriseOptions());

    require(contains(contractResult.modernCode, "virtual ~Interface() = default;"),
            "class with virtual methods should gain virtual destructor");
    require(contains(contractResult.modernCode, "void execute(int value) override;"),
            "derived override should be annotated without relying on ownership conversion");
    requireCompilePassIfCompilerAvailable(contractResult, "polymorphic contract modernization should compile");
}

void runScopedEnumOutputTests()
{
    const RuleBasedConverterEngine converter;

    const ConversionResult streamResult = converter.convert(
        "#include <iostream>\n"
        "enum State { Idle, Running };\n"
        "void printState()\n"
        "{\n"
        "    State state = Running;\n"
        "    std::cout << state << '\\n';\n"
        "}\n",
        enterpriseOptions());

    require(contains(streamResult.modernCode, "enum class State"), "unscoped enum should become enum class before output propagation");
    require(contains(streamResult.modernCode, "std::cout << static_cast<std::underlying_type_t<State>>(state) << '\\n';"),
            "scoped enum variable streamed to cout should be cast to an output-safe value\nOutput:\n" + streamResult.modernCode);
    require(contains(streamResult.modernCode, "#include <type_traits>"), "underlying_type_t cast should add <type_traits>");
    require(hasAppliedRule(streamResult, "Scoped enum output propagation"), "stream output propagation should be tracked");
    requireCompilePassIfCompilerAvailable(streamResult, "scoped enum stream output should compile");

    const ConversionResult getterResult = converter.convert(
        "#include <iostream>\n"
        "#include <memory>\n"
        "enum Mode { Manual, Automatic };\n"
        "struct Controller\n"
        "{\n"
        "    Mode getMode() const { return Automatic; }\n"
        "};\n"
        "void inspect(const std::unique_ptr<Controller>& controller)\n"
        "{\n"
        "    std::cout << controller->getMode() << '\\n';\n"
        "}\n",
        enterpriseOptions());

    require(contains(getterResult.modernCode, "static_cast<std::underlying_type_t<Mode>>(controller->getMode())"),
            "getter returning scoped enum through smart pointer access should be cast before streaming");
    require(!contains(getterResult.modernCode, "controller->static_cast"),
            "scoped enum propagation must not insert casts after member access operators");
    requireCompilePassIfCompilerAvailable(getterResult, "scoped enum getter output should compile");

    const ConversionResult rawPointerGetterResult = converter.convert(
        "#include <iostream>\n"
        "enum Phase { First, Second };\n"
        "struct Reader\n"
        "{\n"
        "    Phase getPhase() const { return Second; }\n"
        "};\n"
        "void inspect(const Reader* reader)\n"
        "{\n"
        "    std::cout << reader->getPhase() << '\\n';\n"
        "}\n",
        enterpriseOptions());

    require(contains(rawPointerGetterResult.modernCode, "static_cast<std::underlying_type_t<Phase>>(reader->getPhase())"),
            "raw pointer member-access getter should be wrapped as one complete enum-valued expression");
    require(!contains(rawPointerGetterResult.modernCode, "reader->static_cast"),
            "raw pointer getter rewrite should not create member-access static_cast syntax");
    requireCompilePassIfCompilerAvailable(rawPointerGetterResult, "raw pointer scoped enum getter output should compile");

    const ConversionResult iteratorGetterResult = converter.convert(
        "#include <iostream>\n"
        "#include <memory>\n"
        "#include <vector>\n"
        "enum Kind { Alpha, Beta };\n"
        "struct Node\n"
        "{\n"
        "    Kind getKind() const { return Beta; }\n"
        "};\n"
        "void printKinds(const std::vector<std::unique_ptr<Node>>& nodes)\n"
        "{\n"
        "    for (auto it = nodes.begin(); it != nodes.end(); ++it)\n"
        "    {\n"
        "        std::cout << (*it)->getKind() << '\\n';\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(iteratorGetterResult.modernCode, "static_cast<std::underlying_type_t<Kind>>((*it)->getKind())")
                || contains(iteratorGetterResult.modernCode, "static_cast<std::underlying_type_t<Kind>>((node)->getKind())")
                || contains(iteratorGetterResult.modernCode, "static_cast<std::underlying_type_t<Kind>>(item->getKind())"),
            "iterator dereference getter should be cast as a complete expression\nOutput:\n" + iteratorGetterResult.modernCode);
    require(!contains(iteratorGetterResult.modernCode, "->static_cast"),
            "iterator getter rewrite should not generate invalid member-access cast syntax");
    requireCompilePassIfCompilerAvailable(iteratorGetterResult, "iterator scoped enum getter output should compile");

    const ConversionResult containerResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "namespace std { template <typename... Args> string format(const char*, Args&&...) { return {}; } }\n"
        "namespace fmt { template <typename... Args> std::string format(const char*, Args&&...) { return {}; } }\n"
        "enum Level : unsigned char { Low, High };\n"
        "struct Entry { Level current; };\n"
        "void printValues()\n"
        "{\n"
        "    std::vector<Level> levels{Low, High};\n"
        "    std::map<int, Level> byId;\n"
        "    byId.emplace(1, High);\n"
        "    Entry entry{Low};\n"
        "    auto choose = []() { return High; };\n"
        "    std::cout << levels[0] << '\\n';\n"
        "    std::cout << byId[1] << '\\n';\n"
        "    std::cout << entry.current << '\\n';\n"
        "    std::cout << choose() << '\\n';\n"
        "    auto one = std::format(\"{}\", High);\n"
        "    auto two = fmt::format(\"{}\", levels[1]);\n"
        "}\n",
        enterpriseOptions());

    require(contains(containerResult.modernCode, "enum class Level : unsigned char"),
            "explicit enum underlying type should be preserved during enum class conversion");
    require(contains(containerResult.modernCode, "static_cast<unsigned char>(levels[0])"),
            "vector element scoped enum output should use explicit underlying type cast");
    require(contains(containerResult.modernCode, "static_cast<unsigned char>(byId[1])"),
            "map value scoped enum output should be cast");
    require(contains(containerResult.modernCode, "static_cast<unsigned char>(entry.current)"),
            "nested member scoped enum output should be cast");
    require(contains(containerResult.modernCode, "static_cast<unsigned char>(choose())"),
            "lambda returning scoped enum should be cast when streamed");
    require(contains(containerResult.modernCode, "std::format(\"{}\", static_cast<unsigned char>(Level::High))"),
            "std::format enum argument should be output-safe");
    require(contains(containerResult.modernCode, "fmt::format(\"{}\", static_cast<unsigned char>(levels[1]))"),
            "fmt::format enum argument should be output-safe");
    require(countOccurrences(containerResult.modernCode, "static_cast<unsigned char>(levels[1])") == 1,
            "format propagation should cast an enum argument exactly once");
    requireCompilePassIfCompilerAvailable(containerResult, "scoped enum container and format output should compile");

    const ConversionResult switchResult = converter.convert(
        "#include <iostream>\n"
        "enum Status { Ready, Busy, Failed };\n"
        "void report(Status status)\n"
        "{\n"
        "    switch (status)\n"
        "    {\n"
        "    case Ready: std::cout << Ready << '\\n'; break;\n"
        "    case Busy:\n"
        "        std::cout << Busy << '\\n';\n"
        "        break;\n"
        "    default:\n"
        "        std::cout << Failed << '\\n';\n"
        "        break;\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(switchResult.modernCode, "switch (status)"),
            "switch expression should not be numerically cast after enum class conversion");
    require(contains(switchResult.modernCode, "case Status::Ready: std::cout << static_cast<std::underlying_type_t<Status>>(Status::Ready) << '\\n'; break;"),
            "single-line case label should remain scoped while only streamed enum is cast\nOutput:\n" + switchResult.modernCode);
    require(contains(switchResult.modernCode, "case Status::Busy:"),
            "multi-line case label should remain a scoped enum label");
    require(!contains(switchResult.modernCode, "case static_cast"),
            "case labels must not be replaced by numeric casts");
    requireCompilePassIfCompilerAvailable(switchResult, "scoped enum switch/output propagation should compile");

    const ModernizationOptions options = enterpriseOptions();
    const ScopedEnumOutputPropagationPass propagationPass;
    const ScopedEnumOutputValidator outputValidator;
    std::vector<ConversionChange> directChanges;
    const std::string directInput =
        "#include <iostream>\n"
        "enum class Status { Ready, Busy };\n"
        "Status current();\n"
        "void report()\n"
        "{\n"
        "    std::cout << current() << '\\n';\n"
        "}\n";
    const std::string once = outputValidator.validateAndRepair(propagationPass.rewrite(directInput, options, directChanges), options, directChanges);
    const std::string twice = outputValidator.validateAndRepair(propagationPass.rewrite(once, options, directChanges), options, directChanges);

    require(once == twice, "running scoped enum output propagation twice should be idempotent");
    require(countOccurrences(twice, "static_cast<std::underlying_type_t<Status>>(current())") == 1,
            "idempotent rewrite should leave exactly one cast around the enum expression");

    const std::string alreadyCastedInput =
        "#include <iostream>\n"
        "#include <type_traits>\n"
        "enum class Status { Ready, Busy };\n"
        "Status current();\n"
        "void report()\n"
        "{\n"
        "    std::cout << static_cast<std::underlying_type_t<Status>>(current()) << '\\n';\n"
        "}\n";
    const std::string alreadyCasted = outputValidator.validateAndRepair(propagationPass.rewrite(alreadyCastedInput, options, directChanges), options, directChanges);
    require(countOccurrences(alreadyCasted, "static_cast<std::underlying_type_t<Status>>(current())") == 1,
            "already static_cast enum expression should not be recast");

    const ScopedEnumCastValidationPass castValidationPass;
    std::vector<ConversionChange> validationChanges;
    const std::string nestedCasts =
        "#include <iostream>\n"
        "#include <type_traits>\n"
        "enum class Status { Ready, Busy };\n"
        "Status current();\n"
        "void report()\n"
        "{\n"
        "    std::cout << static_cast<std::underlying_type_t<Status>>(static_cast<std::underlying_type_t<Status>>(current())) << '\\n';\n"
        "}\n";
    const std::string normalized = castValidationPass.validateAndNormalize(nestedCasts, validationChanges);
    require(countOccurrences(normalized, "static_cast<std::underlying_type_t<Status>>(current())") == 1,
            "nested identical static_cast chains should normalize to one cast");
    require(!contains(normalized, "static_cast<std::underlying_type_t<Status>>(static_cast"),
            "nested scoped enum casts should not remain after validation");

    const std::string misplacedCast =
        "#include <iostream>\n"
        "#include <type_traits>\n"
        "enum class Status { Ready, Busy };\n"
        "struct Reader { Status current() const; };\n"
        "void report(Reader* reader)\n"
        "{\n"
        "    std::cout << reader->static_cast<std::underlying_type_t<Status>>(current()) << '\\n';\n"
        "}\n";
    const std::string repairedMisplacedCast = castValidationPass.validateAndNormalize(misplacedCast, validationChanges);
    require(contains(repairedMisplacedCast, "static_cast<std::underlying_type_t<Status>>(reader->current())"),
            "invalid member-access static_cast placement should be reconstructed around the complete expression");
    require(!contains(repairedMisplacedCast, "->static_cast") && !contains(repairedMisplacedCast, ".static_cast"),
            "member-access static_cast syntax should not remain after validation");
}

void runFinalPolishTests()
{
    const RuleBasedConverterEngine converter;

    const ConversionResult polymorphicResult = converter.convert(
        "struct Interface\n"
        "{\n"
        "    virtual void configure(int value) noexcept;\n"
        "};\n"
        "struct Implementation : public Interface\n"
        "{\n"
        "    virtual ~Implementation()\n"
        "    {\n"
        "    }\n"
        "    virtual void configure(int value) noexcept;\n"
        "};\n",
        enterpriseOptions());

    require(contains(polymorphicResult.modernCode, "virtual ~Interface() = default;"),
            "polymorphic base should receive virtual destructor\nOutput:\n" + polymorphicResult.modernCode);
    require(contains(polymorphicResult.modernCode, "~Implementation() override"),
            "derived destructor should use override when base destructor is virtual");
    require(contains(polymorphicResult.modernCode, "void configure(int value) noexcept override;"),
            "derived method should receive override while preserving noexcept");
    requireCompilePassIfCompilerAvailable(polymorphicResult, "polymorphic polish should compile");

    const ConversionResult iteratorResult = converter.convert(
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
        enterpriseOptions());

    require(contains(iteratorResult.modernCode, "for (const auto& value : values)"),
            "explicit const_iterator loop should become range-based for");
    requireCompilePassIfCompilerAvailable(iteratorResult, "iterator polish should compile");

    const ConversionResult mapResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void print(const std::map<int, int>& values)\n"
        "{\n"
        "    for (std::map<int, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << it->first << it->second << std::endl;\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(mapResult.modernCode, "for (const auto& [key, value] : values)"),
            "map iterator loop should become structured binding");
    requireCompilePassIfCompilerAvailable(mapResult, "structured binding polish should compile");

    const ConversionResult containerResult = converter.convert(
        "#include <map>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "struct Record\n"
        "{\n"
        "    Record(int, const std::string&) {}\n"
        "};\n"
        "void build()\n"
        "{\n"
        "    std::map<int, std::string> names;\n"
        "    names.insert(std::pair<int, std::string>(1, \"one\"));\n"
        "    std::vector<Record> records;\n"
        "    records.push_back(Record(7, \"seven\"));\n"
        "}\n",
        enterpriseOptions());

    require(contains(containerResult.modernCode, "names.emplace(1, \"one\");"),
            "pair insert should become emplace");
    require(contains(containerResult.modernCode, "records.emplace_back(7, \"seven\");"),
            "push_back temporary should become emplace_back");
    requireCompilePassIfCompilerAvailable(containerResult, "container polish should compile");

    const ConversionResult ruleOfZeroResult = converter.convert(
        "#include <string>\n"
        "#include <vector>\n"
        "struct Holder\n"
        "{\n"
        "    Holder() = default;\n"
        "    ~Holder() = default;\n"
        "    Holder(const Holder&) = default;\n"
        "    Holder& operator=(const Holder&) = default;\n"
        "    std::vector<std::string> values;\n"
        "};\n",
        enterpriseOptions());

    require(!contains(ruleOfZeroResult.modernCode, "~Holder() = default;"),
            "explicit default destructor should be removed by Rule of Zero polish");
    require(!contains(ruleOfZeroResult.modernCode, "Holder(const Holder&) = default;"),
            "explicit default copy constructor should be removed by Rule of Zero polish");
    require(!contains(ruleOfZeroResult.modernCode, "operator=(const Holder&) = default;"),
            "explicit default copy assignment should be removed by Rule of Zero polish");
    requireCompilePassIfCompilerAvailable(ruleOfZeroResult, "Rule of Zero polish should compile");

    ModernizationOptions stringViewOptions = enterpriseOptions();
    stringViewOptions.applyStringViewWhenSafe = true;
    const ConversionResult stringViewResult = converter.convert(
        "#include <iostream>\n"
        "#include <string>\n"
        "void show(const std::string& name)\n"
        "{\n"
        "    std::cout << name << '\\n';\n"
        "}\n",
        stringViewOptions);

    require(contains(stringViewResult.modernCode, "#include <string_view>"),
            "string_view polish should add string_view include when applied");
    require(contains(stringViewResult.modernCode, "void show(std::string_view name)"),
            "safe read-only string parameter should become string_view when enabled");
    requireCompilePassIfCompilerAvailable(stringViewResult, "string_view polish should compile");

    const ConversionResult unsafeStringViewResult = converter.convert(
        "#include <string>\n"
        "std::string stored;\n"
        "void store(const std::string& name)\n"
        "{\n"
        "    stored = name;\n"
        "}\n",
        stringViewOptions);

    require(contains(unsafeStringViewResult.modernCode, "const std::string& name"),
            "escaping string parameter should not be auto-converted to string_view\nOutput:\n" + unsafeStringViewResult.modernCode);
    requireCompilePassIfCompilerAvailable(unsafeStringViewResult, "unsafe string_view candidate should remain compilable");
}

void runRegressionModernizationTests()
{
    const RuleBasedConverterEngine converter;

    const ConversionResult crossFunctionResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Item {};\n"
        "void inspectAll(const std::vector<Item*>& items)\n"
        "{\n"
        "    for (Item* item : items)\n"
        "    {\n"
        "        (void)item;\n"
        "    }\n"
        "}\n"
        "void run()\n"
        "{\n"
        "    std::vector<Item*> items;\n"
        "    items.push_back(new Item());\n"
        "    inspectAll(items);\n"
        "    for (Item* item : items)\n"
        "    {\n"
        "        delete item;\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(crossFunctionResult.modernCode, "std::vector<std::unique_ptr<Item>> items;"),
            "owning raw pointer vector should become vector<unique_ptr>");
    const bool signatureUpdated = contains(crossFunctionResult.modernCode, "inspectAll(const std::vector<std::unique_ptr<Item>>& items)")
        || contains(crossFunctionResult.modernCode, "inspectAll(const std::vector<std::unique_ptr<Item> >& items)");
    const bool callAdapted = contains(crossFunctionResult.modernCode, ".get()")
        && contains(crossFunctionResult.modernCode, "std::vector<Item*>")
        && !contains(crossFunctionResult.modernCode, "inspectAll(items);");
    require(signatureUpdated || callAdapted,
            "vector<unique_ptr<T>> should not be passed directly to vector<T*> APIs\nOutput:\n" + crossFunctionResult.modernCode);
    requireCompilePassIfCompilerAvailable(crossFunctionResult, "cross-function smart pointer propagation should compile");

    const ConversionResult stringBufferResult = converter.convert(
        "#include <cstring>\n"
        "#include <string>\n"
        "class TextBuffer\n"
        "{\n"
        "public:\n"
        "    explicit TextBuffer(const char* input)\n"
        "    {\n"
        "        text = new char[std::strlen(input) + 1];\n"
        "        std::strcpy(text, input);\n"
        "    }\n"
        "    TextBuffer(const TextBuffer& other)\n"
        "    {\n"
        "        text = new char[std::strlen(other.text) + 1];\n"
        "        std::strcpy(text, other.text);\n"
        "    }\n"
        "    TextBuffer& operator=(const TextBuffer& other)\n"
        "    {\n"
        "        if (this != &other)\n"
        "        {\n"
        "            delete[] text;\n"
        "            text = new char[std::strlen(other.text) + 1];\n"
        "            std::strcpy(text, other.text);\n"
        "        }\n"
        "        return *this;\n"
        "    }\n"
        "    ~TextBuffer()\n"
        "    {\n"
        "        delete[] text;\n"
        "    }\n"
        "    const char* c_str() const { return text; }\n"
        "    std::size_t length() const { return std::strlen(text); }\n"
        "private:\n"
        "    char* text;\n"
        "};\n",
        enterpriseOptions());

    require(contains(stringBufferResult.modernCode, "std::string text;"),
            "owned class char* text buffer should become std::string\nOutput:\n" + stringBufferResult.modernCode);
    require(!contains(stringBufferResult.modernCode, "new char["), "string buffer modernization should remove new char[]");
    require(!contains(stringBufferResult.modernCode, "delete[] text"), "string buffer modernization should remove delete[]");
    require(!contains(stringBufferResult.modernCode, "std::strcpy"), "string buffer modernization should remove strcpy");
    require(contains(stringBufferResult.modernCode, "return text.c_str();"), "C string getter should return std::string::c_str()");
    require(contains(stringBufferResult.modernCode, "return text.size();"), "length getter should return std::string::size()");
    require(!contains(stringBufferResult.modernCode, "~TextBuffer()"), "cleanup-only destructor should be removed after string modernization");
    requireCompilePassIfCompilerAvailable(stringBufferResult, "class string buffer modernization should compile");

    const ConversionResult mutexResult = converter.convert(
        "#include <mutex>\n"
        "std::mutex gate;\n"
        "int value = 0;\n"
        "void update()\n"
        "{\n"
        "    gate.lock();\n"
        "    ++value;\n"
        "    gate.unlock();\n"
        "}\n",
        enterpriseOptions());

    require(contains(mutexResult.modernCode, "std::lock_guard<std::mutex>") || contains(mutexResult.modernCode, "std::scoped_lock"),
            "same-scope mutex lock/unlock should become an RAII guard\nOutput:\n" + mutexResult.modernCode);
    require(!contains(mutexResult.modernCode, "gate.unlock();"), "safe mutex RAII conversion should remove manual unlock");
    requireCompilePassIfCompilerAvailable(mutexResult, "mutex RAII modernization should compile");

    const ConversionResult unsafeMutexResult = converter.convert(
        "#include <mutex>\n"
        "std::mutex gate;\n"
        "int value = 0;\n"
        "void update(bool skip)\n"
        "{\n"
        "    gate.lock();\n"
        "    if (skip)\n"
        "    {\n"
        "        gate.unlock();\n"
        "        return;\n"
        "    }\n"
        "    ++value;\n"
        "    gate.unlock();\n"
        "}\n",
        enterpriseOptions());

    require(contains(unsafeMutexResult.modernCode, "gate.lock();") && contains(unsafeMutexResult.modernCode, "gate.unlock();"),
            "branching lock/unlock should remain unchanged when RAII conversion is unsafe");
    require(hasSuggestionRule(unsafeMutexResult, "mutex") || hasSuggestionRule(unsafeMutexResult, "lock"),
            "unsafe lock/unlock pattern should emit a modernization suggestion");
    requireCompilePassIfCompilerAvailable(unsafeMutexResult, "unsafe mutex pattern should remain compilable");

    const ConversionResult threadResult = converter.convert(
        "#include <thread>\n"
        "void work() {}\n"
        "void run()\n"
        "{\n"
        "    std::thread worker(work);\n"
        "    worker.join();\n"
        "}\n",
        enterpriseOptions());

    require(contains(threadResult.modernCode, "std::thread worker(work);"),
            "thread join modernization should remain conservative by default");
    require(hasSuggestionRule(threadResult, "thread") || hasSuggestionRule(threadResult, "jthread"),
            "manual thread join should emit a RAII/jthread suggestion");
    requireCompilePassIfCompilerAvailable(threadResult, "thread suggestion-only modernization should compile");

    const ConversionResult functorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct IsLarge\n"
        "{\n"
        "    bool operator()(int value) const { return value > 10; }\n"
        "};\n"
        "bool hasLarge(const std::vector<int>& values)\n"
        "{\n"
        "    return std::any_of(values.begin(), values.end(), IsLarge());\n"
        "}\n",
        enterpriseOptions());

    require(contains(functorResult.modernCode, "[](const auto& item)"),
            "single-use predicate functor should become an inline lambda");
    require(!contains(functorResult.modernCode, "struct IsLarge"), "obsolete single-use functor should be removed");
    requireCompilePassIfCompilerAvailable(functorResult, "functor-to-lambda regression should compile");

    const ConversionResult loopResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "#include <vector>\n"
        "void printValues(const std::vector<int>& values, const std::map<int, int>& lookup)\n"
        "{\n"
        "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << std::endl;\n"
        "    }\n"
        "    for (std::map<int, int>::const_iterator it = lookup.begin(); it != lookup.end(); ++it)\n"
        "    {\n"
        "        std::cout << it->first << it->second << std::endl;\n"
        "    }\n"
        "}\n",
        enterpriseOptions());

    require(contains(loopResult.modernCode, "for (const auto& value : values)"),
            "safe vector iterator loop should become range-for");
    require(contains(loopResult.modernCode, "for (const auto& [key, value] : lookup)"),
            "safe map iterator loop should become structured binding");
    requireCompilePassIfCompilerAvailable(loopResult, "iterator and structured binding regression should compile");

    const std::filesystem::path root = makeTempDirectory("moderncpp_regression_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "#include <mutex>\n"
        "std::mutex gate;\n"
        "int value = 0;\n"
        "void update()\n"
        "{\n"
        "    gate.lock();\n"
        "    ++value;\n"
        "    gate.unlock();\n"
        "}\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/regression";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::Balanced;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoModernized = readTextFile(root / "src" / "legacy.cpp");
    require(repoResult.filesModified == 1, "repository regression fixture should be modified");
    require(contains(repoModernized, "std::lock_guard<std::mutex>") || contains(repoModernized, "std::scoped_lock"),
            "repository mode should use the same mutex RAII modernization pipeline");
}

void runQualitySprintTests()
{
    const RuleBasedConverterEngine converter;
    const ModernizationOptions options = qualitySprintOptions();

    const ConversionResult iteratorResult = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(const std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << *it << '\\n';\n"
        "    }\n"
        "}\n",
        options);
    require(contains(iteratorResult.modernCode, "for (const auto& value : values)"),
            "explicit const_iterator loop should become range-for\nOutput:\n" + iteratorResult.modernCode);
    requireCompilePassIfCompilerAvailable(iteratorResult, "iterator range-for modernization should compile");

    const ConversionResult unsafeIteratorResult = converter.convert(
        "#include <vector>\n"
        "void removeZeros(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        if (*it == 0)\n"
        "        {\n"
        "            it = values.erase(it);\n"
        "        }\n"
        "    }\n"
        "}\n",
        options);
    require(contains(unsafeIteratorResult.modernCode, "std::vector<int>::iterator it"),
            "iterator erase loop should not be converted to range-for");
    requireCompilePassIfCompilerAvailable(unsafeIteratorResult, "unsafe iterator loop should remain compilable");

    const ConversionResult mapLoopResult = converter.convert(
        "#include <iostream>\n"
        "#include <map>\n"
        "void print(const std::map<int, int>& values)\n"
        "{\n"
        "    for (std::map<int, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        std::cout << it->first << ':' << it->second << '\\n';\n"
        "    }\n"
        "}\n",
        options);
    require(contains(mapLoopResult.modernCode, "for (const auto& [key, value] : values)"),
            "map iterator loop should become structured binding\nOutput:\n" + mapLoopResult.modernCode);
    requireCompilePassIfCompilerAvailable(mapLoopResult, "map structured binding modernization should compile");

    const ConversionResult unsafeMapLoopResult = converter.convert(
        "#include <map>\n"
        "void prune(std::map<int, int>& values)\n"
        "{\n"
        "    for (std::map<int, int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        if (it->second == 0)\n"
        "        {\n"
        "            values.erase(it);\n"
        "        }\n"
        "    }\n"
        "}\n",
        options);
    require(contains(unsafeMapLoopResult.modernCode, "std::map<int, int>::iterator it"),
            "map erase loop should not be converted to structured binding");
    requireCompilePassIfCompilerAvailable(unsafeMapLoopResult, "unsafe map iterator loop should remain compilable");

    const ConversionResult indexLoopResult = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void print(const std::vector<int>& values)\n"
        "{\n"
        "    for (std::size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        std::cout << values[index] << '\\n';\n"
        "    }\n"
        "}\n",
        options);
    require(contains(indexLoopResult.modernCode, "for (const auto& value : values)"),
            "safe index loop should become range-for\nOutput:\n" + indexLoopResult.modernCode);
    require(!contains(indexLoopResult.modernCode, "values[index]"), "converted range loop should remove indexed access");
    requireCompilePassIfCompilerAvailable(indexLoopResult, "index range-for modernization should compile");

    const ConversionResult stringViewCStringResult = converter.convert(
        "#include <string>\n"
        "void legacy(const char* text);\n"
        "void send(const std::string& text)\n"
        "{\n"
        "    legacy(text.c_str());\n"
        "}\n",
        options);
    require(!contains(stringViewCStringResult.modernCode, "std::string_view text")
                || !contains(stringViewCStringResult.modernCode, "text.c_str()"),
            "string_view conversion must not leave invalid .c_str() calls\nOutput:\n" + stringViewCStringResult.modernCode);
    requireCompilePassIfCompilerAvailable(stringViewCStringResult, "string_view c_str safety should compile");

    const ConversionResult unsafeStringViewResult = converter.convert(
        "#include <string>\n"
        "void legacy(const char* text);\n"
        "void sendTwice(const std::string& text)\n"
        "{\n"
        "    legacy(text.c_str());\n"
        "    legacy(text.c_str());\n"
        "}\n",
        options);
    require(contains(unsafeStringViewResult.modernCode, "const std::string& text"),
            "repeated C-string API use should roll back string_view conversion\nOutput:\n" + unsafeStringViewResult.modernCode);
    requireCompilePassIfCompilerAvailable(unsafeStringViewResult, "unsafe string_view candidate should remain compilable");

    const ConversionResult moveResult = converter.convert(
        "#include <string>\n"
        "#include <vector>\n"
        "void append(std::vector<std::string>& values, std::string value)\n"
        "{\n"
        "    values.push_back(value);\n"
        "}\n",
        options);
    require(contains(moveResult.modernCode, "values.push_back(std::move(value));")
                || contains(moveResult.modernCode, "values.emplace_back(std::move(value));"),
            "unused local value transferred to container should use std::move\nOutput:\n" + moveResult.modernCode);
    requireCompilePassIfCompilerAvailable(moveResult, "safe move modernization should compile");

    const ConversionResult noMoveResult = converter.convert(
        "#include <iostream>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "void appendAndPrint(std::vector<std::string>& values, std::string value)\n"
        "{\n"
        "    values.push_back(value);\n"
        "    std::cout << value << '\\n';\n"
        "}\n",
        options);
    require(!contains(noMoveResult.modernCode, "std::move(value)"),
            "value used after push_back should not be moved");
    requireCompilePassIfCompilerAvailable(noMoveResult, "move rejection case should compile");

    const ConversionResult swapResult = converter.convert(
        "#include <string>\n"
        "void exchange(std::string& left, std::string& right)\n"
        "{\n"
        "    std::string temp = left;\n"
        "    left = right;\n"
        "    right = temp;\n"
        "}\n",
        options);
    require(contains(swapResult.modernCode, "std::swap(left, right);")
                || (contains(swapResult.modernCode, "using std::swap;") && contains(swapResult.modernCode, "swap(left, right);")),
            "manual temporary swap should become std::swap\nOutput:\n" + swapResult.modernCode);
    requireCompilePassIfCompilerAvailable(swapResult, "swap modernization should compile");

    const ConversionResult noexceptResult = converter.convert(
        "struct Counter\n"
        "{\n"
        "    int value;\n"
        "    int get() const { return value; }\n"
        "};\n",
        options);
    require(contains(noexceptResult.modernCode, "int get() const noexcept"),
            "simple non-throwing getter should become noexcept\nOutput:\n" + noexceptResult.modernCode);
    requireCompilePassIfCompilerAvailable(noexceptResult, "noexcept modernization should compile");

    const ConversionResult nsdmiResult = converter.convert(
        "class Settings\n"
        "{\n"
        "public:\n"
        "    Settings()\n"
        "    {\n"
        "        count = 0;\n"
        "        enabled = false;\n"
        "    }\n"
        "    explicit Settings(int seed)\n"
        "    {\n"
        "        count = 0;\n"
        "        enabled = false;\n"
        "        (void)seed;\n"
        "    }\n"
        "private:\n"
        "    int count;\n"
        "    bool enabled;\n"
        "};\n",
        options);
    require(contains(nsdmiResult.modernCode, "int count = 0;")
                && contains(nsdmiResult.modernCode, "bool enabled = false;"),
            "shared constructor defaults should become NSDMI\nOutput:\n" + nsdmiResult.modernCode);
    require(!contains(nsdmiResult.modernCode, "\n        count = 0;"),
            "constructor default assignment should be removed after NSDMI");
    requireCompilePassIfCompilerAvailable(nsdmiResult, "NSDMI modernization should compile");

    const ConversionResult cHeaderResult = converter.convert(
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "void print(const char* text)\n"
        "{\n"
        "    printf(\"%s\", text);\n"
        "    strlen(text);\n"
        "}\n",
        options);
    require(contains(cHeaderResult.modernCode, "#include <iostream>")
                && contains(cHeaderResult.modernCode, "#include <cstring>"),
            "C headers and converted printf output should use the required C++ headers\nOutput:\n" + cHeaderResult.modernCode);
    require(contains(cHeaderResult.modernCode, "std::cout << text;") && contains(cHeaderResult.modernCode, "std::strlen"),
            "C header modernization should qualify remaining C symbols and printf modernization should use iostream output");
    requireCompilePassIfCompilerAvailable(cHeaderResult, "C header modernization should compile");

    const ConversionResult constantResult = converter.convert(
        "struct Limits\n"
        "{\n"
        "    enum { MaxItems = 8 };\n"
        "};\n",
        options);
    require(contains(constantResult.modernCode, "static constexpr")
                && contains(constantResult.modernCode, "MaxItems")
                && !contains(constantResult.modernCode, "enum {"),
            "enum constant hack should become constexpr where safe\nOutput:\n" + constantResult.modernCode);
    requireCompilePassIfCompilerAvailable(constantResult, "constexpr enum-hack modernization should compile");

    const ConversionResult trailingReturnResult = converter.convert(
        "#include <utility>\n"
        "template <typename Container>\n"
        "decltype(std::declval<Container&>().begin()) first(Container& container)\n"
        "{\n"
        "    return container.begin();\n"
        "}\n",
        options);
    require(hasSuggestionRule(trailingReturnResult, "Trailing return type"),
            "complex dependent return type should produce a trailing-return suggestion");
    requireCompilePassIfCompilerAvailable(trailingReturnResult, "trailing return suggestion should keep code compilable");

    const ConversionResult rollbackResult = converter.convert(
        "#include <string>\n"
        "void legacy(const char* text);\n"
        "void send(const std::string& text)\n"
        "{\n"
        "    legacy(text.c_str());\n"
        "}\n",
        options);
    require(!contains(rollbackResult.modernCode, "std::string_view text") || rollbackResult.compileVerificationPassed,
            "compile-breaking string_view transformation should be repaired or rolled back");
    requireCompilePassIfCompilerAvailable(rollbackResult, "compile-fix rollback sample should compile");

    const std::filesystem::path root = makeTempDirectory("moderncpp_quality_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "#include <stdio.h>\n"
        "#include <string>\n"
        "void exchange(std::string& left, std::string& right)\n"
        "{\n"
        "    std::string temp = left;\n"
        "    left = right;\n"
        "    right = temp;\n"
        "    printf(\"done\");\n"
        "}\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/quality";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoModernized = readTextFile(root / "src" / "legacy.cpp");
    require(repoResult.filesModified == 1, "repository quality fixture should be modified");
    require(contains(repoModernized, "#include <iostream>"), "repository mode should apply printf output modernization");
    require(contains(repoModernized, "std::swap(left, right);")
                || (contains(repoModernized, "using std::swap;") && contains(repoModernized, "swap(left, right);")),
            "repository mode should use the same swap modernization pass");
}

void runCrossScopeTypePropagationTests()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = enterpriseOptions();
    options.offlineModernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    options.compileVerificationEnabled = true;
    options.applyStringViewWhenSafe = true;

    const ConversionResult visibleFunctionResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "std::size_t countNodes(const std::vector<Node*>& nodes)\n"
        "{\n"
        "    return nodes.size();\n"
        "}\n"
        "std::size_t build()\n"
        "{\n"
        "    std::vector<Node*> nodes;\n"
        "    nodes.push_back(new Node());\n"
        "    const std::size_t count = countNodes(nodes);\n"
        "    for (Node* node : nodes)\n"
        "    {\n"
        "        delete node;\n"
        "    }\n"
        "    return count;\n"
        "}\n",
        options);
    require(contains(visibleFunctionResult.modernCode, "std::vector<std::unique_ptr<Node>> nodes;"),
            "local owning raw pointer vector should become vector<unique_ptr>");
    require(contains(visibleFunctionResult.modernCode, "countNodes(const std::vector<std::unique_ptr<Node>>& nodes)"),
            "visible raw-pointer vector function parameter should propagate to vector<unique_ptr>\nOutput:\n" + visibleFunctionResult.modernCode);
    require(contains(visibleFunctionResult.modernCode, "countNodes(nodes);"),
            "callsite should remain direct after visible signature propagation");
    require(!contains(visibleFunctionResult.modernCode, "nodesRawView"),
            "visible signature propagation should be preferred over adapter view");
    requireCompilePassIfCompilerAvailable(visibleFunctionResult, "visible function vector propagation should compile");

    const ConversionResult methodResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "struct Inspector\n"
        "{\n"
        "    void inspect(const std::vector<Node*>& nodes)\n"
        "    {\n"
        "        (void)nodes.size();\n"
        "    }\n"
        "};\n"
        "void build(Inspector& inspector)\n"
        "{\n"
        "    std::vector<Node*> nodes;\n"
        "    nodes.push_back(new Node());\n"
        "    inspector.inspect(nodes);\n"
        "    for (Node* node : nodes)\n"
        "    {\n"
        "        delete node;\n"
        "    }\n"
        "}\n",
        options);
    require(contains(methodResult.modernCode, "inspect(const std::vector<std::unique_ptr<Node>>& nodes)"),
            "visible method parameter should propagate to vector<unique_ptr>\nOutput:\n" + methodResult.modernCode);
    require(contains(methodResult.modernCode, "inspector.inspect(nodes);"),
            "method call should remain direct after signature propagation");
    requireCompilePassIfCompilerAvailable(methodResult, "visible method vector propagation should compile");

    const ConversionResult uniquePtrHelperResult = converter.convert(
        "#include <memory>\n"
        "struct Node { int value = 0; };\n"
        "int inspect(Node* node)\n"
        "{\n"
        "    return node->value;\n"
        "}\n"
        "int build()\n"
        "{\n"
        "    Node* node = new Node();\n"
        "    const int value = inspect(node);\n"
        "    delete node;\n"
        "    return value;\n"
        "}\n",
        options);
    require(contains(uniquePtrHelperResult.modernCode, "std::unique_ptr<Node>")
                || contains(uniquePtrHelperResult.modernCode, "std::make_unique<Node>"),
            "local raw owner should become unique_ptr");
    const bool uniquePtrSignatureUpdated = contains(uniquePtrHelperResult.modernCode, "inspect(const std::unique_ptr<Node>& node)");
    require(uniquePtrSignatureUpdated || contains(uniquePtrHelperResult.modernCode, "inspect(node.get())"),
            "unique_ptr consumer should be updated by signature propagation or safe .get() adaptation\nOutput:\n" + uniquePtrHelperResult.modernCode);
    require(uniquePtrSignatureUpdated || !contains(uniquePtrHelperResult.modernCode, "inspect(node);"),
            "unique_ptr should not be passed directly to raw pointer helper");
    requireCompilePassIfCompilerAvailable(uniquePtrHelperResult, "unique_ptr helper propagation should compile");

    const ConversionResult sharedPtrHelperResult = converter.convert(
        "#include <memory>\n"
        "struct Node { int value = 0; };\n"
        "int inspect(Node* node)\n"
        "{\n"
        "    return node->value;\n"
        "}\n"
        "int build()\n"
        "{\n"
        "    auto node = std::make_shared<Node>();\n"
        "    return inspect(node);\n"
        "}\n",
        options);
    const bool sharedPtrSignatureUpdated = contains(sharedPtrHelperResult.modernCode, "inspect(const std::shared_ptr<Node>& node)");
    require(sharedPtrSignatureUpdated || contains(sharedPtrHelperResult.modernCode, "inspect(node.get())"),
            "shared_ptr consumer should be updated by signature propagation or safe .get() adaptation\nOutput:\n" + sharedPtrHelperResult.modernCode);
    require(sharedPtrSignatureUpdated || !contains(sharedPtrHelperResult.modernCode, "inspect(node);"),
            "shared_ptr should not be passed directly to raw pointer helper");
    requireCompilePassIfCompilerAvailable(sharedPtrHelperResult, "shared_ptr helper propagation should compile");

    const ConversionResult stringViewApiResult = converter.convert(
        "#include <string>\n"
        "void legacy(const char* text);\n"
        "void send(const std::string& text)\n"
        "{\n"
        "    legacy(text.c_str());\n"
        "}\n",
        options);
    require(!contains(stringViewApiResult.modernCode, "std::string_view text")
                || !contains(stringViewApiResult.modernCode, "text.c_str()"),
            "string_view propagation must repair or roll back C string API usage\nOutput:\n" + stringViewApiResult.modernCode);
    requireCompilePassIfCompilerAvailable(stringViewApiResult, "string_view API propagation should compile");

    const ConversionResult vectorReturnResult = converter.convert(
        "#include <vector>\n"
        "struct Item {};\n"
        "class Store\n"
        "{\n"
        "public:\n"
        "    Store(int count)\n"
        "    {\n"
        "        items = new Item[count];\n"
        "    }\n"
        "    const Item* getItems() const { return items; }\n"
        "    ~Store()\n"
        "    {\n"
        "        delete[] items;\n"
        "    }\n"
        "private:\n"
        "    Item* items;\n"
        "};\n",
        options);
    require(contains(vectorReturnResult.modernCode, "std::vector<Item> items;"),
            "raw array member should become vector");
    require(!contains(vectorReturnResult.modernCode, "const Item* getItems() const { return items; }"),
            "vector member must not be returned as a raw pointer object");
    require(contains(vectorReturnResult.modernCode, "const std::vector<Item>& getItems() const { return items; }")
                || contains(vectorReturnResult.modernCode, "return items.data();"),
            "vector return API should be propagated or adapted safely\nOutput:\n" + vectorReturnResult.modernCode);
    requireCompilePassIfCompilerAvailable(vectorReturnResult, "vector return propagation should compile");

    const ConversionResult classMemberApiResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "class Registry\n"
        "{\n"
        "public:\n"
        "    void add()\n"
        "    {\n"
        "        nodes.push_back(new Node());\n"
        "    }\n"
        "    const std::vector<Node*>& getNodes() const { return nodes; }\n"
        "    ~Registry()\n"
        "    {\n"
        "        for (Node* node : nodes)\n"
        "        {\n"
        "            delete node;\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    std::vector<Node*> nodes;\n"
        "};\n",
        options);
    require(contains(classMemberApiResult.modernCode, "std::vector<std::unique_ptr<Node>> nodes;"),
            "owning class vector member should become vector<unique_ptr>");
    require(contains(classMemberApiResult.modernCode, "const std::vector<std::unique_ptr<Node>>& getNodes() const { return nodes; }"),
            "class getter return type should propagate to vector<unique_ptr>\nOutput:\n" + classMemberApiResult.modernCode);
    require(!contains(classMemberApiResult.modernCode, "std::vector<Node*>& getNodes"),
            "class getter should not expose stale raw pointer vector type");
    requireCompilePassIfCompilerAvailable(classMemberApiResult, "class member API propagation should compile");

    const ConversionResult multipleCallsitesResult = converter.convert(
        "#include <memory>\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "void inspect(const std::vector<Node*>& nodes)\n"
        "{\n"
        "    (void)nodes.size();\n"
        "}\n"
        "void build()\n"
        "{\n"
        "    std::vector<Node*> nodes;\n"
        "    nodes.push_back(new Node());\n"
        "    inspect(nodes);\n"
        "    inspect(nodes);\n"
        "    for (Node* node : nodes)\n"
        "    {\n"
        "        delete node;\n"
        "    }\n"
        "}\n",
        options);
    require(contains(multipleCallsitesResult.modernCode, "inspect(const std::vector<std::unique_ptr<Node>>& nodes)"),
            "visible signature should update once for multiple callsites");
    require(countOccurrences(multipleCallsitesResult.modernCode, "inspect(nodes);") == 2,
            "all callsites should remain direct after visible signature propagation\nOutput:\n" + multipleCallsitesResult.modernCode);
    requireCompilePassIfCompilerAvailable(multipleCallsitesResult, "multiple callsite propagation should compile");

    const std::filesystem::path root = makeTempDirectory("moderncpp_cross_scope_repo");
    writeTextFile(root / "include" / "api.hpp",
        "#pragma once\n"
        "#include <vector>\n"
        "struct Node {};\n"
        "void inspect(const std::vector<Node*>& nodes);\n");
    writeTextFile(root / "src" / "main.cpp",
        "#include <memory>\n"
        "#include <vector>\n"
        "#include \"../include/api.hpp\"\n"
        "void build()\n"
        "{\n"
        "    std::vector<Node*> nodes;\n"
        "    nodes.push_back(new Node());\n"
        "    inspect(nodes);\n"
        "    for (Node* node : nodes)\n"
        "    {\n"
        "        delete node;\n"
        "    }\n"
        "}\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/cross-scope";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoMain = readTextFile(root / "src" / "main.cpp");
    require(repoResult.filesModified >= 1, "repository cross-scope fixture should be modified");
    require(!contains(repoMain, "inspect(nodes);")
                || contains(repoMain, "nodesRawView")
                || contains(readTextFile(root / "include" / "api.hpp"), "std::vector<std::unique_ptr<Node>>"),
            "repository mode should adapt cross-file raw-vector consumers or update visible declarations\nOutput:\n" + repoMain);
}

void runNsdmiScopeSafetyTests()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = qualitySprintOptions();
    options.compileVerificationEnabled = true;

    const ConversionResult constructorParameterResult = converter.convert(
        "class BoundedStore\n"
        "{\n"
        "public:\n"
        "    explicit BoundedStore(int requested)\n"
        "    {\n"
        "        limit = requested;\n"
        "        total = 0;\n"
        "    }\n"
        "private:\n"
        "    int limit;\n"
        "    int total;\n"
        "};\n",
        options);
    require(!contains(constructorParameterResult.modernCode, "int limit = requested;"),
            "NSDMI must not reference constructor parameters at class scope\nOutput:\n" + constructorParameterResult.modernCode);
    require(contains(constructorParameterResult.modernCode, "limit = requested;"),
            "constructor-dependent member initialization should remain in constructor scope");
    require(contains(constructorParameterResult.modernCode, "int total = 0;"),
            "constant constructor defaults may still become NSDMI");
    requireCompilePassIfCompilerAvailable(constructorParameterResult, "constructor-parameter NSDMI safety sample should compile");

    const ConversionResult loopBoundResult = converter.convert(
        "class LoopBounded\n"
        "{\n"
        "public:\n"
        "    explicit LoopBounded(int count)\n"
        "    {\n"
        "        bound = count;\n"
        "        sum = 0;\n"
        "        for (int index = 0; index < bound; ++index)\n"
        "        {\n"
        "            sum += index;\n"
        "        }\n"
        "    }\n"
        "private:\n"
        "    int bound;\n"
        "    int sum;\n"
        "};\n",
        options);
    const std::size_t assignmentPosition = loopBoundResult.modernCode.find("bound = count;");
    const std::size_t loopPosition = loopBoundResult.modernCode.find("for (int index = 0; index < bound; ++index)");
    require(assignmentPosition != std::string::npos && loopPosition != std::string::npos && assignmentPosition < loopPosition,
            "constructor loop bound must be initialized before the loop executes\nOutput:\n" + loopBoundResult.modernCode);
    require(!contains(loopBoundResult.modernCode, "int bound = count;"),
            "loop-bound member must not be moved to class scope with a constructor parameter");
    requireCompilePassIfCompilerAvailable(loopBoundResult, "loop-bound initialization should compile");

    const ConversionResult invalidInitializerResult = converter.convert(
        "class RepairableInlineInitializer\n"
        "{\n"
        "public:\n"
        "    explicit RepairableInlineInitializer(int requested)\n"
        "    {\n"
        "    }\n"
        "private:\n"
        "    int limit = requested;\n"
        "};\n",
        options);
    require(!contains(invalidInitializerResult.modernCode, "int limit = requested;"),
            "scope validator should remove inline initializers that reference constructor parameters\nOutput:\n" + invalidInitializerResult.modernCode);
    require(contains(invalidInitializerResult.modernCode, "limit = requested;"),
            "scope validator should repair invalid NSDMI by moving initialization back into the constructor");
    require(hasAppliedRule(invalidInitializerResult, "NsdmiScopeSafetyPass"),
            "invalid NSDMI repair should be tracked as an applied change");
    requireCompilePassIfCompilerAvailable(invalidInitializerResult, "repaired invalid NSDMI should compile");

    const ConversionResult localInitializerResult = converter.convert(
        "class LocalDependentInitializer\n"
        "{\n"
        "public:\n"
        "    explicit LocalDependentInitializer(int input)\n"
        "    {\n"
        "        int adjusted = input + 1;\n"
        "        value = adjusted;\n"
        "    }\n"
        "private:\n"
        "    int value = adjusted;\n"
        "};\n",
        options);
    require(!contains(localInitializerResult.modernCode, "int value = adjusted;"),
            "scope validator should remove inline initializers that reference constructor locals");
    require(contains(localInitializerResult.modernCode, "value = adjusted;"),
            "constructor-local dependent initialization should remain after the local declaration");
    requireCompilePassIfCompilerAvailable(localInitializerResult, "constructor-local NSDMI repair should compile");

    const ConversionResult constRefResult = converter.convert(
        "struct Payload\n"
        "{\n"
        "    int value;\n"
        "};\n"
        "int inspect(Payload payload)\n"
        "{\n"
        "    return payload.value;\n"
        "}\n",
        options);
    require(contains(constRefResult.modernCode, "int inspect(const Payload& payload)"),
            "read-only non-trivial by-value parameter should become const reference\nOutput:\n" + constRefResult.modernCode);
    requireCompilePassIfCompilerAvailable(constRefResult, "pass-by-const-ref modernization should compile");

    const ConversionResult moveIdiomResult = converter.convert(
        "#include <string>\n"
        "#include <utility>\n"
        "class TextSink\n"
        "{\n"
        "public:\n"
        "    void set(std::string value)\n"
        "    {\n"
        "        text = std::move(value);\n"
        "    }\n"
        "private:\n"
        "    std::string text;\n"
        "};\n",
        options);
    require(contains(moveIdiomResult.modernCode, "void set(std::string value)"),
            "pass-by-value-and-move sink parameter should not become const reference\nOutput:\n" + moveIdiomResult.modernCode);
    requireCompilePassIfCompilerAvailable(moveIdiomResult, "pass-by-value-and-move idiom should compile");

    const ConversionResult primitiveResult = converter.convert(
        "int increment(int value)\n"
        "{\n"
        "    return value + 1;\n"
        "}\n",
        options);
    require(contains(primitiveResult.modernCode, "int increment(int value)"),
            "primitive pass-by-value parameter should remain by value");
    requireCompilePassIfCompilerAvailable(primitiveResult, "primitive by-value function should compile");

    const std::filesystem::path root = makeTempDirectory("moderncpp_nsdmi_scope_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "class RepoInlineInitializer\n"
        "{\n"
        "public:\n"
        "    explicit RepoInlineInitializer(int configured)\n"
        "    {\n"
        "    }\n"
        "private:\n"
        "    int limit = configured;\n"
        "};\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/nsdmi-scope";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoModernized = readTextFile(root / "src" / "legacy.cpp");
    require(repoResult.filesModified == 1, "repository NSDMI fixture should be modified");
    require(!contains(repoModernized, "int limit = configured;"),
            "repository mode should use the same NSDMI scope repair");
    require(contains(repoModernized, "limit = configured;"),
            "repository mode should move constructor-dependent initialization into constructor scope");
}

void runFunctionalModernizationTests()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = enterpriseOptions();
    options.offlineModernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    options.useAuto = true;
    options.useRangeBasedFor = true;
    options.useStructuredBindings = true;
    options.compileVerificationEnabled = true;

    const ConversionResult diagnosticsResult = converter.convert(
        "#include <vector>\n"
        "int sumDiagnostics(const std::vector<int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::vector<int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        total += *it;\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(hasAnyRule(diagnosticsResult, "Functional diagnostics: FunctorToLambdaPass"),
            "functor pass should emit test-visible execution diagnostics");
    require(hasAnyRule(diagnosticsResult, "Functional diagnostics: RangeForModernizationPass"),
            "range-for pass should emit test-visible execution diagnostics");
    require(hasAnyRule(diagnosticsResult, "Functional diagnostics: IndexLoopModernizationPass"),
            "index-loop pass should emit test-visible execution diagnostics");
    require(hasAnyRule(diagnosticsResult, "Functional diagnostics: AutoDeductionPass"),
            "auto deduction pass should emit test-visible execution diagnostics");
    requireCompilePassIfCompilerAvailable(diagnosticsResult, "functional diagnostics sample should compile");

    const ConversionResult simpleFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct IsPositive\n"
        "{\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        return value > 0;\n"
        "    }\n"
        "};\n"
        "int countPositive(const std::vector<int>& values)\n"
        "{\n"
        "    return std::count_if(values.begin(), values.end(), IsPositive());\n"
        "}\n",
        options);
    require(contains(simpleFunctorResult.modernCode, "[](const auto& item)"),
            "simple predicate functor should become lambda\nOutput:\n" + simpleFunctorResult.modernCode);
    require(contains(simpleFunctorResult.modernCode, "return item > 0;"),
            "lambda should preserve predicate expression");
    require(!contains(simpleFunctorResult.modernCode, "struct IsPositive"),
            "unused converted functor should be removed");
    requireCompilePassIfCompilerAvailable(simpleFunctorResult, "simple functor-to-lambda should compile");

    const ConversionResult capturedFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct AboveThreshold\n"
        "{\n"
        "    int threshold;\n"
        "    explicit AboveThreshold(int value) : threshold(value) {}\n"
        "    bool operator()(int sample) const\n"
        "    {\n"
        "        return sample > threshold;\n"
        "    }\n"
        "};\n"
        "bool hasLarge(const std::vector<int>& values)\n"
        "{\n"
        "    return std::any_of(values.begin(), values.end(), AboveThreshold(40));\n"
        "}\n",
        options);
    require(contains(capturedFunctorResult.modernCode, "[threshold = 40](const auto& item)"),
            "constructor-initialized functor state should become lambda capture\nOutput:\n" + capturedFunctorResult.modernCode);
    require(contains(capturedFunctorResult.modernCode, "return item > threshold;"),
            "captured functor condition should be preserved");
    requireCompilePassIfCompilerAvailable(capturedFunctorResult, "captured functor-to-lambda should compile");

    const ConversionResult localFunctorResult = converter.convert(
        "#include <vector>\n"
        "struct AtLeast\n"
        "{\n"
        "    int threshold;\n"
        "    explicit AtLeast(int value) : threshold(value) {}\n"
        "    bool operator()(int sample) const\n"
        "    {\n"
        "        return sample >= threshold;\n"
        "    }\n"
        "};\n"
        "int countLocal(const std::vector<int>& values, int limit)\n"
        "{\n"
        "    AtLeast filter(limit);\n"
        "    int total = 0;\n"
        "    for (size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        if (filter(values[index]))\n"
        "        {\n"
        "            total += values[index];\n"
        "        }\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(localFunctorResult.modernCode, "auto filter = [threshold = limit](int sample)"),
            "local functor object should become lambda with constructor capture\nOutput:\n" + localFunctorResult.modernCode);
    require(contains(localFunctorResult.modernCode, "if (filter(value))"),
            "combined functor plus index loop should use range variable in predicate");
    require(contains(localFunctorResult.modernCode, "for (const auto& value : values)"),
            "combined functor plus index loop should become range-for");
    require(!contains(localFunctorResult.modernCode, "struct AtLeast"),
            "unused local functor class should be removed after conversion");
    requireCompilePassIfCompilerAvailable(localFunctorResult, "local functor plus index loop should compile");

    const ConversionResult retainedFunctorResult = converter.convert(
        "#include <vector>\n"
        "struct BoundCheck\n"
        "{\n"
        "    int threshold;\n"
        "    explicit BoundCheck(int value) : threshold(value) {}\n"
        "    bool operator()(int sample) const { return sample < threshold; }\n"
        "};\n"
        "void consume(BoundCheck predicate);\n"
        "bool anyBelow(const std::vector<int>& values, int limit)\n"
        "{\n"
        "    BoundCheck filter(limit);\n"
        "    consume(filter);\n"
        "    return filter(values[0]);\n"
        "}\n",
        options);
    require(contains(retainedFunctorResult.modernCode, "struct BoundCheck"),
            "functor class still referenced elsewhere should not be removed");
    require(contains(retainedFunctorResult.modernCode, "BoundCheck filter(limit);"),
            "functor object passed as its own type should remain unchanged");
    requireCompilePassIfCompilerAvailable(retainedFunctorResult, "retained functor should compile");

    const ConversionResult findIfResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct IsTarget\n"
        "{\n"
        "    bool operator()(int value) const { return value == 7; }\n"
        "};\n"
        "auto findTarget(std::vector<int>& values)\n"
        "{\n"
        "    return std::find_if(values.begin(), values.end(), IsTarget{});\n"
        "}\n",
        options);
    require(contains(findIfResult.modernCode, "std::find_if(values.begin(), values.end(), [](const auto& item)"),
            "find_if predicate functor should become inline lambda");
    requireCompilePassIfCompilerAvailable(findIfResult, "find_if functor-to-lambda should compile");

    const ConversionResult rangesFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct IsEnabled\n"
        "{\n"
        "    bool operator()(int value) const { return value != 0; }\n"
        "};\n"
        "int countEnabled(const std::vector<int>& values)\n"
        "{\n"
        "    return std::ranges::count_if(values, IsEnabled());\n"
        "}\n",
        options);
    require(contains(rangesFunctorResult.modernCode, "std::ranges::count_if(values, [](const auto& item)"),
            "ranges::count_if predicate functor should become inline lambda\nOutput:\n" + rangesFunctorResult.modernCode);
    requireCompilePassIfCompilerAvailable(rangesFunctorResult, "ranges functor-to-lambda should compile");

    const ConversionResult iteratorResult = converter.convert(
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
        options);
    require(contains(iteratorResult.modernCode, "for (const auto& value : values)"),
            "const iterator loop should become const range-for");
    requireCompilePassIfCompilerAvailable(iteratorResult, "iterator range-for should compile");

    const ConversionResult mutableIteratorResult = converter.convert(
        "#include <vector>\n"
        "void increment(std::vector<int>& values)\n"
        "{\n"
        "    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        *it += 1;\n"
        "    }\n"
        "}\n",
        options);
    require(contains(mutableIteratorResult.modernCode, "for (auto& value : values)"),
            "mutable iterator loop should become auto& range-for\nOutput:\n" + mutableIteratorResult.modernCode);
    requireCompilePassIfCompilerAvailable(mutableIteratorResult, "mutable iterator range-for should compile");

    const ConversionResult typenameIteratorResult = converter.convert(
        "#include <vector>\n"
        "template <typename Container>\n"
        "int total(const Container& values)\n"
        "{\n"
        "    int sum = 0;\n"
        "    for (typename Container::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        sum += *it;\n"
        "    }\n"
        "    return sum;\n"
        "}\n",
        options);
    require(contains(typenameIteratorResult.modernCode, "for (const auto& value : values)"),
            "typename const_iterator loop should become range-for");
    requireCompilePassIfCompilerAvailable(typenameIteratorResult, "typename iterator range-for should compile");

    const ConversionResult typenameMutableIteratorResult = converter.convert(
        "#include <vector>\n"
        "template <typename Container>\n"
        "void bump(Container& values)\n"
        "{\n"
        "    for (typename Container::iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        *it += 1;\n"
        "    }\n"
        "}\n",
        options);
    require(contains(typenameMutableIteratorResult.modernCode, "for (auto& value : values)"),
            "typename mutable iterator loop should become auto& range-for\nOutput:\n" + typenameMutableIteratorResult.modernCode);
    requireCompilePassIfCompilerAvailable(typenameMutableIteratorResult, "typename mutable iterator range-for should compile");

    const ConversionResult mapResult = converter.convert(
        "#include <map>\n"
        "int sumMap(const std::map<int, int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::map<int, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        total += it->first + it->second;\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(mapResult.modernCode, "for (const auto& [key, value] : values)"),
            "map iterator loop should become structured binding");
    require(contains(mapResult.modernCode, "total += key + value;"),
            "structured binding body should use key/value");
    requireCompilePassIfCompilerAvailable(mapResult, "map structured binding should compile");

    const ConversionResult unorderedMapResult = converter.convert(
        "#include <string>\n"
        "#include <unordered_map>\n"
        "int sumValues(const std::unordered_map<std::string, int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::unordered_map<std::string, int>::const_iterator it = values.begin(); it != values.end(); ++it)\n"
        "    {\n"
        "        total += (*it).second + static_cast<int>((*it).first.size());\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(unorderedMapResult.modernCode, "for (const auto& [key, value] : values)"),
            "associative iterator loop using (*it).first/second should become structured binding\nOutput:\n" + unorderedMapResult.modernCode);
    requireCompilePassIfCompilerAvailable(unorderedMapResult, "unordered_map structured binding should compile");

    const ConversionResult indexResult = converter.convert(
        "#include <vector>\n"
        "int sumIndex(const std::vector<int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        total += values[index];\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(indexResult.modernCode, "for (const auto& value : values)"),
            "index loop that only selects current element should become range-for");
    requireCompilePassIfCompilerAvailable(indexResult, "index range-for should compile");

    const ConversionResult indexDependentResult = converter.convert(
        "#include <iostream>\n"
        "#include <vector>\n"
        "void printIndexed(const std::vector<int>& values)\n"
        "{\n"
        "    for (size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        std::cout << index << ':' << values[index] << '\\n';\n"
        "    }\n"
        "}\n",
        options);
    require(contains(indexDependentResult.modernCode, "index < values.size()"),
            "index-dependent loop should remain unchanged");
    requireCompilePassIfCompilerAvailable(indexDependentResult, "index-dependent loop should compile");

    const ConversionResult reverseIteratorResult = converter.convert(
        "#include <vector>\n"
        "int reverseSum(const std::vector<int>& values)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (std::vector<int>::const_reverse_iterator it = values.rbegin(); it != values.rend(); ++it)\n"
        "    {\n"
        "        total += *it;\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(reverseIteratorResult.modernCode, "const_reverse_iterator it"),
            "reverse iterator loop should remain explicit");
    requireCompilePassIfCompilerAvailable(reverseIteratorResult, "reverse iterator loop should compile");

    const ConversionResult multiContainerResult = converter.convert(
        "#include <vector>\n"
        "int dot(const std::vector<int>& left, const std::vector<int>& right)\n"
        "{\n"
        "    int total = 0;\n"
        "    for (size_t index = 0; index < left.size(); ++index)\n"
        "    {\n"
        "        total += left[index] * right[index];\n"
        "    }\n"
        "    return total;\n"
        "}\n",
        options);
    require(contains(multiContainerResult.modernCode, "right[index]"),
            "multi-container index loop should remain unchanged");
    requireCompilePassIfCompilerAvailable(multiContainerResult, "multi-container loop should compile");

    const ConversionResult sideEffectFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct CountingPredicate\n"
        "{\n"
        "    mutable int calls = 0;\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        ++calls;\n"
        "        return value > 0;\n"
        "    }\n"
        "};\n"
        "int count(const std::vector<int>& values)\n"
        "{\n"
        "    return std::count_if(values.begin(), values.end(), CountingPredicate());\n"
        "}\n",
        options);
    require(contains(sideEffectFunctorResult.modernCode, "struct CountingPredicate"),
            "functor with side effects should remain unchanged");
    requireCompilePassIfCompilerAvailable(sideEffectFunctorResult, "side-effect functor should compile");

    const ConversionResult polymorphicFunctorResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct PredicateBase { virtual ~PredicateBase() = default; };\n"
        "struct DerivedPredicate : PredicateBase\n"
        "{\n"
        "    bool operator()(int value) const { return value > 0; }\n"
        "};\n"
        "int count(const std::vector<int>& values)\n"
        "{\n"
        "    return std::count_if(values.begin(), values.end(), DerivedPredicate());\n"
        "}\n",
        options);
    require(contains(polymorphicFunctorResult.modernCode, "struct DerivedPredicate"),
            "polymorphic functor should not be converted automatically");
    requireCompilePassIfCompilerAvailable(polymorphicFunctorResult, "polymorphic functor should compile");

    const ConversionResult autoIteratorResult = converter.convert(
        "#include <vector>\n"
        "void useIterator(std::vector<int>& values)\n"
        "{\n"
        "    std::vector<int>::iterator it = values.begin();\n"
        "    if (it != values.end())\n"
        "    {\n"
        "        *it = 1;\n"
        "    }\n"
        "}\n",
        options);
    require(contains(autoIteratorResult.modernCode, "auto it = values.begin();"),
            "standalone verbose iterator declaration should use auto");
    requireCompilePassIfCompilerAvailable(autoIteratorResult, "auto iterator deduction should compile");

    const std::filesystem::path root = makeTempDirectory("moderncpp_functional_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "#include <vector>\n"
        "struct Positive\n"
        "{\n"
        "    bool operator()(int value) const { return value > 0; }\n"
        "};\n"
        "int sum(const std::vector<int>& values)\n"
        "{\n"
        "    Positive filter;\n"
        "    int total = 0;\n"
        "    for (size_t index = 0; index < values.size(); ++index)\n"
        "    {\n"
        "        if (filter(values[index]))\n"
        "        {\n"
        "            total += values[index];\n"
        "        }\n"
        "    }\n"
        "    return total;\n"
        "}\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/functional";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoModernized = readTextFile(root / "src" / "legacy.cpp");
    require(repoResult.filesModified == 1, "repository functional fixture should be modified");
    require((contains(repoModernized, "auto filter = [](int value)") || contains(repoModernized, "auto filter = [](const auto& item)"))
                && contains(repoModernized, "for (const auto& value : values)")
                && contains(repoModernized, "if (filter(value))"),
            "repository mode should run the same functional modernization pipeline\nOutput:\n" + repoModernized);
}

void runFunctorToLambdaSpecificTests()
{
    const RuleBasedConverterEngine converter;
    ModernizationOptions options = enterpriseOptions();
    options.offlineModernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    options.useLambdas = true;
    options.compileVerificationEnabled = true;

    const ConversionResult directCallResult = converter.convert(
        "struct ThresholdPredicate\n"
        "{\n"
        "    explicit ThresholdPredicate(int configured) : threshold(configured) {}\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        return value > threshold;\n"
        "    }\n"
        "private:\n"
        "    int threshold;\n"
        "};\n"
        "int inspect(int value, int limit)\n"
        "{\n"
        "    ThresholdPredicate filter(limit);\n"
        "    if (filter(value))\n"
        "    {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        options);
    require(contains(directCallResult.modernCode, "auto filter = [threshold = limit](int value)"),
            "local predicate functor object used in if should become lambda\nOutput:\n" + directCallResult.modernCode);
    require(contains(directCallResult.modernCode, "return value > threshold;"),
            "lambda should preserve the predicate return expression");
    require(!contains(directCallResult.modernCode, "struct ThresholdPredicate"),
            "unused functor class should be removed after local object conversion");
    requireCompilePassIfCompilerAvailable(directCallResult, "direct-call functor-to-lambda should compile");

    const ConversionResult localAlgorithmVariableResult = converter.convert(
        "#include <algorithm>\n"
        "#include <vector>\n"
        "struct AlgorithmPredicate\n"
        "{\n"
        "    explicit AlgorithmPredicate(int configured) : threshold(configured) {}\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        return value >= threshold;\n"
        "    }\n"
        "private:\n"
        "    int threshold;\n"
        "};\n"
        "int countMatches(const std::vector<int>& values, int limit)\n"
        "{\n"
        "    AlgorithmPredicate filter(limit);\n"
        "    return static_cast<int>(std::count_if(values.begin(), values.end(), filter));\n"
        "}\n",
        options);
    require(contains(localAlgorithmVariableResult.modernCode, "auto filter = [threshold = limit](int value)"),
            "local functor object passed as an algorithm predicate variable should become lambda\nOutput:\n" + localAlgorithmVariableResult.modernCode);
    require(!contains(localAlgorithmVariableResult.modernCode, "struct AlgorithmPredicate"),
            "unused algorithm predicate functor should be removed");
    requireCompilePassIfCompilerAvailable(localAlgorithmVariableResult, "local algorithm predicate variable should compile");

    const ConversionResult thisMemberResult = converter.convert(
        "class LessThanPredicate\n"
        "{\n"
        "public:\n"
        "    explicit LessThanPredicate(int configured) : limit(configured) {}\n"
        "    bool operator()(int sample) const\n"
        "    {\n"
        "        return sample < this->limit;\n"
        "    }\n"
        "private:\n"
        "    int limit;\n"
        "};\n"
        "bool check(int sample, int maximum)\n"
        "{\n"
        "    LessThanPredicate filter{maximum};\n"
        "    return filter(sample);\n"
        "}\n",
        options);
    require(contains(thisMemberResult.modernCode, "auto filter = [limit = maximum](int sample)"),
            "this->member predicate capture should use a lambda init-capture\nOutput:\n" + thisMemberResult.modernCode);
    require(contains(thisMemberResult.modernCode, "return sample < limit;"),
            "this->member reference should be rewritten to the lambda capture");
    require(!contains(thisMemberResult.modernCode, "this->limit"),
            "converted lambda should not keep functor this-> member access");
    requireCompilePassIfCompilerAvailable(thisMemberResult, "this-member functor-to-lambda should compile");

    const ConversionResult prefixedMemberResult = converter.convert(
        "struct PrefixPredicate\n"
        "{\n"
        "    explicit PrefixPredicate(int configured) : m_threshold(configured) {}\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        return value != m_threshold;\n"
        "    }\n"
        "private:\n"
        "    int m_threshold;\n"
        "};\n"
        "bool differs(int value, int threshold)\n"
        "{\n"
        "    auto filter = PrefixPredicate(threshold);\n"
        "    return filter(value);\n"
        "}\n",
        options);
    require(contains(prefixedMemberResult.modernCode, "auto filter = [threshold = threshold](int value)"),
            "m_ member predicate should use a clean capture alias\nOutput:\n" + prefixedMemberResult.modernCode);
    require(contains(prefixedMemberResult.modernCode, "return value != threshold;"),
            "m_ member reference should be rewritten to the clean lambda capture name");
    require(!contains(prefixedMemberResult.modernCode, "m_threshold"),
            "unused m_ functor field should disappear after class removal");
    requireCompilePassIfCompilerAvailable(prefixedMemberResult, "m-member functor-to-lambda should compile");

    const ConversionResult keptFunctorResult = converter.convert(
        "struct ReusablePredicate\n"
        "{\n"
        "    explicit ReusablePredicate(int configured) : limit(configured) {}\n"
        "    bool operator()(int value) const { return value <= limit; }\n"
        "private:\n"
        "    int limit;\n"
        "};\n"
        "void consume(ReusablePredicate predicate);\n"
        "bool checkReusable(int value, int limit)\n"
        "{\n"
        "    ReusablePredicate filter(limit);\n"
        "    consume(filter);\n"
        "    return filter(value);\n"
        "}\n",
        options);
    require(contains(keptFunctorResult.modernCode, "struct ReusablePredicate"),
            "functor class should stay when the functor object is passed as its own type");
    require(contains(keptFunctorResult.modernCode, "ReusablePredicate filter(limit);"),
            "unsafe reusable functor object should remain unchanged");
    requireCompilePassIfCompilerAvailable(keptFunctorResult, "kept functor sample should compile");

    const ConversionResult overloadedFunctorResult = converter.convert(
        "struct OverloadedPredicate\n"
        "{\n"
        "    bool operator()(int value) const { return value > 0; }\n"
        "    bool operator()(double value) const { return value > 0.0; }\n"
        "};\n"
        "bool check(int value)\n"
        "{\n"
        "    OverloadedPredicate filter;\n"
        "    return filter(value);\n"
        "}\n",
        options);
    require(contains(overloadedFunctorResult.modernCode, "struct OverloadedPredicate"),
            "functor with overloaded call operators should not be converted");
    requireCompilePassIfCompilerAvailable(overloadedFunctorResult, "overloaded functor should remain compilable");

    const ConversionResult sideEffectFunctorResult = converter.convert(
        "struct SideEffectPredicate\n"
        "{\n"
        "    mutable int calls = 0;\n"
        "    bool operator()(int value) const\n"
        "    {\n"
        "        ++calls;\n"
        "        return value > 0;\n"
        "    }\n"
        "};\n"
        "bool check(int value)\n"
        "{\n"
        "    SideEffectPredicate filter;\n"
        "    return filter(value);\n"
        "}\n",
        options);
    require(contains(sideEffectFunctorResult.modernCode, "struct SideEffectPredicate"),
            "mutable/side-effect functor should not be converted");
    requireCompilePassIfCompilerAvailable(sideEffectFunctorResult, "side-effect functor should remain compilable");

    const std::filesystem::path root = makeTempDirectory("moderncpp_functor_specific_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "struct RepoPredicate\n"
        "{\n"
        "    explicit RepoPredicate(int configured) : limit(configured) {}\n"
        "    bool operator()(int value) const { return value >= limit; }\n"
        "private:\n"
        "    int limit;\n"
        "};\n"
        "bool run(int value, int limit)\n"
        "{\n"
        "    RepoPredicate filter(limit);\n"
        "    return filter(value);\n"
        "}\n");

    RepositoryModernizationOptions repoOptions;
    repoOptions.repositoryUrl = "https://github.com/example/functor-specific";
    repoOptions.outputWorkspaceFolder = root.parent_path();
    repoOptions.modernizationLevel = OfflineModernizationLevel::AggressiveSafe;
    repoOptions.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult repoResult = service.modernizeRepository(repoOptions, root);
    const std::string repoModernized = readTextFile(root / "src" / "legacy.cpp");
    require(repoResult.filesModified == 1, "repository functor fixture should be modified");
    require(contains(repoModernized, "auto filter = [limit = limit](int value)")
                && contains(repoModernized, "return value >= limit;"),
            "repository mode should use the same direct-call functor-to-lambda pass\nOutput:\n" + repoModernized);
}

void runRepositoryModeTests()
{
    const std::filesystem::path root = makeTempDirectory("moderncpp_phase4_repo");
    writeTextFile(root / "src" / "legacy.cpp",
        "#include <vector>\n"
        "#include <iostream>\n"
        "struct BaseItem\n"
        "{\n"
        "    virtual void run();\n"
        "};\n"
        "struct DerivedItem : public BaseItem\n"
        "{\n"
        "    virtual ~DerivedItem()\n"
        "    {\n"
        "        std::cout << \"cleanup\\n\";\n"
        "    }\n"
        "    void run();\n"
        "};\n"
        "struct Element {};\n"
        "void observe(Element* item);\n"
        "void build()\n"
        "{\n"
        "    std::vector<std::unique_ptr<Element>> elements;\n"
        "    elements.push_back(new Element());\n"
        "    observe(elements[0]);\n"
        "}\n");

    RepositoryModernizationOptions options;
    options.repositoryUrl = "https://github.com/example/phase4";
    options.outputWorkspaceFolder = root.parent_path();
    options.modernizationLevel = OfflineModernizationLevel::Balanced;
    options.compileVerificationEnabled = false;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, root);
    const std::string modernized = readTextFile(root / "src" / "legacy.cpp");

    require(result.filesScanned == 1, "repository suite should scan one source file");
    require(result.filesModified == 1, "repository suite should modify the source file");
    require(contains(modernized, "elements.push_back(std::make_unique<Element>());"),
            "repository mode should use smart pointer propagation");
    require(contains(modernized, "observe(elements[0].get());"),
            "repository mode should propagate observer calls");
    require(contains(modernized, "virtual ~BaseItem() = default;"),
            "repository mode should add validated virtual base destructor");
    require(contains(modernized, "~DerivedItem() override"),
            "repository mode should validate derived destructor override");

    writeTextFile(root / "src" / "enum_output.cpp",
        "#include <iostream>\n"
        "enum Status { Ready, Failed };\n"
        "Status current() { return Ready; }\n"
        "void report()\n"
        "{\n"
        "    std::cout << current() << '\\n';\n"
        "}\n");

    RepositoryModernizationResult enumResult = service.modernizeRepository(options, root);
    const std::string enumModernized = readTextFile(root / "src" / "enum_output.cpp");
    require(enumResult.filesScanned >= 2, "repository suite should scan the added enum output source file");
    require(contains(enumModernized, "enum class Status"), "repository mode should run enum class modernization");
    require(contains(enumModernized, "static_cast<std::underlying_type_t<Status>>(current())"),
            "repository mode should run scoped enum output propagation");
    require(!contains(enumModernized, "->static_cast") && !contains(enumModernized, ".static_cast"),
            "repository mode should not produce invalid member-access scoped enum casts");
}
} // namespace

int main(int argc, char** argv)
{
    const std::string suite = argc > 1 ? argv[1] : "all";

    if (suite == "ownership" || suite == "all") {
        runOwnershipTests();
    }
    if (suite == "polymorphism" || suite == "all") {
        runPolymorphismTests();
    }
    if (suite == "rule-zero" || suite == "all") {
        runRuleOfZeroTests();
    }
    if (suite == "algorithm" || suite == "all") {
        runAlgorithmModernizationTests();
    }
    if (suite == "iterator" || suite == "all") {
        runIteratorModernizationTests();
    }
    if (suite == "file-io" || suite == "all") {
        runFileIoModernizationTests();
    }
    if (suite == "semantic" || suite == "all") {
        runSemanticConsistencyTests();
    }
    if (suite == "scoped-enum" || suite == "all") {
        runScopedEnumOutputTests();
    }
    if (suite == "polish" || suite == "all") {
        runFinalPolishTests();
    }
    if (suite == "regression" || suite == "all") {
        runRegressionModernizationTests();
    }
    if (suite == "quality" || suite == "all") {
        runQualitySprintTests();
    }
    if (suite == "cross-scope" || suite == "all") {
        runCrossScopeTypePropagationTests();
    }
    if (suite == "nsdmi-scope" || suite == "all") {
        runNsdmiScopeSafetyTests();
    }
    if (suite == "functional" || suite == "all") {
        runFunctionalModernizationTests();
    }
    if (suite == "functor-specific" || suite == "all") {
        runFunctorToLambdaSpecificTests();
    }
    if (suite == "repository" || suite == "all") {
        runRepositoryModeTests();
    }

    std::cout << "Phase 4 " << suite << " tests passed.\n";
    return EXIT_SUCCESS;
}
