#pragma once

#include "core/scene/PreviewFrameState.h"
#include "core/scene/PreviewPreparedSceneCache.h"
class QSGNode;
class PreviewQuickVisualAidLayer {
public:
    QSGNode* updateNode(QSGNode* oldNode, const miacode::preview::scene::PreviewFrameState& state,
        const miacode::preview::scene::PreviewPreparedSceneCache* cache,
        const miacode::preview::scene::PreviewLayerWindowCursor* cursor, const QSize& renderSize, bool measuresOnly);
private:
    quint64 lastDiagnosedRevision_ = std::numeric_limits<quint64>::max();
};
