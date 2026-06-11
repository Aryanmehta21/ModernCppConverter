#pragma once

#include "converter/CodeStructure.h"

#include <cstddef>
#include <string>
#include <vector>

struct DestructorContext
{
    bool exists = false;
    bool isVirtual = false;
    bool hasOverride = false;
    std::string name;
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t headerStart = 0;
    std::size_t headerEnd = 0;
    std::string text;
};

struct ClassContext
{
    ClassBlock block;
    std::string name;
    std::vector<std::string> baseNames;
    bool confident = false;
    bool hasVirtualMethods = false;
    DestructorContext destructor;
};

class ClassContextResolver
{
public:
    [[nodiscard]] std::vector<ClassContext> resolve(const std::string& code) const;
};
