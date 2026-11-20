#include "preview/quick_scene/PatternCandidatePreview.h"
#include "preview/runtime/PreviewRuntime.h"
#include "core/chart/transform/HandOrderPatternRules.h"
#include <cmath>

using namespace miacode::preview::scene;

PatternCandidatePreview::PatternCandidatePreview(QQuickItem* parent) : PreviewQuickSceneRoot(parent)
{
    state_.render.showHandOrderAnnotations = true;
    setFrameState(&state_);
    setLayerFlags(BackdropLayer | GuideLayer | TrackLayer | SlideMotionLayer | HeadLayer | TouchLayer | TouchHoldLayer);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}
void PatternCandidatePreview::setPreviewData(const QVariant& value)
{
    const auto data = value.value<miacode::hand_order::PatternPreview>();
    previewData_ = value;
    state_.noteMarkers = data ? data->markers : QVector<TimelineNoteMarker>{};
    ++state_.sceneContentRevision; ++state_.sceneStorageGeneration;
    update(); emit previewDataChanged();
}
QObject* PatternCandidatePreview::sourceRuntime() const { return source_; }
void PatternCandidatePreview::setSourceRuntime(QObject* value)
{
    auto* source = qobject_cast<PreviewRuntime*>(value);
    if (source_ == source) return;
    disconnect(assetConnection_); source_ = source;
    if (source_) assetConnection_ = connect(source_, &PreviewRuntime::frameStateChanged, this, &PatternCandidatePreview::refreshAssets);
    refreshAssets(); emit sourceRuntimeChanged();
}
void PatternCandidatePreview::refreshAssets()
{
    if (!source_) return;
    const auto& source = source_->frameState();
    state_.skin = source.skin; state_.assets = source.assets;
    state_.render = source.render;
    state_.render.showHandOrderAnnotations = true;
    state_.render.showTimestamp = state_.render.showDebugInfo = false;
    state_.render.layoutSquareScale = 1.0;
    update();
}
void PatternCandidatePreview::setSecond(double value)
{
    if (!std::isfinite(value) || value == state_.playheadSeconds) return;
    state_.playheadSeconds = value; update(); emit secondChanged();
}
