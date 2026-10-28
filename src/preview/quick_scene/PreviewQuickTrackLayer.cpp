#include "preview/quick_scene/PreviewQuickTrackLayer.h"

#include "preview/quick_scene/PreviewQuickSpriteNodes.h"
#include "preview/quick_scene/PreviewTextureRepository.h"
#include "core/scene/PreviewPreparedSceneCache.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "core/scene/PreviewTrackLayerState.h"
#include "common/DebugLog.h"

QSGNode* PreviewQuickTrackLayer::updateNode(
    QSGNode* oldNode,
    const miacode::preview::scene::PreviewFrameState& state,
    const miacode::preview::scene::PreviewPreparedSceneCache* preparedCache,
    const miacode::preview::scene::PreviewLayerWindowCursor* cursor,
    const QSize& renderSize,
    QQuickWindow* window,
    PreviewTextureRepository* textures
) const
{
    const miacode::preview::scene::PreviewActiveMarkerView activeMarkers =
        preparedCache != nullptr && cursor != nullptr
        ? miacode::preview::scene::PreviewActiveMarkerView(state.noteMarkers, preparedCache->slideLikeLayer(), *cursor)
        : miacode::preview::scene::PreviewActiveMarkerView(state.noteMarkers);
    const auto layerState = miacode::preview::scene::buildPreviewTrackLayerState(
        state,
        activeMarkers,
        miacode::preview::scene::playfieldRectForStage(
            miacode::preview::scene::stageRectForSize(renderSize),
            state.render.layoutSquareScale
        )
    );
    if (layerState.missingSlideTimingCount > 0 && lastDiagnosedRevision_ != state.sceneContentRevision) {
        lastDiagnosedRevision_ = state.sceneContentRevision;
        miacode::debug_log::appendLine(miacode::debug_log::Channel::Runtime,
            QStringLiteral("preview/slide_timing"), QStringLiteral("action=skip_invalid_reference count=%1")
                .arg(layerState.missingSlideTimingCount));
    }
    return buildPreviewSpriteNodeTree(oldNode, layerState.sprites, window, textures, state.playheadSeconds, "track");
}
