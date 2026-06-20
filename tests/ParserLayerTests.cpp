#include "converter/RawTextRepresentation.h"
#include "parser/LightweightCppParser.h"

#include <cstdlib>
#include <iostream>
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
        }
    }
    require(sawMain, "identifier tokens should be available");
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

    const ParsedFunction* reset = findFunction(document, "reset");
    require(reset != nullptr, "member function should be detected");
    require(reset->isMember, "member function should be marked as member");
    require(reset->parentName == "Widget", "member function should know parent class");
    require(reset->isConst, "const member function qualifier should be tracked");
    require(reset->parameters.size() == 1, "function parameter should be detected");
    require(reset->parameters.front().name == "value", "parameter name should be captured");
    require(reset->parameters.front().type == "int", "parameter type should be captured");

    const ParsedVariable* count = findMember(document, "count");
    require(count != nullptr, "member variable should be detected");
    require(count->type == "int", "member variable type should be captured");
    require(count->parentName == "Widget", "member variable parent should be captured");

    const ParsedVariable* local = findLocal(document, "local");
    require(local != nullptr, "local variable should be detected");
    require(local->parentName == "reset", "local variable parent function should be captured");

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

void testFallbackOnUnsupportedSyntax()
{
    const std::string source = "void broken() { if (true) {\n";
    const ParsedDocument document = LightweightCppParser{}.parse(source);
    require(!document.parseSucceeded, "unbalanced source should report parser fallback state");
    require(document.originalSource == source, "fallback should preserve original source text");
    require(!document.tokens.empty(), "fallback should still expose tokenization where possible");
    require(!document.warnings.empty(), "fallback should report parser warning");
}
}

int main()
{
    testTokenizationAndDirectives();
    testClassFunctionMemberAndLocalDetection();
    testStructEnumAndSourceRanges();
    testFunctionScopeAndLocalVariableDetection();
    testRepresentationParserBacking();
    testFallbackOnUnsupportedSyntax();

    std::cout << "All parser layer tests passed.\n";
    return EXIT_SUCCESS;
}
