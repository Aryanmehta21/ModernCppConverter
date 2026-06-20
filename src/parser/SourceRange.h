#pragma once

#include <cstddef>

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

    [[nodiscard]] bool isValid() const
    {
        return end.offset >= start.offset;
    }

    [[nodiscard]] std::size_t length() const
    {
        return isValid() ? end.offset - start.offset : 0;
    }
};
