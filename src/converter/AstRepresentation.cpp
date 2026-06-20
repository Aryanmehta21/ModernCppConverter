#include "converter/AstRepresentation.h"

#include "parser/LightweightCppParser.h"

#include <utility>

AstRepresentation::AstRepresentation(std::string source, std::string astSummary)
    : source_(std::move(source))
    , astSummary_(std::move(astSummary))
{
}

CodeRepresentationKind AstRepresentation::kind() const
{
    return CodeRepresentationKind::Ast;
}

std::string AstRepresentation::sourceText() const
{
    return source_;
}

void AstRepresentation::replaceSourceText(std::string source)
{
    source_ = std::move(source);
    astSummary_.clear();
    parsedDocument_.reset();
}

const std::string& AstRepresentation::astSummary() const
{
    return astSummary_;
}

const ParsedDocument* AstRepresentation::parsedDocument() const
{
    if (!parsedDocument_.has_value()) {
        parsedDocument_ = LightweightCppParser{}.parse(source_);
    }
    return &*parsedDocument_;
}

bool AstRepresentation::refreshParsedDocument()
{
    parsedDocument_ = LightweightCppParser{}.parse(source_);
    return parsedDocument_->parseSucceeded;
}
