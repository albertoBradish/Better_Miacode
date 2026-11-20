#pragma once
#include "audio/PreviewAudioBackend.h"

namespace miacode::preview_audio::test {
class NullPreviewAudioBackend : public PreviewAudioBackend {
public:
    QString backendId() const override { return QStringLiteral("test-renderer"); }
    bool canBePrimary(QString*) const override { return true; }
    void setWarmupResolvedPaths(const QString&, const QString&, const QString&) override {}
    void reloadAssets(const PreviewAudioSettings&) override {}
    bool audioEngineInitialized() const override { return true; }
    void setChartPath(const QString&) override {}
    void setBackgroundTrackOffsetSeconds(double) override {}
    void setBackgroundTrackPlaybackRate(double) override {}
    void applyLevels(const PreviewAudioSettings&) override {}
    void configureTimeline(const QVector<TimelineNoteMarker>&, double, const PreviewTimingSettings&) override {}
    void clearTimeline() override {}
    void setPlaybackTransactionId(quint64) override {}
    double preparePreviewPlaybackTransaction(double second, bool, double) override { return second; }
    void commitPreparedPreviewPlayback() override {}
    void cancelPreparedPreviewPlayback() override {}
    double preparedStartSecond() const override { return 0.0; }
    void applyPausedPreviewState(const QVector<TimelineNoteMarker>&, bool, double, double, const PreviewTimingSettings&) override {}
    double startPreviewPlaybackTransaction(double second, bool, double) override { return second; }
    PausePreviewResult capturePausedPreviewTransaction() override { return {}; }
    PausePreviewResult pausePreviewPlaybackTransaction() override { return {}; }
    double resumeRetainedPreviewPlaybackTransaction() override { return 0.0; }
    double seekRetainedPreviewPlaybackTransaction(double second, bool) override { return second; }
    void resetRetainedPreviewPlaybackTransaction(double) override {}
    void clearRetainedPreviewPlaybackTransaction() override {}
    RetainedPlaybackMode retainedPlaybackMode() const override { return RetainedPlaybackMode::None; }
    RetainedBgmState retainedBgmState() const override { return RetainedBgmState::NoneLoaded; }
    double authoritativePlaybackSecond() const override { return 0.0; }
    double syncPreviewPlaybackClockTransaction(double second) override { return second; }
    void resetCursor(double, bool) override {}
    void drainEvents(double) override {}
    void pauseTouchholdVoices() override {}
    void restoreTouchholdVoices(double) override {}
    void syncBackgroundTrack(double) override {}
    bool hasBackgroundTrack() const override { return false; }
    bool isBackgroundTrackRunning() const override { return false; }
    void startBackgroundTrack(double) override {}
    void seekBackgroundTrack(double) override {}
    void pauseBackgroundTrack() override {}
    double backgroundPlaybackSecond() const override { return 0.0; }
    bool audition(const QString&, double) override { return true; }
    void stopAll() override {}
};
}
