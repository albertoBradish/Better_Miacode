#pragma once

#include <QStringView>

namespace miacode::hand_order {

// Lower is stronger. An unlisted rule cannot claim an overlapping phrase.
int handOrderPatternPriority(QStringView id);

} // namespace miacode::hand_order
