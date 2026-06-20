#pragma once

#include "converter/CodeRepresentation.h"

#include <optional>
#include <string>
#include <vector>

class TokenRepresentation final : public CodeRepresentation
{
public:
    TokenRepresentation(std::string source, std::vector<std::string> tokens);

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    virtual void replaceSourceText(std::string source) override;
    [[nodiscard]] const std::vector<std::string>& tokens() const;
    [[nodiscard]] const ParsedDocument* parsedDocument() const override;
    bool refreshParsedDocument() override;

private:
    std::string source_;
    std::vector<std::string> tokens_;
    mutable std::optional<ParsedDocument> parsedDocument_;
};
