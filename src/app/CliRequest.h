#pragma once

#include <QJsonObject>
#include <QStringList>

namespace miacode::app::entry {
struct CliRequest {
    QJsonObject values;
    QString error;
    bool json = false;
    bool ndjson = false;
    bool metadataOnly() const;
    bool needsGui() const;
};
bool isPublicCliInvocation(const QStringList& arguments);
CliRequest parseCliRequest(const QStringList& arguments, const QByteArray* suppliedRequest = nullptr);
}
