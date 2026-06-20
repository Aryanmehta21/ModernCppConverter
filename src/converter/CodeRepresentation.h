#pragma once

#include "parser/ParsedEntity.h"

#include <string>

enum class CodeRepresentationKind
{
    RawText,
    Token,
    Ast,
};

class CodeRepresentation
{
public:
    virtual ~CodeRepresentation() = default;

    [[nodiscard]] virtual CodeRepresentationKind kind() const = 0;
    [[nodiscard]] virtual std::string sourceText() const = 0;
    virtual void replaceSourceText(std::string source) = 0;
    [[nodiscard]] virtual const ParsedDocument* parsedDocument() const
    {
        return nullptr;
    }
    virtual bool refreshParsedDocument()
    {
        return false;
    }
};
