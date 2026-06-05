#include "converter/TokenRepresentation.h"

#include <utility>

TokenRepresentation::TokenRepresentation(std::string source, std::vector<std::string> tokens)
    : source_(std::move(source))
    , tokens_(std::move(tokens))
{
}

CodeRepresentationKind TokenRepresentation::kind() const
{
    return CodeRepresentationKind::Token;
}

std::string TokenRepresentation::sourceText() const
{
    return source_;
}

void TokenRepresentation::replaceSourceText(std::string source)
{
    source_ = std::move(source);
    tokens_.clear();
}

const std::vector<std::string>& TokenRepresentation::tokens() const
{
    return tokens_;
}
