#pragma once

#include <string>

class FunctionalModernizationValidator
{
public:
    [[nodiscard]] bool isValid(const std::string& code, std::string& reason) const;
};
