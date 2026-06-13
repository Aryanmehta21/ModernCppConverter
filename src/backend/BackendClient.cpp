#include "backend/BackendClient.h"

#include <QEventLoop>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace
{
QString modeToString(ConversionMode mode)
{
    switch (mode) {
    case ConversionMode::OfflineRuleBased:
        return "offline";
    case ConversionMode::OnlineAiAssisted:
        return "online";
    case ConversionMode::HybridOfflineAiReview:
        return "hybrid";
    }
    return "offline";
}

QString aggressivenessForMode(ConversionMode mode)
{
    switch (mode) {
    case ConversionMode::OnlineAiAssisted:
        return "balanced";
    case ConversionMode::HybridOfflineAiReview:
        return "aggressive_safe";
    case ConversionMode::OfflineRuleBased:
        return "conservative";
    }
    return "conservative";
}
} // namespace

BackendClient::BackendClient(BackendConfig config)
    : config_(std::move(config))
{
}

bool BackendClient::isAvailable() const
{
    QElapsedTimer elapsed;
    elapsed.start();
    qInfo() << "Backend health check starts for" << config_.backendUrl;
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(config_.backendUrl + "/health"));
    QNetworkReply* reply = manager.get(request);

    QTimer timer;
    timer.setSingleShot(true);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(config_.requestTimeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
        const bool ok = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && deserializeHealthResponse(reply->readAll());
        qInfo() << "Backend health check" << (ok ? "succeeded" : "failed") << "elapsed_ms =" << elapsed.elapsed();
        reply->deleteLater();
        return ok;
    }

    reply->abort();
    reply->deleteLater();
    qWarning() << "Backend health check timed out; elapsed_ms =" << elapsed.elapsed();
    return false;
}

BackendConversionResponse BackendClient::convert(const std::string& code,
                                                 const ModernizationOptions& options,
                                                 ConversionMode mode,
                                                 const ConversionResult* localResult) const
{
    QElapsedTimer elapsed;
    elapsed.start();
    qInfo() << "Backend conversion request starts; mode =" << modeToString(mode);
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(config_.backendUrl + "/api/convert"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = manager.post(request, serializeConversionRequest(code, options, mode, localResult));

    QTimer timer;
    timer.setSingleShot(true);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(config_.requestTimeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        qWarning() << "Backend conversion request timed out; elapsed_ms =" << elapsed.elapsed();
        return {false, {}, "Backend request timed out."};
    }

    timer.stop();
    if (reply->error() != QNetworkReply::NoError) {
        const std::string error = reply->errorString().toStdString();
        reply->deleteLater();
        qWarning() << "Backend conversion request failed:" << QString::fromStdString(error) << "elapsed_ms =" << elapsed.elapsed();
        return {false, {}, error};
    }

    const QByteArray payload = reply->readAll();
    reply->deleteLater();
    BackendConversionResponse response = deserializeConversionResponse(payload);
    qInfo() << "Backend conversion request" << (response.ok ? "succeeded" : "failed") << "elapsed_ms =" << elapsed.elapsed();
    return response;
}

QByteArray BackendClient::serializeConversionRequest(const std::string& code,
                                                     const ModernizationOptions& options,
                                                     ConversionMode mode,
                                                     const ConversionResult* localResult) const
{
    QJsonObject object;
    object["code"] = QString::fromStdString(code);
    object["mode"] = modeToString(mode);
    object["aggressivenessLevel"] = aggressivenessForMode(mode);
    object["options"] = optionsToJson(options);
    if (localResult != nullptr) {
        object["localResult"] = resultToJson(*localResult);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

BackendConversionResponse BackendClient::deserializeConversionResponse(const QByteArray& payload) const
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return {false, {}, "Backend returned invalid JSON."};
    }

    const QJsonObject object = document.object();
    if (!object.value("ok").toBool(false)) {
        ConversionResult result;
        result.backendStatus = object.value("backendStatus").toString("Error").toStdString();
        result.aiProvider = object.value("aiProvider").toString().toStdString();
        result.aiModel = object.value("aiModel").toString().toStdString();
        return {false, result, object.value("error").toString("Backend conversion failed.").toStdString()};
    }

    ConversionResult result;
    result.modernCode = object.value("modernCode").toString().toStdString();
    result.explanation = object.value("explanation").toString().toStdString();
    result.backendStatus = object.value("backendStatus").toString("Connected").toStdString();
    result.aiProvider = object.value("aiProvider").toString().toStdString();
    result.aiModel = object.value("aiModel").toString().toStdString();

    const QJsonArray warnings = object.value("warnings").toArray();
    if (!warnings.empty()) {
        result.explanation += "\n\nWarnings\n========\n\n";
        for (const QJsonValue& warning : warnings) {
            result.explanation += "- ";
            result.explanation += warning.toString().toStdString();
            result.explanation += "\n";
            result.diagnosticMessages.push_back(warning.toString().toStdString());
        }
    }

    const QJsonArray changes = object.value("changes").toArray();
    result.changes.reserve(static_cast<std::size_t>(changes.size()));
    for (const QJsonValue& value : changes) {
        if (value.isObject()) {
            result.changes.push_back(changeFromJson(value.toObject()));
        }
    }

    const QJsonArray suggestions = object.value("suggestions").toArray();
    for (const QJsonValue& suggestion : suggestions) {
        result.changes.push_back({
            "AI suggestion",
            suggestion.toString().toStdString(),
            "",
            "Suggestion returned by the backend AI service.",
            false,
            false,
        });
    }

    return {true, result, {}};
}

bool BackendClient::deserializeHealthResponse(const QByteArray& payload) const
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    return document.isObject() && document.object().value("status").toString() == "ok";
}

QJsonObject BackendClient::optionsToJson(const ModernizationOptions& options)
{
    QJsonObject object;
    object["useNullptr"] = options.useNullptr;
    object["useUsingAliases"] = options.useUsingAliases;
    object["useSmartPointers"] = options.useSmartPointers;
    object["useMakeUnique"] = options.useMakeUnique;
    object["applySafeOwnershipModernization"] = options.applySafeOwnershipModernization;
    object["useStringView"] = options.useStringView;
    object["applyStringViewWhenSafe"] = options.applyStringViewWhenSafe;
    object["useStdFormatForStreams"] = options.useStdFormatForStreams;
    object["customInstruction"] = QString::fromStdString(options.customInstruction);
    return object;
}

QJsonObject BackendClient::changeToJson(const ConversionChange& change)
{
    QJsonObject object;
    object["ruleName"] = QString::fromStdString(change.ruleName);
    object["before"] = QString::fromStdString(change.before);
    object["after"] = QString::fromStdString(change.after);
    object["reason"] = QString::fromStdString(change.reason);
    object["applied"] = change.applied;
    object["skipped"] = change.skipped;
    return object;
}

QJsonObject BackendClient::resultToJson(const ConversionResult& result)
{
    QJsonObject object;
    object["modernCode"] = QString::fromStdString(result.modernCode);
    object["explanation"] = QString::fromStdString(result.explanation);
    QJsonArray changes;
    for (const ConversionChange& change : result.changes) {
        changes.append(changeToJson(change));
    }
    object["changes"] = changes;
    return object;
}

ConversionChange BackendClient::changeFromJson(const QJsonObject& object)
{
    ConversionChange change;
    change.ruleName = object.value("ruleName").toString().toStdString();
    change.before = object.value("before").toString().toStdString();
    change.after = object.value("after").toString().toStdString();
    change.reason = object.value("reason").toString().toStdString();
    change.applied = object.value("applied").toBool(false);
    change.skipped = object.value("skipped").toBool(false);
    return change;
}
