#pragma once
#include "app/services/ChartWorkspace.h"
#include "common/TaskCancellation.h"
#include "preview/runtime/PreviewRuntime.h"
#include "preview/stage_media/PreviewStageMediaHost.h"
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include "audio/QtPreviewSfxRuntime.h"

#include <QTimer>

namespace miacode::ui {
class PreviewModel;
class ComparisonPreviews;

struct PreviewSong {
    QString chartPath;
    QString trackPath;
    QString trackKey;
    quint64 generation = 0;
};

class ComparisonPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString panelId READ panelId CONSTANT)
    Q_PROPERTY(QObject* runtime READ runtime CONSTANT)
    Q_PROPERTY(QObject* mediaHost READ mediaHost NOTIFY mediaHostChanged)
    Q_PROPERTY(QString title READ title NOTIFY chartChanged)
    Q_PROPERTY(QString workspaceLabel READ workspaceLabel CONSTANT)
    Q_PROPERTY(bool surfaceVisible READ surfaceVisible WRITE setSurfaceVisible NOTIFY surfaceVisibleChanged)
    Q_PROPERTY(double duration READ duration NOTIFY transportChanged)
    Q_PROPERTY(double lowerBound READ lowerBound NOTIFY transportChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
    Q_PROPERTY(bool syncEnabled READ syncEnabled WRITE setSyncEnabled NOTIFY transportChanged)
    Q_PROPERTY(bool linked READ linked NOTIFY transportChanged)
public:
    ~ComparisonPreview() override;
    QObject* runtime() { return &scene_; }
    QObject* mediaHost() const { return mediaSource_; }
    QString panelId() const { return panelId_; }
    QString title() const { return title_; }
    QString workspaceLabel() const;
    bool surfaceVisible() const { return surfaceVisible_; }
    void setSurfaceVisible(bool visible);
    double duration() const { return displayedDuration_; }
    double lowerBound() const { return lowerBound_; }
    double position() const { return position_; }
    bool playing() const { return playing_; }
    bool syncEnabled() const { return syncEnabled_; }
    void setSyncEnabled(bool value);
    Q_INVOKABLE void setScreenPan(double pan);
    bool linked() const { return linked_; }
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void close();
signals:
    void chartChanged();
    void mediaHostChanged();
    void surfaceVisibleChanged();
    void positionChanged();
    void transportChanged();
private:
    friend class ComparisonPreviews;
    ComparisonPreview(ComparisonPreviews& owner, int difficulty, int workspaceNumber);
    void readChart(const SimaiDocument& document);
    void scheduleBuild();
    void startBuild();
    void inheritAppearance();
    void setMediaSource(PreviewStageMediaHost* source);
    void releaseOwnedMedia();
    void updateMediaPlayback(double second, bool playing, double rate, bool moved, bool jump);
    void present(double position, double duration, bool playing, double rate, bool jump);
    void warmAudio(bool retry = false);
    void setLocalClock(double second, bool running);
    double localPosition() const;

    struct Build {
        SimaiParseResult parsed;
        QVector<TimelineNoteMarker> markers;
        MuriAnalysisReport report;
    };
    ComparisonPreviews& owner_;
    const QString panelId_;
    const int difficulty_;
    const int workspaceNumber_;
    PreviewSong song_;
    QString title_, chart_;
    SimaiDocument versionMetadata_;
    QString automaticOutlinePath_;
    QString videoOverridePath_;
    QString outlinePath_;
    PreviewOutlineVariant outlineVariant_ = PreviewOutlineVariant::None;
    preview::runtime::PreviewOutlineImageMode outlineImageMode_ = preview::runtime::PreviewOutlineImageMode::Direct;
    std::optional<preview::scene::PreviewAssetState> outlineAssets_;
    simai::SimaiTimingMetadata timing_;
    double first_ = 0;
    int clockCount_ = 0;
    MuriRenderOptions options_;
    int thresholdMs_ = -1;
    quint64 revision_ = 0;
    task::CancellationFlag cancellation_;
    QFutureWatcher<Build> build_;
    QTimer debounce_;
    PreviewRuntime scene_;
    PreviewStageMediaHost media_;
    QPointer<PreviewStageMediaHost> mediaSource_;
    QMetaObject::Connection mediaStateConnection_, mediaVisibilityConnection_;
    bool mediaPlaying_ = false;
    bool mediaNeedsSync_ = false;
    bool surfaceVisible_ = false;
    std::unique_ptr<QtPreviewSfxRuntime> audio_;
    double pan_ = 0;
    bool syncEnabled_ = true, audioReady_ = false, audioFailed_ = false;
    bool audioAllowed_ = false, audioMixed_ = false, audioRetiring_ = false;
    quint64 audioRevision_ = 0, builtRevision_ = 0;
    quint64 outputFade_ = 0;
    PreviewAudioSettings appliedSettings_;
    PreviewTimingSettings appliedTiming_;
    ComparisonPreview* leader_ = nullptr; // null follows the main transport
    bool linked_ = false, playing_ = false, sounding_ = false;
    double position_ = 0, displayedDuration_ = 0, chartDuration_ = 0, trackDuration_ = 0, rate_ = 1;
    double clockStart_ = 0, lowerBound_ = 0;
    QElapsedTimer clock_, lastFrame_;
    bool building_ = false;
};

class ComparisonPreviews final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QVariantList panels READ panels NOTIFY countChanged)
    Q_PROPERTY(bool presentationActive READ presentationActive WRITE setPresentationActive NOTIFY presentationActiveChanged)
    Q_PROPERTY(bool mixAudio READ mixAudio WRITE setMixAudio NOTIFY mixAudioChanged)
    Q_PROPERTY(bool synchronize READ synchronize WRITE setSynchronize NOTIFY synchronizeChanged)
public:
    ComparisonPreviews(ChartWorkspace& workspace, PreviewModel& main, QObject* parent = nullptr);
    ~ComparisonPreviews() override;
    QVariantList panels() const;
    int count() const { return items_.size(); }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : count(); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    bool presentationActive() const { return presentationActive_; }
    void setPresentationActive(bool value);
    bool mixAudio() const { return mixAudio_; }
    void setMixAudio(bool value);
    Q_INVOKABLE void focusPanelAudio(const QString& panelId);
    Q_INVOKABLE void setMainPan(double pan);
    bool synchronize() const { return synchronize_; }
    void setSynchronize(bool value);
    Q_INVOKABLE void addCurrent();
signals:
    void countChanged();
    void mixAudioChanged();
    void synchronizeChanged();
    void presentationActiveChanged();
    void previewAdded();
private:
    friend class ComparisonPreview;
    PreviewSong identify(const ChartWorkspaceSnapshot& snapshot);
    bool sameSong(const PreviewSong& left, const PreviewSong& right) const;
    void refreshDocument();
    void refreshMediaPreference();
    void refreshMediaRoutes();
    void queueMediaRouteRefresh();
    void regroup();
    void updateClocks(ComparisonPreview* seekLeader = nullptr, bool seek = false);
    void remove(ComparisonPreview* item);
    void control(ComparisonPreview* item, double second, bool playing);
    ChartWorkspace& workspace_;
    PreviewModel& main_;
    QPointer<PreviewRuntime> mainScene_;
    QPointer<PreviewStageMediaHost> mainMedia_;
    PreviewSong current_;
    QList<ComparisonPreview*> items_;
    QHash<QString,QByteArray> digests_;
    QTimer tick_;
    void updateAudioRoutes();
    void applyAudioRoutes(bool focusedPlaying);
    void setMainOutputRoute(double gain, double pan);
    QTimer audioRoutePoll_;
    QList<std::shared_ptr<PreviewOutputRoute>> closingAudioRoutes_;
    QPointer<ComparisonPreview> focused_;
    bool mixAudio_ = false;
    double mainPan_ = 0;
    double mainOutputGain_ = -1, mainOutputPan_ = 0;
    bool synchronize_ = true;
    bool presentationActive_ = true;
    bool mainAvailable_ = false;
    bool showMediaWhenPaused_ = true;
    bool forceJudgeAreaWhenPaused_ = false;
    bool automaticOutline_ = false;
    PreviewOutlineVariant outlineVariant_ = PreviewOutlineVariant::Line;
    BackgroundPolicy backgroundPolicy_ = BackgroundPolicy::Normal;
    bool mediaReloadPending_ = false;
    bool mediaRouteRefreshQueued_ = false;
};
}
