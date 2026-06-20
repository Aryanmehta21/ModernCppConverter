#pragma once

#include "converter/CodeRepresentation.h"

#include <optional>
#include <string>

class AstRepresentation final : public CodeRepresentation
{
public:
    AstRepresentation(std::string source, std::string astSummary = {});

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    virtual void replaceSourceText(std::string source) override;
    [[nodiscard]] const std::string& astSummary() const;
    [[nodiscard]] const ParsedDocument* parsedDocument() const override;
    bool refreshParsedDocument() override;

private:
    std::string source_;
    std::string astSummary_;
    mutable std::optional<ParsedDocument> parsedDocument_;
};
