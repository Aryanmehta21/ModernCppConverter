#pragma once

#include <string>

class ModernizationPolishValidator
{
public:
    [[nodiscard]] bool isValid(const std::string& code, std::string& reason) const;
};

