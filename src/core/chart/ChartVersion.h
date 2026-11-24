#pragma once

#include "core/chart/document/SimaiDocument.h"
#include "core/chart/model/TimelineData.h"
#include <QStringList>
#include <optional>

namespace miacode::chart_version {

enum class Family { Original = 1, Green, Orange, Pink, Murasaki, Milk, Finale, Dx, Splash,
    Universe, Festival, Buddies, Prism, Circle, LightDx, Gamble, Magical };

struct Version {
    Family family = Family::Original;
    bool plus = false;
    bool deluxe() const { return family >= Family::Dx; }
    QString outlineFileName() const;
    QString name() const;
    int officialSortKey() const;
};

std::optional<Version> namedVersion(QString token);
QStringList officialNames();
Version detect(const SimaiDocument& document, const QVector<TimelineNoteMarker>& markers);

} // namespace miacode::chart_version
