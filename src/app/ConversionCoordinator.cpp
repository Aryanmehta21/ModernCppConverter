#include "app/ConversionCoordinator.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>

#include <iterator>
#include <stdexcept>
#include <utility>

namespace
{
QString modeName(ConversionMode mode)
{
    switch (mode) {
    case ConversionMode::OfflineRuleBased:
        return "Offline Rule-Based";
    case ConversionMode::OnlineAiAssisted:
        return "Online AI-Assisted";
    case ConversionMode::HybridOfflineAiReview:
        return "Hybrid Offline + AI Review";
    }
    return "Offline Rule-Based";
}

std::string currentTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss").toStdString();
}

std::string modernizationLevelName(OfflineModernizationLevel level)
{
    switch (level) {
    case OfflineModernizationLevel::Conservative:
        return "Conservative";
    case OfflineModernizationLevel::Balanced:
        return "Balanced";
    case OfflineModernizationLevel::AggressiveSafe:
        return "Aggressive Safe";
    case OfflineModernizationLevel::AiStyleAggressiveRewrite:
        return "AI-Style Aggressive Rewrite";
    }
    return "Balanced";
}

std::string targetStandardName(CppStandard standard)
{
    return standard == CppStandard::Cpp17 ? "C++17" : "C++20";
}

std::string optionsDiagnostic(const ModernizationOptions& options)
{
    return "selected modernization options level=" + modernizationLevelName(options.offlineModernizationLevel)
        + " target=" + targetStandardName(options.targetStandard)
        + " compileVerification=" + (options.compileVerificationEnabled ? "enabled" : "profile-default")
        + " auto=" + (options.useAuto ? "on" : "off")
        + " lambdas=" + (options.useLambdas ? "on" : "off")
        + " constexpr=" + (options.useConstexpr ? "on" : "off")
        + " smartPointers=" + (options.useSmartPointers ? "on" : "off")
        + " rangeFor=" + (options.useRangeBasedFor ? "on" : "off")
        + " structuredBindings=" + (options.useStructuredBindings ? "on" : "off")
        + " ownershipModernization=" + (options.applySafeOwnershipModernization ? "on" : "off");
}

void stampResult(ConversionResult& result,
                 ConversionMode source,
                 std::string backendStatus,
                 std::string aiProvider = {},
                 std::string aiModel = {},
                 bool fallbackUsed = false,
                 std::vector<std::string> diagnostics = {})
{
    result.conversionSource = fallbackUsed ? "Offline Fallback after AI Failure" : modeName(source).toStdString();
    result.backendStatus = std::move(backendStatus);
    result.aiProvider = std::move(aiProvider);
    result.aiModel = std::move(aiModel);
    result.fallbackUsed = fallbackUsed;
    result.convertedAt = currentTimestamp();
    result.diagnosticMessages.insert(result.diagnosticMessages.end(),
                                     std::make_move_iterator(diagnostics.begin()),
                                     std::make_move_iterator(diagnostics.end()));
}

void applyOfflineSourceLabel(ConversionResult& result)
{
    if (result.rewriteLevel == "Offline Aggressive AI-like Rewrite") {
        result.conversionSource = "Offline Aggressive AI-like Rewrite";
    }
}

void appendAiReview(ConversionResult& localResult, const ConversionResult& aiResult)
{
    if (!aiResult.modernCode.empty()) {
        localResult.modernCode = aiResult.modernCode;
    }

    for (ConversionChange change : aiResult.changes) {
        change.ruleName = "AI Review: " + change.ruleName;
        localResult.changes.push_back(std::move(change));
    }

    if (!aiResult.explanation.empty()) {
        localResult.explanation += "\n\nAI Review\n=========\n\n";
        localResult.explanation += aiResult.explanation;
    }
}
} // namespace

ConversionCoordinator::ConversionCoordinator(std::unique_ptr<IConverterEngine> localEngine,
                                             std::unique_ptr<IBackendClient> backendClient)
    : localEngine_(std::move(localEngine))
    , backendClient_(std::move(backendClient))
{
    if (!localEngine_) {
        throw std::invalid_argument("ConversionCoordinator requires a local engine.");
    }
    if (!backendClient_) {
        throw std::invalid_argument("ConversionCoordinator requires a backend client.");
    }
}

CoordinatedConversionResult ConversionCoordinator::convert(const std::string& code,
                                                           const ModernizationOptions& options,
                                                           ConversionMode requestedMode) const
{
    if (requestedMode == ConversionMode::OfflineRuleBased) {
        qInfo() << "Offline mode selected";
        QElapsedTimer timer;
        timer.start();
        qInfo() << "Offline pipeline started";
        ConversionResult result = localEngine_->convert(code, options);
        const qint64 elapsed = timer.elapsed();
        qInfo() << "Offline pipeline finished; elapsed_ms =" << elapsed;
        result.diagnosticMessages.push_back("selected mode: " + modeName(requestedMode).toStdString());
        result.diagnosticMessages.push_back(optionsDiagnostic(options));
        result.diagnosticMessages.push_back("offline pipeline finished elapsed_ms=" + std::to_string(elapsed));
        stampResult(result, ConversionMode::OfflineRuleBased, "Not used");
        applyOfflineSourceLabel(result);
        return {std::move(result), ConversionMode::OfflineRuleBased, false, {}};
    }

    qInfo() << modeName(requestedMode) << "selected";
    qInfo() << "Backend health check starts";
    if (!backendClient_->isAvailable()) {
        qWarning() << "Backend health check failed; offline fallback will be used";
        QElapsedTimer timer;
        timer.start();
        qInfo() << "Offline fallback pipeline started";
        ConversionResult result = localEngine_->convert(code, options);
        const qint64 elapsed = timer.elapsed();
        qInfo() << "Offline fallback pipeline finished; elapsed_ms =" << elapsed;
        result.diagnosticMessages.push_back("selected mode: " + modeName(requestedMode).toStdString());
        result.diagnosticMessages.push_back(optionsDiagnostic(options));
        result.diagnosticMessages.push_back("offline fallback pipeline finished elapsed_ms=" + std::to_string(elapsed));
        stampResult(result,
                    ConversionMode::OfflineRuleBased,
                    "Unavailable",
                    {},
                    {},
                    true,
                    {"AI backend unavailable. Falling back to Offline Mode."});
        applyOfflineSourceLabel(result);
        return {
            std::move(result),
            ConversionMode::OfflineRuleBased,
            true,
            "AI backend unavailable. Falling back to Offline Mode.",
        };
    }
    qInfo() << "Backend health check succeeded";

    if (requestedMode == ConversionMode::OnlineAiAssisted) {
        qInfo() << "Online conversion request starts";
        BackendConversionResponse response = backendClient_->convert(code, options, requestedMode, nullptr);
        if (response.ok) {
            qInfo() << "Online conversion request succeeded";
            stampResult(response.result,
                        ConversionMode::OnlineAiAssisted,
                        response.result.backendStatus.empty() ? "Connected" : response.result.backendStatus,
                        response.result.aiProvider,
                        response.result.aiModel,
                        false,
                        response.result.diagnosticMessages);
            return {std::move(response.result), ConversionMode::OnlineAiAssisted, false, {}};
        }
        qWarning() << "Online conversion request failed; offline fallback will be used:" << QString::fromStdString(response.errorMessage);
        ConversionResult result = localEngine_->convert(code, options);
        result.diagnosticMessages.push_back("selected mode: " + modeName(requestedMode).toStdString());
        result.diagnosticMessages.push_back(optionsDiagnostic(options));
        stampResult(result,
                    ConversionMode::OfflineRuleBased,
                    "Request failed",
                    response.result.aiProvider,
                    response.result.aiModel,
                    true,
                    {response.errorMessage.empty() ? "AI backend conversion failed. Falling back to Offline Mode." : response.errorMessage});
        applyOfflineSourceLabel(result);
        return {
            std::move(result),
            ConversionMode::OfflineRuleBased,
            true,
            "AI backend unavailable. Falling back to Offline Mode.",
        };
    }

    QElapsedTimer localTimer;
    localTimer.start();
    qInfo() << "Hybrid local offline pipeline started";
    ConversionResult localResult = localEngine_->convert(code, options);
    const qint64 localElapsed = localTimer.elapsed();
    qInfo() << "Hybrid local offline pipeline finished; elapsed_ms =" << localElapsed;
    localResult.diagnosticMessages.push_back("selected mode: " + modeName(requestedMode).toStdString());
    localResult.diagnosticMessages.push_back(optionsDiagnostic(options));
    localResult.diagnosticMessages.push_back("hybrid local offline pipeline finished elapsed_ms=" + std::to_string(localElapsed));
    qInfo() << "Hybrid conversion request starts after local conversion";
    BackendConversionResponse response = backendClient_->convert(localResult.modernCode, options, requestedMode, &localResult);
    if (response.ok) {
        qInfo() << "Hybrid conversion request succeeded";
        const std::string provider = response.result.aiProvider;
        const std::string model = response.result.aiModel;
        std::vector<std::string> diagnostics = response.result.diagnosticMessages;
        appendAiReview(localResult, response.result);
        stampResult(localResult,
                    ConversionMode::HybridOfflineAiReview,
                    response.result.backendStatus.empty() ? "Connected" : response.result.backendStatus,
                    provider,
                    model,
                    false,
                    std::move(diagnostics));
        return {std::move(localResult), ConversionMode::HybridOfflineAiReview, false, {}};
    }

    qWarning() << "Hybrid conversion request failed; keeping offline result:" << QString::fromStdString(response.errorMessage);
    stampResult(localResult,
                ConversionMode::OfflineRuleBased,
                "Request failed",
                response.result.aiProvider,
                response.result.aiModel,
                true,
                {response.errorMessage.empty() ? "AI backend conversion failed. Keeping offline result." : response.errorMessage});
    applyOfflineSourceLabel(localResult);
    return {
        std::move(localResult),
        ConversionMode::OfflineRuleBased,
        true,
        "AI backend unavailable. Falling back to Offline Mode.",
    };
}

bool ConversionCoordinator::backendAvailable() const
{
    return backendClient_->isAvailable();
}
