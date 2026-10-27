#include "core/chart/SlideJudgeData.h"
#include "core/chart/SlideReferenceData.h"

#include <QJsonArray>

namespace miacode::slide_judge {
namespace {
Areas readAreas(const QJsonArray& source)
{
    Areas result;
    for (const QJsonValue& area : source) {
        if (!area.isArray()) return {};
        QStringList pads;
        for (const QJsonValue& pad : area.toArray()) {
            if (!pad.isString() || pad.toString().isEmpty()) return {};
            pads.append(pad.toString());
        }
        if (pads.isEmpty()) return {};
        result.append(pads);
    }
    return result;
}
}

Areas slideAreas(const QString& segmentKey)
{
    return readAreas(slide_reference::root().value(QStringLiteral("slides")).toObject()
        .value(segmentKey).toObject().value(QStringLiteral("judge_sequence")).toArray());
}

QVector<Areas> wifiLanes(const QString& trackKey)
{
    QVector<Areas> result;
    const QJsonArray lanes = slide_reference::root().value(QStringLiteral("wifi")).toObject()
        .value(trackKey).toObject().value(QStringLiteral("tri_judge_sequence")).toArray();
    for (const QJsonValue& lane : lanes) {
        const Areas areas = readAreas(lane.toArray());
        if (areas.isEmpty()) return {};
        result.append(areas);
    }
    if (result.size() != 3 || result.at(0).size() != result.at(1).size()
        || result.at(2).size() != result.at(1).size()) return {};
    return result;
}
}
