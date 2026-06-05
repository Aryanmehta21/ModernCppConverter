#include "app/ConversionCoordinator.h"

#include <stdexcept>
#include <utility>

namespace
{
void appendAiReview(ConversionResult& localResult, const ConversionResult& aiResult)
{
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
        return {localEngine_->convert(code, options), ConversionMode::OfflineRuleBased, false, {}};
    }

    if (!backendClient_->isAvailable()) {
        return {
            localEngine_->convert(code, options),
            ConversionMode::OfflineRuleBased,
            true,
            "AI backend unavailable. Falling back to Offline Mode.",
        };
    }

    if (requestedMode == ConversionMode::OnlineAiAssisted) {
        BackendConversionResponse response = backendClient_->convert(code, options, requestedMode, nullptr);
        if (response.ok) {
            return {std::move(response.result), ConversionMode::OnlineAiAssisted, false, {}};
        }
        return {
            localEngine_->convert(code, options),
            ConversionMode::OfflineRuleBased,
            true,
            "AI backend unavailable. Falling back to Offline Mode.",
        };
    }

    ConversionResult localResult = localEngine_->convert(code, options);
    BackendConversionResponse response = backendClient_->convert(localResult.modernCode, options, requestedMode, &localResult);
    if (response.ok) {
        appendAiReview(localResult, response.result);
        return {std::move(localResult), ConversionMode::HybridOfflineAiReview, false, {}};
    }

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
