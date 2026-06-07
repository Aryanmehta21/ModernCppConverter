#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct PreprocessorBlock
{
    std::size_t startLine = 0;
    std::size_t endLine = 0;
    std::string openingDirective;
    std::vector<std::string> bodyLines;
};

struct StructTypedefDeclaration
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string declarationText;
    std::string tagName;
    std::string aliasName;
    std::string body;
    bool containsFunctionPointer = false;
};

struct ClassBlock
{
    std::size_t start = 0;
    std::size_t openBrace = 0;
    std::size_t closeBrace = 0;
    std::size_t end = 0;
    std::string keyword;
    std::string name;
    std::string text;
};

struct LoopBlock
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
    std::string header;
    std::string body;
};

struct CodeStructure
{
    std::vector<PreprocessorBlock> preprocessorBlocks;
    std::vector<StructTypedefDeclaration> typedefStructs;
    std::vector<ClassBlock> classes;
    std::vector<LoopBlock> loops;
};
