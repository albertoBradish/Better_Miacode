#pragma once
#include "app/services/PreviewSurface.h"
#include "app/services/PlaybackControl.h"

namespace miacode::test {
inline QString rateLabelFor(double rate)
{
    QString text = QString::number(rate, 'f', 2);
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return QStringLiteral("%1x").arg(text);
}

// One rate lives behind both faces, the way the runtime has it: the control
// writes it, the surface reports it, and nothing signals in between.
struct FakeBackendRate {
    double rate = 1.0;
    int setCount = 0;
    int nudgeCount = 0;
    double position = 16.0;
    bool playing = false;
    QObject* runtime = nullptr;
    QObject* mediaHost = nullptr;
    QVariantMap renderSettings;
};

class FakeSurface final : public miacode::PreviewSurface
{
public:
    RenderMode mode_ = RenderMode::Native;
    explicit FakeSurface(FakeBackendRate& shared) : shared_(shared) {}

    bool playing() const override { return shared_.playing; }
    miacode::PlaybackTransportState playbackTransportState() const override
    {
        return miacode::PlaybackTransportState::Paused;
    }
    double positionSeconds() const override { return shared_.position; }
    double durationSeconds() const override { return 164.0; }
    double lowerBoundSeconds() const override { return 0.0; }
    double playbackRate() const override { return shared_.rate; }
    QString playbackRateLabel() const override { return rateLabelFor(shared_.rate); }
    QObject* previewRuntimeObject() const override { return shared_.runtime; }
    QObject* stageMediaHostObject() const override { return shared_.mediaHost; }
    double canvasAspectRatio() const override { return 1.0; }
    QStringList statsTexts() const override { return {}; }
    RenderMode muriRenderMode() const override { return mode_; }
    void setMuriRenderMode(RenderMode mode) override { mode_ = mode; }
    void toggleMuriRenderMode() override {}
    int muriHandRadiusPx() const override { return 30; }
    void setMuriHandRadiusPx(int) override {}
    int muriTapOnSlideThresholdMs() const override { return 200; }
    void setMuriTapOnSlideThresholdMs(int) override {}
    QStringList availableSkinDirectoryNames() const override { return {}; }
    QString skinDisplayName(const QString&) const override { return {}; }
    QString resolveSkinDir() const override { return {}; }
    QString resolveSkinRootDir() const override { return {}; }
    QString resolveCustomOutlineDir() const override { return {}; }
    QStringList availableCustomOutlineFileNames() const override { return {}; }
    QString customOutlineDisplayName(const QString&) const override { return {}; }
    QString currentCustomOutlineFileName() const override { return {}; }
    void applyOutlineVariant(PreviewOutlineVariant, bool, bool) override {}
    void applyCustomOutlineFileName(const QString&, bool) override {}
    QVariantMap renderSettings() const override { return shared_.renderSettings; }
    void setRenderSetting(const QString&, const QVariant&) override {}
    void refreshSurfaces() override {}
    void applySfxLevels() override {}
    void prepareForShutdown() override {}
    PreviewAudioSettings audioSettings() const override { return {}; }
    void applyAudioSettings(const PreviewAudioSettings&) override {}
    void saveAudioSettingsAsSoftwareDefault() override {}
    void restoreAudioSettingsFromSoftwareDefault() override {}

private:
    FakeBackendRate& shared_;
};

class FakeControl final : public miacode::PlaybackControl
{
public:
    explicit FakeControl(FakeBackendRate& shared) : shared_(shared) {}

    miacode::PlaybackSnapshot playbackSnapshot() const override { return {}; }
    bool acceptsPlaybackCallback(const miacode::PlaybackCallbackStamp&) const override
    {
        return true;
    }
    void togglePlayback() override { shared_.playing = !shared_.playing; }
    void stopOrPlay() override {}
    void setPlaybackRangeEnabled(bool, double, double) override {}
    void stop() override {}
    void seek(double value) override { shared_.position = value; }
    void beginScrub() override {}
    void updateScrub(double) override {}
    void endScrub(double) override {}
    void setPlaybackRate(double rate) override
    {
        shared_.rate = qMax(0.25, rate);
        ++shared_.setCount;
    }
    // The same ladder runtime/Shared.cpp steps through.
    void nudgePlaybackRate(int direction) override
    {
        static const QList<double> ladder{0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
        int nearest = 0;
        for (int i = 1; i < ladder.size(); ++i) {
            if (qAbs(ladder.at(i) - shared_.rate) < qAbs(ladder.at(nearest) - shared_.rate))
                nearest = i;
        }
        shared_.rate = ladder.at(qBound(0, nearest + direction, ladder.size() - 1));
        ++shared_.nudgeCount;
    }

private:
    FakeBackendRate& shared_;
};

} // namespace miacode::test
