#pragma once

// Per-project (per-chart-folder) preferences persisted in the isolated
// hand-order runtime data directory. Use this for settings that
// are scoped to a single chart project (one maidata.txt + its assets) —
// e.g. "all difficulties share the same designer name" toggle.
//
// File layout: <runtime>/data/projects/<chart-folder-hash>/preferences.json
//
// All functions are no-ops / return empties when chartFilePath is empty
// or its parent directory doesn't exist; callers don't need to defend.
// Pure JSON-on-disk; no caching, no observer pattern. Read at load,
// write at save. Corrupt JSON is backed up before rebuilding an empty object;
// IO/backup failures preserve the original. Writes use atomic replacement.
// A read IO failure blocks saves to that file for this process lifetime, even
// after IO recovers, because callers may retain fallback compound settings.

#include <QJsonObject>
#include <QString>

namespace miacode::project_preferences {

QString projectPreferencesFilePath(const QString& chartFilePath);

QJsonObject load(const QString& chartFilePath);

bool save(const QString& chartFilePath, const QJsonObject& preferences);

}  // namespace miacode::project_preferences
