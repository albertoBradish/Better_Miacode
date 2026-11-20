#pragma once

#include "app/services/ExportPagePort.h"

#include <QStringView>

namespace miacode::runtime::document_detail {

inline bool exportPageOwnsWorkspaceRevision(QStringView outlineKey, const ExportPagePort* page,
                                           bool documentIdentityChanged)
{
    // Normalization can publish a revision while the export session is active
    // but its audition has not installed the scene yet.
    return !documentIdentityChanged && outlineKey == u"export" && page != nullptr && page->pageSessionActive();
}

}  // namespace miacode::runtime::document_detail
