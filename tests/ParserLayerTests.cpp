#include "converter/RawTextRepresentation.h"
#include "converter/RewriteCoordinator.h"
#include "frontend/FrontendFactory.h"
#include "frontend/LightweightFrontend.h"
#include "parser/LightweightCppParser.h"

#include <cstdlib>
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
    require(diagnosticsContain(result.diagnostics, "clang_experiment=disabled"),
            "default build diagnostics should report Clang experiment disabled");
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
    testFallbackOnUnsupportedSyntax();
    testRewriteCoordinatorAppliesDescendingEdits();
    testRewriteCoordinatorRejectsOverlapsDuplicatesAndInvalidRanges();

    std::cout << "All parser layer tests passed.\n";
    return EXIT_SUCCESS;
}
