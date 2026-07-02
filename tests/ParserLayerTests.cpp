#include "converter/RawTextRepresentation.h"
#include "converter/RewriteCoordinator.h"
#include "frontend/FrontendFactory.h"
#include "frontend/LightweightFrontend.h"
#include "parser/LightweightCppParser.h"

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

std::string slice(const std::string& source, const SourceRange& range)
{
    return source.substr(range.start.offset, range.length());
}

const ParsedAggregate* findAggregate(const ParsedDocument& document, const std::string& name)
{
    for (const ParsedAggregate& aggregate : document.aggregates) {
        if (aggregate.name == name) {
            return &aggregate;
        }
    }
    return nullptr;
}

const ParsedFunction* findFunction(const ParsedDocument& document, const std::string& name)
{
    for (const ParsedFunction& function : document.functions) {
        if (function.name == name) {
            return &function;
        }
    }
    return nullptr;
}

const ParsedFunction* findFunction(const ParsedDocument& document,
                                   const std::string& name,
                                   const std::string& parentName)
{
    for (const ParsedFunction& function : document.functions) {
        if (function.name == name && function.parentName == parentName) {
            return &function;
        }
    }
    return nullptr;
}

const ParsedEnum* findEnum(const ParsedDocument& document, const std::string& name)
{
    for (const ParsedEnum& parsedEnum : document.enums) {
        if (parsedEnum.name == name) {
            return &parsedEnum;
        }
    }
    return nullptr;
}

const ParsedVariable* findMember(const ParsedDocument& document, const std::string& name)
{
    for (const ParsedVariable& variable : document.memberVariables) {
        if (variable.name == name) {
            return &variable;
        }
    }
    return nullptr;
}

const ParsedVariable* findLocal(const ParsedDocument& document, const std::string& name)
{
    for (const ParsedVariable& variable : document.localVariables) {
        if (variable.name == name) {
            return &variable;
        }
    }
    return nullptr;
}

bool hasScope(const ParsedDocument& document, ScopeKind kind, const std::string& name)
{
    for (const ScopeInfo& scope : document.scopes) {
        if (scope.kind == kind && scope.name == name) {
            return true;
        }
    }
    return false;
}

const ParsedSymbol* findSymbol(const ParsedDocument& document, ParsedSymbolKind kind, const std::string& name)
{
    for (const ParsedSymbol& symbol : document.symbols) {
        if (symbol.kind == kind && symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const ParsedSymbol* findChildSymbol(const ParsedDocument& document,
                                    ParsedSymbolId parentId,
                                    ParsedSymbolKind kind,
                                    const std::string& name)
{
    for (const ParsedSymbol& symbol : document.symbols) {
        if (symbol.kind == kind && symbol.name == name && symbol.parentId == parentId) {
            return &symbol;
        }
    }
    return nullptr;
}

std::size_t countChildSymbols(const ParsedDocument& document,
                              ParsedSymbolId parentId,
                              ParsedSymbolKind kind,
                              const std::string& name)
{
    std::size_t count = 0;
    for (const ParsedSymbol& symbol : document.symbols) {
        if (symbol.kind == kind && symbol.name == name && symbol.parentId == parentId) {
            ++count;
        }
    }
    return count;
}

bool hasResolvedReferenceTo(const ParsedDocument& document, ParsedSymbolId symbolId)
{
    for (const ParsedSymbolReference& reference : document.symbolReferences) {
        if (reference.resolved && reference.symbolId == symbolId) {
            return true;
        }
    }
    return false;
}

bool hasResolvedReferenceToChildNamed(const ParsedDocument& document,
                                      ParsedSymbolId parentId,
                                      ParsedSymbolKind kind,
                                      const std::string& name)
{
    for (const ParsedSymbol& symbol : document.symbols) {
        if (symbol.parentId == parentId && symbol.kind == kind && symbol.name == name
            && hasResolvedReferenceTo(document, symbol.id)) {
            return true;
        }
    }
    return false;
}

bool diagnosticsContain(const std::vector<std::string>& diagnostics, const std::string& needle)
{
    for (const std::string& diagnostic : diagnostics) {
        if (diagnostic.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

SourceRange testRange(std::size_t start, std::size_t end, SourceEntityKind kind = SourceEntityKind::Expression)
{
    SourceRange range;
    range.start.offset = start;
    range.start.line = 1;
    range.start.column = start + 1;
    range.end.offset = end;
    range.end.line = 1;
    range.end.column = end + 1;
    range.entityKind = kind;
    return range;
}

RewriteEdit testEdit(std::size_t start,
                     std::size_t end,
                     std::string replacement,
                     std::string symbol = {})
{
    RewriteEdit edit;
    edit.range = testRange(start, end);
    edit.replacementText = std::move(replacement);
    edit.passName = "ParserLayerTestPass";
    edit.reason = "test edit";
    edit.affectedSymbol = std::move(symbol);
    return edit;
}

void testTokenizationAndDirectives()
{
    const std::string source =
        "#include <vector>\n"
        "#define LIMIT 4\n"
        "int main() { return LIMIT; }\n";

    const ParsedDocument document = LightweightCppParser{}.parse(source);
    require(document.parseSucceeded, "basic source should parse successfully");
    require(document.originalSource == source, "parser should preserve original source");
    require(document.includes.size() == 1, "include directive should be detected");
    require(document.includes.front().path == "<vector>", "include path should be captured");
    require(document.macros.size() == 1, "macro directive should be detected");
    require(document.macros.front().name == "LIMIT", "macro name should be captured");

    bool sawMain = false;
    for (const CppToken& token : document.tokens) {
        if (token.text == "main") {
            sawMain = true;
            require(token.range.start.line == 3, "token line should be tracked");
            require(token.range.start.column == 5, "token column should be tracked");
            require(token.range.entityKind == SourceEntityKind::Token, "token range should report token entity kind");
            require(token.range.entityName == "main", "token range should retain token text as entity name");
        }
    }
    require(sawMain, "identifier tokens should be available");
    require(document.includes.front().range.entityKind == SourceEntityKind::Include,
            "include ranges should be annotated as include entities");
    require(document.macros.front().range.entityKind == SourceEntityKind::Macro,
            "macro ranges should be annotated as macro entities");
}

void testClassFunctionMemberAndLocalDetection()
{
    const std::string source =
        "class Widget {\n"
        "public:\n"
        "    int count;\n"
        "    void reset(int value) const {\n"
        "        int local = value;\n"
        "        helper(local);\n"
        "    }\n"
        "};\n";

    const ParsedDocument document = LightweightCppParser{}.parse(source);
    const ParsedAggregate* aggregate = findAggregate(document, "Widget");
    require(aggregate != nullptr, "class should be detected");
    require(aggregate->kind == ParsedAggregateKind::Class, "class kind should be recorded");
    require(slice(source, aggregate->range).find("class Widget") == 0, "class source range should start at declaration");
    require(aggregate->range.entityKind == SourceEntityKind::Class, "class range should carry class entity kind");
    require(aggregate->range.entityName == "Widget", "class range should carry class name");
    require(aggregate->range.parentScopeId.has_value(), "class range should record parent scope id");

    const ParsedFunction* reset = findFunction(document, "reset");
    require(reset != nullptr, "member function should be detected");
    require(reset->isMember, "member function should be marked as member");
    require(reset->parentName == "Widget", "member function should know parent class");
    require(reset->isConst, "const member function qualifier should be tracked");
    require(reset->parameters.size() == 1, "function parameter should be detected");
    require(reset->parameters.front().name == "value", "parameter name should be captured");
    require(reset->parameters.front().type == "int", "parameter type should be captured");
    require(reset->range.entityKind == SourceEntityKind::Function, "function range should carry function entity kind");

    const ParsedVariable* count = findMember(document, "count");
    require(count != nullptr, "member variable should be detected");
    require(count->type == "int", "member variable type should be captured");
    require(count->parentName == "Widget", "member variable parent should be captured");
    require(count->range.entityKind == SourceEntityKind::Member, "member range should carry member entity kind");

    const ParsedVariable* local = findLocal(document, "local");
    require(local != nullptr, "local variable should be detected");
    require(local->parentName == "reset", "local variable parent function should be captured");
    require(local->range.entityKind == SourceEntityKind::Local, "local range should carry local entity kind");

    bool sawHelperCall = false;
    for (const ParsedCallExpression& call : document.callExpressions) {
        if (call.callee == "helper" && call.parentFunction == "reset") {
            sawHelperCall = true;
        }
    }
    require(sawHelperCall, "simple call expression should be captured");
}

void testStructEnumAndSourceRanges()
{
    const std::string source =
        "struct Derived : public Base {\n"
        "    double reading;\n"
        "};\n"
        "enum class State : unsigned int {\n"
        "    Ready,\n"
        "    Failed = 2\n"
        "};\n";

    const ParsedDocument document = LightweightCppParser{}.parse(source);
    const ParsedAggregate* aggregate = findAggregate(document, "Derived");
    require(aggregate != nullptr, "struct should be detected");
    require(aggregate->kind == ParsedAggregateKind::Struct, "struct kind should be recorded");
    require(!aggregate->baseNames.empty() && aggregate->baseNames.front() == "Base", "base class should be detected");
    require(slice(source, aggregate->range).find("struct Derived") == 0, "struct source range should be correct");

    require(document.enums.size() == 1, "enum declaration should be detected");
    const ParsedEnum& state = document.enums.front();
    require(state.name == "State", "enum name should be captured");
    require(state.scoped, "enum class should be marked scoped");
    require(state.underlyingType == "unsigned int", "enum underlying type should be captured");
    require(state.enumerators.size() == 2, "enum enumerators should be captured");
    require(state.enumerators.front() == "Ready", "first enumerator should be captured");
    require(slice(source, state.range).find("enum class State") == 0, "enum source range should be correct");
}

void testFunctionScopeAndLocalVariableDetection()
{
    const std::string source =
        "int add(int lhs, int rhs) {\n"
        "    int result = lhs + rhs;\n"
        "    return result;\n"
        "}\n";

    const ParsedDocument document = LightweightCppParser{}.parse(source);
    const ParsedFunction* add = findFunction(document, "add");
    require(add != nullptr, "free function should be detected");
    require(!add->isMember, "free function should not be marked member");
    require(add->returnType == "int", "function return type should be captured");
    require(add->parameters.size() == 2, "function parameters should be captured");
    require(add->bodyRange.length() > 0, "function body range should be captured");

    bool sawFunctionScope = false;
    for (const ScopeInfo& scope : document.scopes) {
        if (scope.kind == ScopeKind::Function && scope.name == "add") {
            sawFunctionScope = true;
        }
    }
    require(sawFunctionScope, "function scope should be recorded");
    require(findLocal(document, "result") != nullptr, "function local variable should be detected");
}

void testRepresentationParserBacking()
{
    RawTextRepresentation representation("int value;\n");
    const ParsedDocument* firstDocument = representation.parsedDocument();
    require(firstDocument != nullptr, "CodeRepresentation should expose parser metadata");
    require(!firstDocument->tokens.empty(), "parser metadata should include tokens");

    representation.replaceSourceText("class Item { int id; };\n");
    const ParsedDocument* secondDocument = representation.parsedDocument();
    require(secondDocument != nullptr, "parser metadata should refresh after source replacement");
    require(findAggregate(*secondDocument, "Item") != nullptr, "refreshed parser metadata should reflect new source");
}

void testFrontendInterfaceDefaultsToLightweight()
{
    const std::string source =
        "enum class Mode { One };\n"
        "class Widget { int value; };\n"
        "int run() { return 0; }\n";

    const std::unique_ptr<IModernizationFrontend> frontend = createDefaultModernizationFrontend();
    require(frontend != nullptr, "default modernization frontend should be available");
    require(frontend->kind() == ModernizationFrontendKind::Lightweight, "default frontend should remain lightweight");
    require(frontend->name() == "LightweightFrontend", "default frontend name should be stable");
    require(!frontend->isExperimental(), "default frontend must not be experimental");

    const ModernizationFrontendResult result = frontend->analyze(source);
    require(result.parseSucceeded, "lightweight frontend should parse simple source");
    require(result.entityCounts.classes == 1, "lightweight frontend should report class count");
    require(result.entityCounts.functions == 1, "lightweight frontend should report function count");
    require(result.entityCounts.enums == 1, "lightweight frontend should report enum count");
    require(diagnosticsContain(result.diagnostics, "FRONTEND used=LightweightFrontend"),
            "frontend diagnostics should report the frontend used");
    require(diagnosticsContain(result.diagnostics, clangExperimentsEnabled() ? "clang_experiment=enabled" : "clang_experiment=disabled"),
            "default frontend diagnostics should report the compiled Clang experiment state");
}

void testOptionalClangFrontendFactory()
{
    const std::string source =
        "enum Color { Red };\n"
        "struct Sample { int value; };\n"
        "int compute() { return 1; }\n";

    const std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend();
    if (!clangExperimentsEnabled()) {
        require(clangFrontend == nullptr, "Clang frontend factory should return null when experiments are disabled");
        return;
    }

    require(clangFrontend != nullptr, "Clang frontend should be available when experiments are enabled");
    require(clangFrontend->kind() == ModernizationFrontendKind::ClangExperimental,
            "Clang experiment should report its frontend kind");
    require(clangFrontend->isExperimental(), "Clang frontend should be marked experimental");

    const ModernizationFrontendResult result = clangFrontend->analyze(source);
    require(diagnosticsContain(result.diagnostics, "FRONTEND used=ClangExperimentalFrontend"),
            "Clang frontend diagnostics should report the frontend used");
    require(result.parseSucceeded, "Clang experimental frontend should parse simple source when enabled");
    require(result.entityCounts.classes >= 1, "Clang experimental frontend should detect a simple aggregate");
    require(result.entityCounts.functions >= 1, "Clang experimental frontend should detect a simple function");
    require(result.entityCounts.enums >= 1, "Clang experimental frontend should detect a simple enum");
}

void testClangFrontendReadOnlyEntityPopulation()
{
    const std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend();
    if (!clangExperimentsEnabled()) {
        require(clangFrontend == nullptr, "Clang frontend should stay unavailable in the default build");
        return;
    }

    const std::filesystem::path headerPath =
        std::filesystem::temp_directory_path() / "moderncppconverter_clang_frontend_empty.h";
    {
        std::ofstream header(headerPath);
        header << "// intentionally empty parser test header\n";
    }

    const std::string source =
        "#include \"" + headerPath.generic_string() + "\"\n"
        "#define LIMIT 4\n"
        "namespace demo {\n"
        "enum class Mode : unsigned short { Ready, Failed };\n"
        "struct Widget {\n"
        "    int value;\n"
        "    Widget(int initial) : value(initial) {}\n"
        "    ~Widget() {}\n"
        "    int adjust(int delta) const {\n"
        "        int local = value + delta;\n"
        "        return local;\n"
        "    }\n"
        "};\n"
        "int run() {\n"
        "    Widget widget(LIMIT);\n"
        "    return widget.adjust(2);\n"
        "}\n"
        "}\n";

    const ModernizationFrontendResult result = clangFrontend->analyze(source);
    require(result.parseSucceeded, "Clang frontend should parse the entity population sample");
    require(diagnosticsContain(result.diagnostics, "clang_parse=success"), "Clang parse success should be reported");
    require(diagnosticsContain(result.diagnostics, "FRONTEND_COMPARE"),
            "Clang frontend should compare its entity counts with the lightweight frontend");

    const ParsedDocument& document = result.document;
    require(document.originalSource == source, "Clang document should preserve original source");
    require(!document.tokens.empty(), "Clang frontend should preserve token metadata from the lightweight tokenizer");
    require(!document.includes.empty() && document.includes.front().path.find("moderncppconverter_clang_frontend_empty.h") != std::string::npos,
            "Clang frontend should collect include directives");
    require(!document.macros.empty() && document.macros.front().name == "LIMIT",
            "Clang frontend should collect macro directives");
    require(hasScope(document, ScopeKind::Namespace, "demo"), "Clang frontend should collect namespace scopes");

    const ParsedAggregate* widget = findAggregate(document, "Widget");
    require(widget != nullptr, "Clang frontend should collect structs/classes");
    require(widget->kind == ParsedAggregateKind::Struct, "Clang frontend should preserve aggregate kind");
    require(slice(source, widget->range).find("struct Widget") == 0,
            "Clang aggregate source range should start at the declaration");
    require(widget->range.entityName == "Widget", "Clang aggregate range should retain the entity name");

    const ParsedEnum* mode = findEnum(document, "Mode");
    require(mode != nullptr, "Clang frontend should collect enum declarations");
    require(mode->scoped, "Clang frontend should preserve scoped enum information");
    require(mode->underlyingType.find("unsigned short") != std::string::npos,
            "Clang frontend should preserve enum underlying type");
    require(mode->enumerators.size() == 2 && mode->enumerators.front() == "Ready",
            "Clang frontend should collect enum constants");

    const ParsedFunction* constructor = findFunction(document, "Widget", "Widget");
    require(constructor != nullptr, "Clang frontend should collect constructors");
    require(constructor->parameters.size() == 1, "Clang constructor parameters should be collected");
    require(constructor->parameters.front().canonicalType.find("int") != std::string::npos,
            "Clang constructor parameter canonical type should be available");

    const ParsedFunction* destructor = findFunction(document, "~Widget", "Widget");
    require(destructor != nullptr, "Clang frontend should collect destructors");

    const ParsedFunction* adjust = findFunction(document, "adjust", "Widget");
    require(adjust != nullptr, "Clang frontend should collect methods");
    require(adjust->isMember && adjust->isConst, "Clang frontend should preserve method constness");
    require(adjust->returnType.find("int") != std::string::npos, "Clang method return type should be available");
    require(adjust->canonicalReturnType.find("int") != std::string::npos,
            "Clang method canonical return type should be available");

    const ParsedVariable* value = findMember(document, "value");
    require(value != nullptr, "Clang frontend should collect member variables");
    require(value->canonicalType.find("int") != std::string::npos,
            "Clang member canonical type should be available");

    const ParsedVariable* local = findLocal(document, "local");
    require(local != nullptr, "Clang frontend should collect local variables where practical");
    require(local->parentName == "adjust", "Clang local variable should record the parent function");
    require(local->range.isValidFor(source.size()), "Clang local source range should be valid");
}

void testClangFrontendSymbolResolutionGraph()
{
    const std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend();
    if (!clangExperimentsEnabled()) {
        require(clangFrontend == nullptr, "Clang symbol graph test should stay disabled in default builds");
        return;
    }

    const std::string source =
        "namespace outer { namespace inner {\n"
        "class Forward;\n"
        "typedef int Count;\n"
        "using Speed = Count;\n"
        "enum class State : unsigned char { Idle, Running };\n"
        "int globalRpm = 0;\n"
        "template <class T>\n"
        "struct Box { T value; };\n"
        "class Engine {\n"
        "public:\n"
        "    class Controller { public: int nestedValue; };\n"
        "    Engine();\n"
        "    ~Engine();\n"
        "    void start(Count targetRpm);\n"
        "    int compute(int value);\n"
        "    int compute(double value);\n"
        "private:\n"
        "    State state;\n"
        "    Count rpm;\n"
        "};\n"
        "Engine::Engine() : state(State::Idle), rpm(0) {}\n"
        "Engine::~Engine() {}\n"
        "void Engine::start(Count targetRpm) {\n"
        "    State localState = State::Running;\n"
        "    Count localRpm = targetRpm;\n"
        "    rpm = localRpm;\n"
        "    globalRpm = rpm;\n"
        "}\n"
        "int Engine::compute(int value) { return value; }\n"
        "int Engine::compute(double value) { return static_cast<int>(value); }\n"
        "}}\n";

    const ModernizationFrontendResult result = clangFrontend->analyze(source);
    require(result.parseSucceeded, "Clang symbol graph sample should parse");
    require(diagnosticsContain(result.diagnostics, "FRONTEND_SYMBOLS total="),
            "Clang diagnostics should include symbol-resolution totals");
    require(diagnosticsContain(result.diagnostics, "unresolved=0"),
            "Clang diagnostics should report no unresolved references in the valid sample");

    const ParsedDocument& document = result.document;
    require(!document.symbols.empty(), "Clang frontend should populate symbols");
    require(!document.symbolReferences.empty(), "Clang frontend should populate symbol references");

    const ParsedSymbol* outer = findSymbol(document, ParsedSymbolKind::Namespace, "outer");
    require(outer != nullptr, "outer namespace should have a symbol");
    const ParsedSymbol* inner = findChildSymbol(document, outer->id, ParsedSymbolKind::Namespace, "inner");
    require(inner != nullptr, "inner namespace should be parented to outer");

    const ParsedSymbol* forward = findChildSymbol(document, inner->id, ParsedSymbolKind::Class, "Forward");
    require(forward != nullptr, "forward declaration should have a class symbol");
    require(!forward->isDefinition, "forward-only class symbol should be marked declaration-only");

    const ParsedSymbol* count = findChildSymbol(document, inner->id, ParsedSymbolKind::Typedef, "Count");
    require(count != nullptr, "typedef should have a symbol");
    require(count->canonicalType.find("int") != std::string::npos, "typedef canonical type should be recorded");
    const ParsedSymbol* speed = findChildSymbol(document, inner->id, ParsedSymbolKind::Alias, "Speed");
    require(speed != nullptr, "using alias should have a symbol");
    require(speed->canonicalType.find("int") != std::string::npos, "alias canonical type should resolve through typedef");

    const ParsedSymbol* state = findChildSymbol(document, inner->id, ParsedSymbolKind::Enum, "State");
    require(state != nullptr && state->isDefinition, "enum class should have a definition symbol");
    const ParsedSymbol* running = findChildSymbol(document, state->id, ParsedSymbolKind::EnumConstant, "Running");
    require(running != nullptr, "enum constants should be parented to the enum");

    const ParsedSymbol* globalRpm = findChildSymbol(document, inner->id, ParsedSymbolKind::GlobalVariable, "globalRpm");
    require(globalRpm != nullptr, "global variables should have symbols");
    require(globalRpm->range.isValidFor(source.size()), "global variable source range should be valid");

    const ParsedSymbol* box = findChildSymbol(document, inner->id, ParsedSymbolKind::Struct, "Box");
    require(box != nullptr, "class templates should expose the templated aggregate symbol");

    const ParsedSymbol* engine = findChildSymbol(document, inner->id, ParsedSymbolKind::Class, "Engine");
    require(engine != nullptr && engine->isDefinition, "Engine class should have a definition symbol");
    const ParsedSymbol* controller = findChildSymbol(document, engine->id, ParsedSymbolKind::Class, "Controller");
    require(controller != nullptr, "nested class should be parented to containing class");
    require(findChildSymbol(document, controller->id, ParsedSymbolKind::Field, "nestedValue") != nullptr,
            "nested class field should be parented to nested class");

    const ParsedSymbol* constructor = findChildSymbol(document, engine->id, ParsedSymbolKind::Constructor, "Engine");
    require(constructor != nullptr && constructor->isDefinition, "constructor symbol should be marked as a definition");
    const ParsedSymbol* destructor = findChildSymbol(document, engine->id, ParsedSymbolKind::Destructor, "~Engine");
    require(destructor != nullptr && destructor->isDefinition, "destructor symbol should be marked as a definition");
    require(countChildSymbols(document, engine->id, ParsedSymbolKind::Method, "compute") == 2,
            "overloaded methods should receive distinct symbols");

    const ParsedSymbol* start = findChildSymbol(document, engine->id, ParsedSymbolKind::Method, "start");
    require(start != nullptr && start->isDefinition, "method definition should be represented as a symbol");
    const ParsedSymbol* targetRpm = findChildSymbol(document, start->id, ParsedSymbolKind::Parameter, "targetRpm");
    require(targetRpm != nullptr, "method parameter should be parented to the method");
    const ParsedSymbol* localRpm = findChildSymbol(document, start->id, ParsedSymbolKind::LocalVariable, "localRpm");
    require(localRpm != nullptr, "local variable should be parented to the method");
    require(localRpm->canonicalType.find("int") != std::string::npos,
            "local variable canonical type should be recorded");
    require(findChildSymbol(document, start->id, ParsedSymbolKind::LocalVariable, "localState") != nullptr,
            "enum-typed local variable should have a symbol");

    require(hasResolvedReferenceToChildNamed(document, start->id, ParsedSymbolKind::Parameter, "targetRpm"),
            "parameter references should resolve to parameter symbols");
    require(hasResolvedReferenceTo(document, globalRpm->id), "global variable references should resolve to global symbols");

    for (const ParsedSymbol& symbol : document.symbols) {
        require(symbol.id != 0, "symbol ids should be non-zero");
        require(symbol.range.isValidFor(source.size()), "symbol source ranges should be valid");
    }
}

void testClangFrontendParsesPastedSingleFileWithHeadersAndMacroTypedef()
{
    const std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend();
    if (!clangExperimentsEnabled()) {
        require(clangFrontend == nullptr, "single-file pasted Clang parser test should stay disabled in default builds");
        return;
    }

    const std::string source =
        "#include <string>\n"
        "#include <vector>\n"
        "#define MAKE_TYPEDEF(T) typedef T MacroAlias;\n"
        "MAKE_TYPEDEF(int)\n"
        "namespace demo {\n"
        "typedef std::string RecordName;\n"
        "class Store {\n"
        "public:\n"
        "    typedef int MemberId;\n"
        "    std::vector<MemberId> ids;\n"
        "};\n"
        "}\n";

    const ModernizationFrontendResult result = clangFrontend->analyze(source);
    require(result.parseSucceeded,
            "Clang frontend should parse valid pasted single-file code with standard headers and macro typedefs");
    require(diagnosticsContain(result.diagnostics, "clang_parse=success"),
            "Clang frontend should report parse success for the pasted-code sample");
    require(diagnosticsContain(result.diagnostics, "virtual_file=input.cpp"),
            "Clang parse config diagnostics should report the stable pasted-code virtual filename");
    require(diagnosticsContain(result.diagnostics, "include_paths="),
            "Clang parse config diagnostics should report include path discovery");
    require(!diagnosticsContain(result.diagnostics, "CLANG DIAGNOSTIC severity=error"),
            "valid pasted-code sample should not emit Clang error diagnostics");

    const ParsedSymbol* recordName = findSymbol(result.document, ParsedSymbolKind::Typedef, "RecordName");
    require(recordName != nullptr, "namespace typedef should be represented as a symbol");
    const ParsedSymbol* memberId = findSymbol(result.document, ParsedSymbolKind::Typedef, "MemberId");
    require(memberId != nullptr, "class member typedef should be represented as a symbol");
    const ParsedSymbol* macroAlias = findSymbol(result.document, ParsedSymbolKind::Typedef, "MacroAlias");
    require(macroAlias != nullptr, "macro-generated typedef should not fail parsing");
}

void testClangFrontendFallbackOnInvalidSource()
{
    const std::unique_ptr<IModernizationFrontend> clangFrontend = createClangExperimentalFrontend();
    if (!clangExperimentsEnabled()) {
        require(clangFrontend == nullptr, "Clang frontend should remain disabled unless explicitly requested");
        return;
    }

    const std::string source = "struct Broken { void run( { int value = 0; }\n";
    const ModernizationFrontendResult result = clangFrontend->analyze(source);
    require(diagnosticsContain(result.diagnostics, "clang_parse=failure fallback=LightweightFrontend"),
            "Clang frontend should report fallback when parsing fails");
    require(result.document.originalSource == source, "fallback document should preserve original source");
    require(!result.document.tokens.empty(), "fallback document should still expose lightweight tokens");
}

void testFallbackOnUnsupportedSyntax()
{
    const std::string source = "void broken() { if (true) {\n";
    const ParsedDocument document = LightweightCppParser{}.parse(source);
    require(!document.parseSucceeded, "unbalanced source should report parser fallback state");
    require(document.originalSource == source, "fallback should preserve original source text");
    require(!document.tokens.empty(), "fallback should still expose tokenization where possible");
    require(!document.warnings.empty(), "fallback should report parser warning");
}

void testRewriteCoordinatorAppliesDescendingEdits()
{
    const std::string source = "abc def ghi";
    const RewriteApplicationResult result = RewriteCoordinator{}.apply(source,
                                                                       {
                                                                           testEdit(0, 3, "ABC", "first"),
                                                                           testEdit(8, 11, "GHI", "last"),
                                                                       });

    require(result.code == "ABC def GHI", "rewrite coordinator should apply edits by descending source offset");
    require(result.appliedEdits.size() == 2, "non-overlapping edits should both apply");
    require(result.skippedEdits.empty(), "valid non-overlapping edits should not be skipped");
}

void testRewriteCoordinatorRejectsOverlapsDuplicatesAndInvalidRanges()
{
    const std::string source = "abcdef";
    RewriteEdit duplicate = testEdit(0, 1, "A", "dup");
    RewriteEdit invalid = testEdit(4, 9, "Z", "invalid");
    RewriteEdit overlap = testEdit(1, 4, "BCD", "overlap");

    const RewriteApplicationResult result = RewriteCoordinator{}.apply(source,
                                                                       {
                                                                           duplicate,
                                                                           duplicate,
                                                                           testEdit(2, 5, "CDE", "winner"),
                                                                           overlap,
                                                                           invalid,
                                                                       });

    require(result.duplicateEdits == 1, "duplicate edits should be detected");
    require(result.invalidRanges == 1, "invalid source ranges should be detected");
    require(result.overlapConflicts == 1, "overlapping edits should be rejected");
    require(result.skippedEdits.size() == 3, "duplicate, invalid, and overlapping edits should be reported as skipped");
    require(result.appliedEdits.size() == 2, "valid non-overlapping edits should still apply");
}
}

int main()
{
    testTokenizationAndDirectives();
    testClassFunctionMemberAndLocalDetection();
    testStructEnumAndSourceRanges();
    testFunctionScopeAndLocalVariableDetection();
    testRepresentationParserBacking();
    testFrontendInterfaceDefaultsToLightweight();
    testOptionalClangFrontendFactory();
    testClangFrontendReadOnlyEntityPopulation();
    testClangFrontendSymbolResolutionGraph();
    testClangFrontendParsesPastedSingleFileWithHeadersAndMacroTypedef();
    testClangFrontendFallbackOnInvalidSource();
    testFallbackOnUnsupportedSyntax();
    testRewriteCoordinatorAppliesDescendingEdits();
    testRewriteCoordinatorRejectsOverlapsDuplicatesAndInvalidRanges();

    std::cout << "All parser layer tests passed.\n";
    return EXIT_SUCCESS;
}
