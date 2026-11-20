#pragma once

#include <QString>

class SimaiDocument;

namespace miacode::chart_transform {

// Returns a serialized copy of the document with chart annotations removed.
// The source document and its save state remain untouched.
QString plainSimaiDocumentText(const SimaiDocument& document);

} // namespace miacode::chart_transform
