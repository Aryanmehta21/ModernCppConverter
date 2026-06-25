#pragma once

#include <string>

struct PostConversionFormatterConfig
{
    std::string clangFormatPathOverride;
    bool allowLightweightFallback = true;
    int timeoutMs = 5000;
};

struct PostConversionFormattingResult
{
    std::string code;
    bool attempted = false;
    bool applied = false;
    std::string formatterName;
    std::string diagnostic;
};

class PostConversionFormatter
{
public:
    explicit PostConversionFormatter(PostConversionFormatterConfig config = {});

    [[nodiscard]] PostConversionFormattingResult format(const std::string& code) const;

private:
    PostConversionFormatterConfig config_;
};
