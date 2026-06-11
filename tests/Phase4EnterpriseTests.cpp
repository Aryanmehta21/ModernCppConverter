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
            "generated destructor name should exactly match enclosing class name");
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
    if (suite == "repository" || suite == "all") {
        runRepositoryModeTests();
    }

    std::cout << "Phase 4 " << suite << " tests passed.\n";
    return EXIT_SUCCESS;
}
