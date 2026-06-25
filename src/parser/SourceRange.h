#pragma once

#include <cstddef>
#include <optional>
#include <string>

enum class SourceEntityKind
{
    Unknown,
    Token,
    Scope,
    Class,
    Struct,
    Enum,
    Enumerator,
    Function,
    Variable,
    Member,
    Local,
    Include,
    Macro,
    Expression,
    Statement,
    Loop,
};

struct SourcePosition
{
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct SourceRange
{
    SourcePosition start;
    SourcePosition end;
    SourceEntityKind entityKind = SourceEntityKind::Unknown;
    std::string entityName;
    std::optional<std::size_t> parentScopeId;

    [[nodiscard]] bool isValid() const
    {
        return end.offset >= start.offset;
    }

    [[nodiscard]] bool isValidFor(std::size_t sourceSize) const
    {
        return isValid() && start.offset <= sourceSize && end.offset <= sourceSize;
    }

    [[nodiscard]] std::size_t length() const
    {
        return isValid() ? end.offset - start.offset : 0;
    }
};
