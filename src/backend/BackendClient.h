#pragma once

#include "backend/BackendConfig.h"
#include "backend/IBackendClient.h"

#include <QByteArray>
#include <QJsonObject>

class BackendClient final : public IBackendClient
{
public:
    explicit BackendClient(BackendConfig config = {});

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] BackendConversionResponse convert(const std::string& code,
                                                    const ModernizationOptions& options,
                                                    ConversionMode mode,
                                                    const ConversionResult* localResult) const override;

    [[nodiscard]] QByteArray serializeConversionRequest(const std::string& code,
                                                        const ModernizationOptions& options,
                                                        ConversionMode mode,
                                                        const ConversionResult* localResult) const;
    [[nodiscard]] BackendConversionResponse deserializeConversionResponse(const QByteArray& payload) const;
    [[nodiscard]] bool deserializeHealthResponse(const QByteArray& payload) const;

private:
    BackendConfig config_;

    [[nodiscard]] static QJsonObject optionsToJson(const ModernizationOptions& options);
    [[nodiscard]] static QJsonObject changeToJson(const ConversionChange& change);
    [[nodiscard]] static QJsonObject resultToJson(const ConversionResult& result);
    [[nodiscard]] static ConversionChange changeFromJson(const QJsonObject& object);
};
