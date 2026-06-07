#include "converter/TransformationContext.h"

#include <utility>

void TransformationContext::registerTypeChange(TypeChangeRecord record)
{
    typeChanges_.push_back(std::move(record));
}

const std::vector<TypeChangeRecord>& TransformationContext::typeChanges() const
{
    return typeChanges_;
}

std::vector<TypeChangeRecord> TransformationContext::typeChangesForNewType(const std::string& newTypePrefix) const
{
    std::vector<TypeChangeRecord> records;
    for (const TypeChangeRecord& record : typeChanges_) {
        if (record.newType.starts_with(newTypePrefix)) {
            records.push_back(record);
        }
    }
    return records;
}

bool TransformationContext::empty() const
{
    return typeChanges_.empty();
}
