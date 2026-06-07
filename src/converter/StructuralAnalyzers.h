#pragma once

#include "converter/CodeStructure.h"

#include <string>
#include <vector>

class PreprocessorAnalyzer
{
public:
    [[nodiscard]] std::vector<PreprocessorBlock> analyze(const std::string& code) const;
};

class TypeDeclarationAnalyzer
{
public:
    [[nodiscard]] std::vector<StructTypedefDeclaration> analyzeTypedefStructs(const std::string& code) const;
};

class ClassResourceAnalyzer
{
public:
    [[nodiscard]] std::vector<ClassBlock> analyzeClasses(const std::string& code) const;
};

class LoopAnalyzer
{
public:
    [[nodiscard]] std::vector<LoopBlock> analyzeLoops(const std::string& code) const;
};

class OwnershipAnalyzer
{
public:
    [[nodiscard]] bool hasPotentialPointerEscape(const std::string& code, const std::string& variableName) const;
    [[nodiscard]] bool hasPointerArithmetic(const std::string& code, const std::string& variableName) const;
};
