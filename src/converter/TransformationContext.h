#pragma once

#include <string>
#include <utility>
#include <vector>

struct TypeChangeRecord
{
    TypeChangeRecord() = default;

    TypeChangeRecord(std::string symbolNameValue,
                     std::string oldTypeValue,
                     std::string newTypeValue,
                     std::string scopeNameValue,
                     bool isClassMemberValue,
                     std::string transformationRuleValue,
                     std::vector<std::string> requiredCleanupActionsValue,
                     std::vector<std::string> warningsValue,
                     bool verificationPassedValue,
                     std::vector<std::string> modernizationTagsValue = {})
        : symbolName(std::move(symbolNameValue))
        , oldType(std::move(oldTypeValue))
        , newType(std::move(newTypeValue))
        , scopeName(std::move(scopeNameValue))
        , isClassMember(isClassMemberValue)
        , transformationRule(std::move(transformationRuleValue))
        , requiredCleanupActions(std::move(requiredCleanupActionsValue))
        , warnings(std::move(warningsValue))
        , verificationPassed(verificationPassedValue)
        , modernizationTags(std::move(modernizationTagsValue))
    {
    }

    std::string symbolName;
    std::string oldType;
    std::string newType;
    std::string scopeName;
    bool isClassMember = false;
    std::string transformationRule;
    std::vector<std::string> requiredCleanupActions;
    std::vector<std::string> warnings;
    bool verificationPassed = false;
    std::vector<std::string> modernizationTags;
};

class TransformationContext
{
public:
    void registerTypeChange(TypeChangeRecord record);

    [[nodiscard]] const std::vector<TypeChangeRecord>& typeChanges() const;
    [[nodiscard]] std::vector<TypeChangeRecord> typeChangesForNewType(const std::string& newTypePrefix) const;
    [[nodiscard]] bool empty() const;

private:
    std::vector<TypeChangeRecord> typeChanges_;
};
