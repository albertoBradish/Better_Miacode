#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include "core/chart/document/SimaiDocument.h"

namespace miacode::metadata {
struct Field {
    QString key;
    const char* label;
    const char* group;
    bool multiline = false;
    const char* help = "metadata.reference.optional";
    QString example;
    QString unit;
    QStringList suggestions;
    bool readOnly = false;
};
QVector<Field> advancedFields(int difficultyId);
QString fieldValue(const SimaiDocument& document, const QString& key);
// Empty means valid. Optional empty fields remain absent rather than becoming zero.
QString validateField(const QString& key, const QString& value);
QStringList conflictingKeys(const QString& source);
} // namespace miacode::metadata
