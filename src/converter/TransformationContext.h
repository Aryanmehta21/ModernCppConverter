#pragma once

#include <string>
#include <vector>

struct TypeChangeRecord
{
    std::string symbolName;
    std::string oldType;
    std::string newType;
    std::string scopeName;
    bool isClassMember = false;
    std::string transformationRule;
    std::vector<std::string> requiredCleanupActions;
    std::vector<std::string> warnings;
    bool verificationPassed = false;
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
