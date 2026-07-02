#include "utils/CrashBreadcrumb.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace
{
constexpr std::size_t maxStageLength = 255;
std::array<char, maxStageLength + 1> lastStageBuffer{};
std::mutex breadcrumbMutex;
std::atomic_bool handlersInstalled{false};

void copyLastStage(const std::string& stage)
{
    const std::size_t length = std::min(stage.size(), maxStageLength);
    std::memcpy(lastStageBuffer.data(), stage.data(), length);
    lastStageBuffer[length] = '\0';
}

void printBreadcrumb(const char* kind, const std::string& stage, const std::string& reason = {})
{
    std::lock_guard<std::mutex> lock(breadcrumbMutex);
    copyLastStage(stage);
    std::cerr << kind << ' ' << stage;
    if (!reason.empty()) {
        std::cerr << ' ' << reason;
    }
    std::cerr << std::endl;
}

const char* signalName(int signalNumber)
{
    switch (signalNumber) {
#if defined(SIGBUS)
    case SIGBUS:
        return "SIGBUS";
#endif
    case SIGSEGV:
        return "SIGSEGV";
    case SIGILL:
        return "SIGILL";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "SIGNAL";
    }
}

void crashSignalHandler(int signalNumber)
{
    const char* prefix = "\nCRASH ";
    const char* middle = " last_stage=\"";
    const char* suffix = "\"\n";
    const char* name = signalName(signalNumber);
#if defined(__unix__) || defined(__APPLE__)
    (void)!::write(STDERR_FILENO, prefix, std::strlen(prefix));
    (void)!::write(STDERR_FILENO, name, std::strlen(name));
    (void)!::write(STDERR_FILENO, middle, std::strlen(middle));
    (void)!::write(STDERR_FILENO, lastStageBuffer.data(), std::strlen(lastStageBuffer.data()));
    (void)!::write(STDERR_FILENO, suffix, std::strlen(suffix));
#else
    std::cerr << prefix << name << middle << lastStageBuffer.data() << suffix;
#endif
    std::signal(signalNumber, SIG_DFL);
    std::raise(signalNumber);
}
} // namespace

namespace CrashBreadcrumb
{
void installSignalHandlers()
{
    bool expected = false;
    if (!handlersInstalled.compare_exchange_strong(expected, true)) {
        return;
    }
    copyLastStage("startup");
#if defined(SIGBUS)
    std::signal(SIGBUS, crashSignalHandler);
#endif
    std::signal(SIGSEGV, crashSignalHandler);
    std::signal(SIGILL, crashSignalHandler);
    std::signal(SIGABRT, crashSignalHandler);
}

void enter(const std::string& stage)
{
    printBreadcrumb("ENTER", stage);
}

void exit(const std::string& stage)
{
    printBreadcrumb("EXIT", stage);
}

void fail(const std::string& stage, const std::string& reason)
{
    printBreadcrumb("FAIL", stage, reason);
}

std::string lastStage()
{
    std::lock_guard<std::mutex> lock(breadcrumbMutex);
    return lastStageBuffer.data();
}

ScopedStage::ScopedStage(std::string stage)
    : stage_(std::move(stage))
{
    enter(stage_);
}

ScopedStage::~ScopedStage()
{
    exit(stage_);
}
} // namespace CrashBreadcrumb
