#pragma once

#include <string>
#include <vector>

class IncludeManager
{
public:
    [[nodiscard]] std::string ensureInclude(std::string code, const std::string& includeLine) const;
    [[nodiscard]] std::string removeIncludeIfUnused(std::string code,
                                                    const std::string& includeLine,
                                                    const std::vector<std::string>& usageNeedles) const;
};
