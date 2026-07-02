#pragma once

#include <string>

namespace CrashBreadcrumb
{
void installSignalHandlers();
void enter(const std::string& stage);
void exit(const std::string& stage);
void fail(const std::string& stage, const std::string& reason);
std::string lastStage();

class ScopedStage
{
public:
    explicit ScopedStage(std::string stage);
    ~ScopedStage();

    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

private:
    std::string stage_;
};
} // namespace CrashBreadcrumb
