#pragma once

#include "converter/CodeRepresentation.h"

#include <string>

class FrontendCodeRepresentation final : public CodeRepresentation
{
public:
    FrontendCodeRepresentation(std::string source, ParsedDocument document);

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    void replaceSourceText(std::string source) override;
    [[nodiscard]] const ParsedDocument* parsedDocument() const override;
    bool refreshParsedDocument() override;

private:
    std::string source_;
    ParsedDocument document_;
};
