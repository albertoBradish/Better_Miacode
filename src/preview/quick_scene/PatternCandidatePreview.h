#pragma once

#include "preview/quick_scene/PreviewQuickSceneRoot.h"
#include "core/scene/PreviewFrameState.h"
#include <QVariant>

// Independent, silent miniature of the same native QSG scene used by preview
// and export. It has no playback, chart-editing, or analysis authority.
class PatternCandidatePreview : public PreviewQuickSceneRoot {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariant previewData READ previewData WRITE setPreviewData NOTIFY previewDataChanged)
    Q_PROPERTY(QObject* sourceRuntime READ sourceRuntime WRITE setSourceRuntime NOTIFY sourceRuntimeChanged)
    Q_PROPERTY(double second READ second WRITE setSecond NOTIFY secondChanged)
public:
    explicit PatternCandidatePreview(QQuickItem* parent = nullptr);
    QVariant previewData() const { return previewData_; }
    void setPreviewData(const QVariant& value);
    QObject* sourceRuntime() const;
    void setSourceRuntime(QObject* value);
    double second() const { return state_.playheadSeconds; }
    void setSecond(double value);
    const miacode::preview::scene::PreviewFrameState& frameState() const { return state_; }
signals:
    void previewDataChanged();
    void sourceRuntimeChanged();
    void secondChanged();
private:
    void refreshAssets();
    miacode::preview::scene::PreviewFrameState state_;
    QPointer<PreviewRuntime> source_;
    QMetaObject::Connection assetConnection_;
    QVariant previewData_;
};
