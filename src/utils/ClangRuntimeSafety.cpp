#include "utils/ClangRuntimeSafety.h"

#include <QCoreApplication>
#include <QThread>

#include <cstdlib>

namespace ClangRuntimeSafety
{
bool inProcessClangAllowed()
{
    if (std::getenv("MODERNCPP_ALLOW_WORKER_CLANG") != nullptr) {
        return true;
    }

    const QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        return true;
    }

    return QThread::currentThread() == application->thread();
}

std::string inProcessClangBlockReason()
{
    if (inProcessClangAllowed()) {
        return {};
    }
    return "In-process Clang tooling disabled on worker thread for crash isolation";
}
} // namespace ClangRuntimeSafety
