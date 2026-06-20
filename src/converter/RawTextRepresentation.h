#pragma once

#include "converter/CodeRepresentation.h"

#include <optional>

class RawTextRepresentation final : public CodeRepresentation
{
public:
    explicit RawTextRepresentation(std::string source);

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    virtual void replaceSourceText(std::string source) override;
    [[nodiscard]] const ParsedDocument* parsedDocument() const override;
    bool refreshParsedDocument() override;

private:
    std::string source_;
    mutable std::optional<ParsedDocument> parsedDocument_;
};
