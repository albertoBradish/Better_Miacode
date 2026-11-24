#include "preview/stage_media/PreviewStageMediaHost.h"

#include "core/chart/ChartAssetPaths.h"
#include "common/DebugLog.h"
#include "common/DebugOptions.h"
#include "common/FileContentStamp.h"
#include "common/OperationLog.h"
#include "preview/stage_media/PreviewSharedD3D11Device.h"  // H2: single_device= log field

#include <cstdio>  // G2 Diag: std::snprintf for sync rate-change beacon lines

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>          // F1: DXGI adapter probe to auto-detect integrated GPUs
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")
#endif

// Windows preview decode backend: FFmpeg via QtAVPlayer. QT_AVPLAYER_MULTIMEDIA
// turns on the QAVVideoFrame -> QVideoFrame bridge (the conversion we feed to
// the QML VideoOutput sink). Still need QVideoFrame/QVideoSink for delivery.
#ifndef QT_AVPLAYER_MULTIMEDIA
#define QT_AVPLAYER_MULTIMEDIA
#endif
#include <QtAVPlayer/qavplayer.h>
#include <QtAVPlayer/qavvideoframe.h>
#include <QVideoFrame>
#include <QVideoSink>
#if defined(Q_OS_WIN)
#include <QtAVPlayer/qavd3d11sharedcontext_p.h>  // HW-decode diag counters / seek catch-up
#endif

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <QVariant>
#include <QtMath>
#include <utility>

#include "preview/stage_media/PreviewStageMediaHostInternal.h"

using namespace miacode::preview::psmh_detail;

namespace {

constexpr int kVideoFrameIntervalWindowSize = 120;

QVideoSink* videoSinkFor(QObject* output)
{
    return output ? qobject_cast<QVideoSink*>(output->property("videoSink").value<QObject*>()) : nullptr;
}

}  // namespace

PreviewStageMediaHost::PreviewStageMediaHost(QObject* parent)
    : QObject(parent)
{
    videoFrameIntervalsMs_.resize(kVideoFrameIntervalWindowSize);
    videoFrameIntervalsMs_.fill(0.0);
    pvMemoryElapsed_.start();
}

PreviewStageMediaHost::~PreviewStageMediaHost()
{
    QElapsedTimer timer;
    timer.start();
    const QString mediaType = debugMediaTypeName();
    const bool hadPlayer = player_ != nullptr;
    shutdownForAppExit();
    destroyPvMemorySource();
    miacode::debug_log::appendTimingLine(
        miacode::debug_log::Channel::Runtime,
        QStringLiteral("app_shutdown/stage_media_host"),
        QStringLiteral("destructor"),
        timer.elapsed(),
        QStringLiteral("had_player=%1 media_kind=%2")
            .arg(hadPlayer ? 1 : 0)
            .arg(mediaType)
    );
}

void PreviewStageMediaHost::shutdownForAppExit()
{
    shuttingDown_ = true;
    clearMedia();
    destroyPvMemorySource();
}

void PreviewStageMediaHost::attachVideoOutputObject(QObject* videoOutputObject)
{
    attachVideoOutputObjects(videoOutputObject, nullptr);
}

void PreviewStageMediaHost::attachVideoOutputObjects(QObject* videoOutputObject, QObject* innerVideoOutputObject)
{
    if (!videoOutputObject) return;
    for (const auto& binding : std::as_const(videoOutputs_)) {
        if (binding.output == videoOutputObject && binding.innerOutput == innerVideoOutputObject) return;
    }
    detachVideoOutputObjects(videoOutputObject, nullptr);
    videoOutputs_.removeIf([](const VideoOutputBinding& binding) { return binding.output.isNull(); });
    const VideoOutputBinding binding{videoOutputObject, innerVideoOutputObject,
        videoSinkFor(videoOutputObject), videoSinkFor(innerVideoOutputObject)};
    videoOutputs_.append(binding);
    if (lastVideoFrame_.isValid()) {
        if (binding.sink) binding.sink->setVideoFrame(lastVideoFrame_);
        if (backgroundScaleMode_ == PreviewBackgroundScaleMode::InnerCircleFitOuterFill
            && binding.innerSink && binding.innerSink != binding.sink) {
            binding.innerSink->setVideoFrame(lastVideoFrame_);
        }
    }
    recordPvMemoryBoundary(PvMemoryBoundary::OutputAttach);
}

void PreviewStageMediaHost::detachVideoOutputObject(QObject* videoOutputObject)
{
    detachVideoOutputObjects(videoOutputObject, nullptr);
}

void PreviewStageMediaHost::detachVideoOutputObjects(QObject* videoOutputObject, QObject* innerVideoOutputObject)
{
    if (!videoOutputObject) return;
    for (qsizetype i = 0; i < videoOutputs_.size(); ++i) {
        const auto& candidate = videoOutputs_.at(i);
        if (candidate.output != videoOutputObject
            || (innerVideoOutputObject && candidate.innerOutput != innerVideoOutputObject)) continue;
        const auto binding = videoOutputs_.takeAt(i);
        if (binding.sink) binding.sink->setVideoFrame(QVideoFrame());
        if (binding.innerSink && binding.innerSink != binding.sink)
            binding.innerSink->setVideoFrame(QVideoFrame());
        recordPvMemoryBoundary(PvMemoryBoundary::OutputDetach);
        return;
    }
}

bool PreviewStageMediaHost::hasVideoOutput() const
{
    for (const auto& binding : videoOutputs_) {
        if (binding.sink) return true;
    }
    return false;
}

void PreviewStageMediaHost::clearVideoOutputFrames()
{
    for (const auto& binding : std::as_const(videoOutputs_)) {
        if (binding.sink) binding.sink->setVideoFrame(QVideoFrame());
        if (binding.innerSink && binding.innerSink != binding.sink)
            binding.innerSink->setVideoFrame(QVideoFrame());
    }
}

void PreviewStageMediaHost::pushVideoOutputFrame(const QVideoFrame& frame)
{
    const bool useInner = backgroundScaleMode_ == PreviewBackgroundScaleMode::InnerCircleFitOuterFill;
    for (const auto& binding : std::as_const(videoOutputs_)) {
        if (binding.sink) binding.sink->setVideoFrame(frame);
        if (useInner && binding.innerSink && binding.innerSink != binding.sink)
            binding.innerSink->setVideoFrame(frame);
    }
}

bool PreviewStageMediaHost::mediaVisible() const
{
    return mediaVisible_;
}

void PreviewStageMediaHost::setMediaVisible(bool visible)
{
    if (mediaVisible_ == visible) {
        return;
    }
    mediaVisible_ = visible;
    emit mediaVisibilityChanged();
}

QUrl PreviewStageMediaHost::imageSource() const
{
    return imageSource_;
}

void PreviewStageMediaHost::setPlaybackTransactionId(quint64 transactionId)
{
    playbackTransactionId_ = transactionId;
}

QString PreviewStageMediaHost::debugMediaTypeName() const
{
    switch (mediaKind_) {
    case MediaKind::Image:
        return QStringLiteral("image");
    case MediaKind::Video:
        return QStringLiteral("video");
    case MediaKind::None:
    default:
        return QStringLiteral("none");
    }
}
