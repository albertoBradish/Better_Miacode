#include "core/chart/transform/HandOrderPatternPriority.h"

#include <QFile>
#include <QHash>
#include <limits>

namespace miacode::hand_order {

int handOrderPatternPriority(QStringView id)
{
    static const auto priorities = [] {
        QHash<QString, int> result;
        QFile table(QStringLiteral(":/data/hand_order_solver_priority.csv"));
        if (!table.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
        table.readLine();
        while (!table.atEnd()) {
            const auto row = table.readLine().trimmed().split(',');
            if (row.size() != 2) continue;
            bool ok = false;
            const int priority = row.last().toInt(&ok);
            if (ok) result.insert(QString::fromLatin1(row.first()), priority);
        }
        return result;
    }();
    return priorities.value(id.toString(), std::numeric_limits<int>::max());
}

} // namespace miacode::hand_order
