#pragma once

#include <QString>

#include "core/chart/model/TimelineData.h"

namespace miacode::hand_order {

struct TokenParts {
    QString baseToken;
    QString suffix;
    int suffixOffset = -1;
};

// Splits at the first annotation delimiter, including malformed suffixes.
TokenParts splitToken(const QString& token);

struct Annotation {
    bool valid = false;
    int startOffset = -1;
    QString error;
    HandOrderHand head = HandOrderHand::Unspecified;
    QVector<QVector<HandOrderTrackStop>> branchTracks;
    QVector<HandOrderWifiMode> wifiModes;
    QVector<QVector<QVector<HandOrderTrackStop>>> wifiSectors;
};

// Parses only the suffix; callers validate its modes against native branches.
Annotation parseAnnotation(const QString& token, bool isSlide);

// Removes token annotations while retaining all other source characters.
QString stripAnnotations(const QString& chartText);

} // namespace miacode::hand_order
