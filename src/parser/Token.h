#pragma once

#include "parser/SourceRange.h"

#include <string>

enum class CppTokenKind
{
    Identifier,
    Keyword,
    Number,
    StringLiteral,
    CharLiteral,
    Comment,
    Preprocessor,
    Symbol,
};

struct CppToken
{
    std::string text;
    CppTokenKind kind = CppTokenKind::Symbol;
    SourceRange range;
};
