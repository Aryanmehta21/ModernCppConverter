#include "converter/FrontendCodeRepresentation.h"

#include <utility>

FrontendCodeRepresentation::FrontendCodeRepresentation(std::string source, ParsedDocument document)
    : source_(std::move(source))
    , document_(std::move(document))
{
    document_.originalSource = source_;
}

CodeRepresentationKind FrontendCodeRepresentation::kind() const
{
    return CodeRepresentationKind::Ast;
}

std::string FrontendCodeRepresentation::sourceText() const
{
    return source_;
}

void FrontendCodeRepresentation::replaceSourceText(std::string source)
{
    source_ = std::move(source);
    document_.originalSource = source_;
    document_.parseSucceeded = false;
    document_.warnings.push_back("FrontendCodeRepresentation source was replaced; parse refresh requires frontend selection.");
}

const ParsedDocument* FrontendCodeRepresentation::parsedDocument() const
{
    return &document_;
}

bool FrontendCodeRepresentation::refreshParsedDocument()
{
    return document_.parseSucceeded;
}
