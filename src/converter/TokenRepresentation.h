#pragma once

#include "converter/CodeRepresentation.h"

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

private:
    std::string source_;
    std::vector<std::string> tokens_;
};
