#pragma once

#include "parser/ScopeInfo.h"
#include "parser/SourceRange.h"
#include "parser/Token.h"

#include <string>
#include <vector>

enum class ParsedAggregateKind
{
    Class,
    Struct,
};

struct ParsedParameter
{
    std::string name;
    std::string type;
    SourceRange range;
    SourceRange nameRange;
};

struct ParsedIncludeDirective
{
    std::string path;
    SourceRange range;
};

struct ParsedMacroDirective
{
    std::string name;
    SourceRange range;
};

struct ParsedAggregate
{
    ParsedAggregateKind kind = ParsedAggregateKind::Class;
    std::string name;
    std::vector<std::string> baseNames;
    SourceRange range;
    SourceRange nameRange;
    SourceRange bodyRange;
};

struct ParsedEnum
{
    std::string name;
    bool scoped = false;
    std::string underlyingType;
    std::vector<std::string> enumerators;
    SourceRange range;
    SourceRange nameRange;
    SourceRange bodyRange;
};

struct ParsedFunction
{
    std::string name;
    std::string returnType;
    std::string parentName;
    std::vector<ParsedParameter> parameters;
    SourceRange range;
    SourceRange nameRange;
    SourceRange bodyRange;
    bool isMember = false;
    bool isConst = false;
    bool hasBody = false;
};

struct ParsedVariable
{
    std::string name;
    std::string type;
    std::string parentName;
    SourceRange range;
    SourceRange nameRange;
    bool isMember = false;
};

struct ParsedCallExpression
{
    std::string callee;
    std::string parentFunction;
    SourceRange range;
    SourceRange nameRange;
};

struct ParsedDocument
{
    std::string originalSource;
    std::vector<CppToken> tokens;
    std::vector<ScopeInfo> scopes;
    std::vector<ParsedIncludeDirective> includes;
    std::vector<ParsedMacroDirective> macros;
    std::vector<ParsedAggregate> aggregates;
    std::vector<ParsedEnum> enums;
    std::vector<ParsedFunction> functions;
    std::vector<ParsedVariable> memberVariables;
    std::vector<ParsedVariable> localVariables;
    std::vector<ParsedCallExpression> callExpressions;
    std::vector<std::string> warnings;
    bool parseSucceeded = true;
};
