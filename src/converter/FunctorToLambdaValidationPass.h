#pragma once

#include <string>

class FunctorToLambdaValidationPass
{
public:
    [[nodiscard]] bool isValid(const std::string& code, std::string& reason) const;
};
