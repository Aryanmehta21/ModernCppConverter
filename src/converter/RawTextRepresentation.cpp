#include "converter/RawTextRepresentation.h"

#include "parser/LightweightCppParser.h"

#include <utility>

RawTextRepresentation::RawTextRepresentation(std::string source)
    : source_(std::move(source))
{
}

CodeRepresentationKind RawTextRepresentation::kind() const
{
    return CodeRepresentationKind::RawText;
}

std::string RawTextRepresentation::sourceText() const
{
    return source_;
}

void RawTextRepresentation::replaceSourceText(std::string source)
{
    source_ = std::move(source);
    parsedDocument_.reset();
}

const ParsedDocument* RawTextRepresentation::parsedDocument() const
{
    if (!parsedDocument_.has_value()) {
        parsedDocument_ = LightweightCppParser{}.parse(source_);
    }
    return &*parsedDocument_;
}

bool RawTextRepresentation::refreshParsedDocument()
{
    parsedDocument_ = LightweightCppParser{}.parse(source_);
    return parsedDocument_->parseSucceeded;
}
