#include "converter/RawTextRepresentation.h"

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
}
