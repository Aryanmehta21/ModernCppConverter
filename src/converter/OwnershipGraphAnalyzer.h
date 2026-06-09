#pragma once

#include <string>
#include <vector>

enum class OwnershipClassification
{
    ExclusiveOwnership,
    SharedOwnership,
    SequentialCollectionOwnership,
    FixedSizeCollectionOwnership,
    RawNonOwningObservation,
    AmbiguousOwnership,
};

struct OwnershipGraphNode
{
    std::string elementType;
    std::string storageName;
    std::string sizeExpression;
    std::string scopeName;
    OwnershipClassification classification = OwnershipClassification::AmbiguousOwnership;
    bool isClassMember = false;
    bool isPointerToPointer = false;
    bool isFixedPointerArray = false;
    bool isStdVectorRawPointer = false;
};

struct StringOwnershipOpportunity
{
    std::string className;
    std::string memberName;
    std::string reason;
};

class OwnershipGraphAnalyzer
{
public:
    [[nodiscard]] std::vector<OwnershipGraphNode> analyze(const std::string& code) const;
};

class StringOwnershipPatternDetector
{
public:
    [[nodiscard]] std::vector<StringOwnershipOpportunity> detect(const std::string& code) const;
};
