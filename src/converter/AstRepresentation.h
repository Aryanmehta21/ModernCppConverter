#pragma once

#include "converter/CodeRepresentation.h"

#include <string>

class AstRepresentation final : public CodeRepresentation
{
public:
    AstRepresentation(std::string source, std::string astSummary = {});

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    virtual void replaceSourceText(std::string source) override;
    [[nodiscard]] const std::string& astSummary() const;

private:
    std::string source_;
    std::string astSummary_;
};
