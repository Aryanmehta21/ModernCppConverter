#pragma once

#include "converter/CodeRepresentation.h"

class RawTextRepresentation final : public CodeRepresentation
{
public:
    explicit RawTextRepresentation(std::string source);

    [[nodiscard]] CodeRepresentationKind kind() const override;
    [[nodiscard]] std::string sourceText() const override;
    virtual void replaceSourceText(std::string source) override;

private:
    std::string source_;
};
