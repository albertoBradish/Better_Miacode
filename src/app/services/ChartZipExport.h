#pragma once

#include "media_tools/zip_export/ChartZipPackager.h"
#include <QVariantMap>

namespace miacode::library {
zip_export::ChartZipResult exportChartZip(zip_export::ChartZipInput input, const QVariantMap& options,
    const zip_export::ChartZipProgressFn& progress = {});
}
