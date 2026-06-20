#pragma once

#include "parser/SourceRange.h"

#include <cstddef>
#include <optional>
#include <string>

enum class ScopeKind
{
    Global,
    Namespace,
    Class,
    Struct,
    Function,
    Block,
};

struct ScopeInfo
{
    ScopeKind kind = ScopeKind::Block;
    std::string name;
    SourceRange range;
    std::optional<std::size_t> parentIndex;
};
