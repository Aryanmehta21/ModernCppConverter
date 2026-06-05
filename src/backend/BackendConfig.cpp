#include "backend/BackendConfig.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

BackendConfig BackendConfig::loadFromFile(const QString& path)
{
    BackendConfig config;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return config;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return config;
    }

    const QJsonObject object = document.object();
    config.backendUrl = object.value("backendUrl").toString(config.backendUrl);
    config.requestTimeoutMs = object.value("requestTimeoutMs").toInt(config.requestTimeoutMs);
    return config;
}
