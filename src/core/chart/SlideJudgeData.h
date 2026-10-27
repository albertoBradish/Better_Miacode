#pragma once

#include <QStringList>
#include <QVector>

// The actual judge-pad groups, shared by analysis and visual aids. A group
// contains alternative pads; its entries are not successive checkpoints.
namespace miacode::slide_judge {
using Areas = QVector<QStringList>;
Areas slideAreas(const QString& segmentKey);
QVector<Areas> wifiLanes(const QString& trackKey);
}
