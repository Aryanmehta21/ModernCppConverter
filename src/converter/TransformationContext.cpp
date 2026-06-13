#include "converter/TransformationContext.h"

#include <algorithm>
#include <utility>

namespace
{
void addUnique(std::vector<std::string>& values, const std::string& value)
{
    if (value.empty() || std::find(values.begin(), values.end(), value) != values.end()) {
        return;
    }
    values.push_back(value);
}

void addDefaultTags(TypeChangeRecord& record)
{
    addUnique(record.modernizationTags, "Modernized");
    addUnique(record.modernizationTags, "NodeModernized");
    addUnique(record.modernizationTags, "AlreadyProcessed");
    if (record.newType.starts_with("std::vector<")) {
        addUnique(record.modernizationTags, "OwnershipConverted");
        addUnique(record.modernizationTags, "RawArrayConverted");
        addUnique(record.modernizationTags, "VectorMigrationApplied");
    } else if (record.newType.starts_with("std::unique_ptr<")
               || record.newType.starts_with("std::shared_ptr<")) {
        addUnique(record.modernizationTags, "OwnershipConverted");
    } else if (record.newType == "std::string" || record.newType.starts_with("std::array<")) {
        addUnique(record.modernizationTags, "OwnershipConverted");
    }
    if (record.transformationRule.find("Rule of Zero") != std::string::npos) {
        addUnique(record.modernizationTags, "RuleOfZeroApplied");
    }
    if (record.transformationRule.find("copy") != std::string::npos
        || record.transformationRule.find("Copy") != std::string::npos) {
        addUnique(record.modernizationTags, "CopyCtorProcessed");
    }
    if (record.transformationRule.find("destructor") != std::string::npos
        || record.transformationRule.find("Destructor") != std::string::npos) {
        addUnique(record.modernizationTags, "DestructorProcessed");
    }
}

void mergeRecord(TypeChangeRecord& existing, const TypeChangeRecord& incoming)
{
    for (const std::string& action : incoming.requiredCleanupActions) {
        addUnique(existing.requiredCleanupActions, action);
    }
    for (const std::string& warning : incoming.warnings) {
        addUnique(existing.warnings, warning);
    }
    for (const std::string& tag : incoming.modernizationTags) {
        addUnique(existing.modernizationTags, tag);
    }
    existing.verificationPassed = existing.verificationPassed || incoming.verificationPassed;
}
} // namespace

void TransformationContext::registerTypeChange(TypeChangeRecord record)
{
    addDefaultTags(record);
    for (TypeChangeRecord& existing : typeChanges_) {
        if (existing.symbolName == record.symbolName
            && existing.scopeName == record.scopeName
            && existing.oldType == record.oldType
            && existing.newType == record.newType
            && existing.isClassMember == record.isClassMember) {
            mergeRecord(existing, record);
            return;
        }
    }
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
