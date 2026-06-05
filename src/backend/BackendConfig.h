#pragma once

#include <QString>

struct BackendConfig
{
    QString backendUrl = "http://localhost:8000";
    int requestTimeoutMs = 30000;

    [[nodiscard]] static BackendConfig loadFromFile(const QString& path);
};
