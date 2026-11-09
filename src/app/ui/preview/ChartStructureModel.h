#pragma once

#include "core/analysis/ChartStructureAnalysis.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class TimelineQuickModel;

namespace miacode {
class ChartWorkspace;
class ChartWorkspaceFileService;
}

namespace miacode::ui {

class ChartStructureModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool pressureAvailable READ pressureAvailable NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool dropAvailable READ dropAvailable NOTIFY changed)
    Q_PROPERTY(double dropStartSeconds READ dropStartSeconds NOTIFY changed)
    Q_PROPERTY(double dropEndSeconds READ dropEndSeconds NOTIFY changed)
    Q_PROPERTY(double mediaDuration READ mediaDuration NOTIFY changed)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    ChartStructureModel(ChartWorkspace& workspace, ChartWorkspaceFileService& files, QObject* parent = nullptr);
    ~ChartStructureModel() override;

    bool available() const;
    bool pressureAvailable() const;
    bool pending() const;
    bool dropAvailable() const;
    double dropStartSeconds() const;
    double dropEndSeconds() const;
    double mediaDuration() const;
    QVariantList candidates() const;
    bool canUndo() const;
    bool canRedo() const;
    bool unsaved() const;
    QString status() const;

    std::optional<chart_preview::DropSelection> effectiveDrop() const;
    std::optional<chart_preview::DropSelection> effectiveDropForDifficulty(int difficultyId) const;
    QString inputFingerprintForDifficulty(int difficultyId) const;
    int activeDifficultyId() const { return input_.difficulty; }
    QString audioFingerprint() const;
    QString inputFingerprint() const;
    void setPlaying(bool playing);
    void setLowPerformanceMode(bool enabled);
    void setPlaybackDuration(double seconds);

    Q_INVOKABLE QVariantList heatmapSamples(double start, double end, int columns) const;
    Q_INVOKABLE QVariantMap pressureAt(double seconds) const;
    Q_INVOKABLE bool setManualDrop(double start, double end);
    Q_INVOKABLE bool canSetDropFromEditorSelection(int difficulty, qulonglong revision, qulonglong generation,
        const QString& text, int start, int end);
    Q_INVOKABLE bool setDropFromEditorSelection(int difficulty, qulonglong revision, qulonglong generation,
        const QString& text, int start, int end);
    Q_INVOKABLE bool clearDrop();
    Q_INVOKABLE bool saveSessionDrop();
    Q_INVOKABLE void undoDrop();
    Q_INVOKABLE void redoDrop();
    Q_INVOKABLE void startAnalysis();
    Q_INVOKABLE void cancelAnalysis();

signals:
    void changed();

private:
    using Selection = std::optional<chart_preview::DropSelection>;
    struct Input {
        quint64 generation = 0;
        quint64 revision = 0;
        int difficulty = 0;
        QString chartPath;
        QString chart;
        simai::SimaiTimingMetadata timing;
        double first = 0.0;
        bool allowNegativeHs = false;
        QString chartFingerprint;
        QString audioPath;
        qint64 audioSize = -1;
        qint64 audioModified = -1;

        bool hasSameContent(const Input& other) const;
    };
    struct Result {
        chart_structure::PressureCurve pressure;
        QVector<chart_structure::DropCandidate> candidates;
        QString fingerprint;
        QString audioHash;
        double audioDuration = 0.0;
        bool audioAvailable = false;
        bool validChart = false;
        bool staleAudio = false;
    };
    struct Edit {
        Selection before;
        Selection after;
    };
    struct EditState {
        QVector<Edit> undo;
        QVector<Edit> redo;
        Selection overrideValue;
        bool hasOverride = false;
        bool suppressAuto = false;
        bool unsaved = false;
    };

    void refresh(bool audioChanged = false);
    void schedule();
    void run();
    void stopCurrentTask();
    static Result calculate(const Input& input, const QString& cacheRoot, const std::atomic_bool& cancelled);
    void acceptResult(const Input& input, quint64 sequence, Result result);
    bool persist(Selection selection, bool explicitSave = false);
    bool applyEdit(Selection selection);
    void restoreEdit(Selection selection);
    Selection currentSelection() const;
    std::optional<chart_preview::MediaRange> dropRangeForEditorSelection(int difficulty, quint64 revision,
        quint64 generation, const QString& text, int start, int end);
    void updateWatches();

    ChartWorkspace& workspace_;
    ChartWorkspaceFileService& files_;
    Input input_;
    std::optional<Result> result_;
    std::unique_ptr<TimelineQuickModel> editorDropTimeline_;
    QHash<int, EditState> edits_;
    QString attemptedAutoFingerprint_;
    QThreadPool pool_;
    QTimer debounce_;
    QFileSystemWatcher watcher_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    quint64 sequence_ = 0;
    double playbackDuration_ = 0.0;
    bool playing_ = false;
    bool lowPerformanceMode_ = false;
    bool cancelled_ = false;
    bool applying_ = false;
};

} // namespace miacode::ui
