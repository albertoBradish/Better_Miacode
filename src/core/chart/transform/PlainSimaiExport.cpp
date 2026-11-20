#include "core/chart/transform/PlainSimaiExport.h"

#include "core/chart/document/SimaiDocument.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"

namespace miacode::chart_transform {

QString plainSimaiDocumentText(const SimaiDocument& document)
{
    SimaiDocument plain = document;
    for (int difficultyId : plain.difficultyIds()) {
        SimaiDifficultyData* difficulty = plain.difficulty(difficultyId);
        difficulty->chart = miacode::hand_order::stripAnnotations(difficulty->chart);
    }
    return plain.toText();
}

} // namespace miacode::chart_transform
