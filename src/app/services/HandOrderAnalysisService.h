#pragma once
#include "app/services/AnalysisService.h"
#include "core/analysis/hand_order/BasicHandOrder.h"
#include "editor/SimaiTextEditPolicy.h"
#include "app/services/JobProgressService.h"
#include <QPointer>
#include <QVariantList>
#include <QThreadPool>
#include <QTimer>
#include <QHash>
#include <QSet>
#include "common/LocalizedText.h"

namespace miacode {
class HandOrderExperimentService;
class HandOrderAnalysisService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool featuresEnabled READ featuresEnabled NOTIFY changed)
    Q_PROPERTY(QObject* experiments READ experiments CONSTANT)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool overwriteExisting READ overwriteExisting WRITE setOverwriteExisting NOTIFY changed)
    Q_PROPERTY(bool highConfidenceOnly READ highConfidenceOnly WRITE setHighConfidenceOnly NOTIFY changed)
    Q_PROPERTY(int inputPreference READ inputPreference WRITE setInputPreference NOTIFY changed)
    Q_PROPERTY(bool colorUnresolved READ colorUnresolved WRITE setColorUnresolved NOTIFY changed)
    Q_PROPERTY(bool optimizingPatterns READ optimizingPatterns NOTIFY changed)
    Q_PROPERTY(bool reviewingPlay READ reviewingPlay NOTIFY changed)
    Q_PROPERTY(bool preservePatternAnnotations READ preservePatternAnnotations WRITE setPreservePatternAnnotations NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(bool backgroundActive READ backgroundActive NOTIFY changed)
    Q_PROPERTY(QString backgroundStatus READ backgroundStatus NOTIFY changed)
    Q_PROPERTY(QString backgroundDetail READ backgroundDetail NOTIFY changed)
    Q_PROPERTY(int backgroundPercent READ backgroundPercent NOTIFY changed)
    Q_PROPERTY(bool cacheHit READ cacheHit NOTIFY changed)
    Q_PROPERTY(QVariantList patternSuggestions READ patternSuggestions NOTIFY patternSuggestionsChanged)
    Q_PROPERTY(int patternCount READ patternCount NOTIFY patternSuggestionsChanged)
    Q_PROPERTY(bool patternChoicesEnabled READ patternChoicesEnabled NOTIFY changed)
public:
    HandOrderAnalysisService(ChartWorkspace& workspace, AnalysisService* analysis = nullptr, QObject* parent = nullptr);
    ~HandOrderAnalysisService() override;
    void setJobProgress(JobProgressService* progress);
    QObject* experiments() const;
    bool pending() const { return pending_ && !backgroundOnly_; }
    bool canApply() const;
    QVariantList rows() const;
    QVariantList patternSuggestions() const;
    int patternCount() const { return patternSuggestions_.size(); }
    bool patternChoicesEnabled() const;
    editor::SimaiTextEditResult takePatternEdit(int group, int variant, const QString& currentText);
    editor::SimaiTextEditResult takePatternBatchEdit(const QStringList& keys, const QString& choice, const QString& currentText);
    QVariantMap patternNavigationRange(int group, const QString& currentText) const;
    Q_INVOKABLE QVariantMap patternNavigationTarget(int group) const;
    Q_INVOKABLE void ignorePattern(int group);
    Q_INVOKABLE void ignorePatterns(const QStringList& keys);
    QString message() const { return message_.text(); }
    void retranslate();
    bool overwriteExisting() const { return parameters_.overwriteExisting; }
    bool highConfidenceOnly() const { return parameters_.highConfidenceOnly; }
    int inputPreference() const { return parameters_.inputPreference; }
    bool colorUnresolved() const { return parameters_.colorUnresolved; }
    bool optimizingPatterns() const { return optimizingPatterns_; }
    bool reviewingPlay() const { return reviewingPlay_; }
    bool preservePatternAnnotations() const { return preservePatternAnnotations_; }
    void setPreservePatternAnnotations(bool preserve);
    void setOverwriteExisting(bool value);
    void setHighConfidenceOnly(bool value);
    void setInputPreference(int value);
    void setReturnSpeed(double speed);
    void setSweepThreshold(int threshold);
    void setColorUnresolved(bool enabled);
    void setPlayerProfile(const hand_order::PlayerProfile& profile);
    void setMaxWorkUnits(qint64 units);
    bool featuresEnabled() const { return featuresEnabled_; }
    void setFeaturesEnabled(bool enabled);
    bool prepare(const QString& text, int begin, int end, bool optimizePatterns = false, bool reviewPlay = false);
    bool request(const QString& text, int begin, int end, bool optimizePatterns = false, bool reviewPlay = false);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    bool paused() const { return pauseRequested_; }
    bool backgroundActive() const { return backgroundOnly_ && pending_; }
    QString backgroundStatus() const { return backgroundStatus_.text(); }
    QString backgroundDetail() const { return backgroundDetail_.text(); }
    int backgroundPercent() const { return backgroundPercent_; }
    bool cacheHit() const { return cacheHit_; }
    quint64 computationCount() const { return computationCount_; }
    void setBackgroundEnabled(bool enabled);
    void setPlaybackActive(bool active);
    void refreshParserPolicy();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool requestCurrentPatterns();
    Q_INVOKABLE void showPatternPanel() { emit patternPanelRequested(); }
    Q_INVOKABLE void setBeamWidth(int width);
    void setParameters(const hand_order::BasicParameters& parameters);
    editor::SimaiTextEditResult takeEdit(const QString& currentText);
    Q_INVOKABLE QVariantMap navigationRange(int row, const QString& currentText) const;
signals:
    void changed();
    void patternSuggestionsChanged();
    void patternPanelRequested();
private:
    bool featuresEnabled_ = false;
    struct Request {
        ParsedChartSnapshot snapshot;
        ChartWorkspaceSnapshot identity;
        quint64 sequence = 0, parameterRevision = 0;
        int begin = 0, end = 0;
        hand_order::BasicParameters parameters;
        bool optimizePatterns = false, preservePatternAnnotations = false;
        bool reviewPlay = false;
        MuriRenderOptions playOptions;
        bool background = false;
        bool allowNegativeHs = false;
        QSet<QString> ignoredPatterns;
    };
    struct Cached { Request request; hand_order::BasicResult result; };
    bool isCurrent(const Request& request) const;
    bool identityCurrent(const Request& request) const;
    bool sameCalculation(const Request& a, const Request& b) const;
    std::optional<Request> makeRequest(const QString& text, int begin, int end, bool optimizePatterns, bool reviewPlay = false) const;
    void invalidate(const LocalizedText& message, bool clearCache = true);
    void scheduleBackground();
    void startBackground();
    void stopBackground();
    void beginForegroundProgress();
    void publishResult(Request request, hand_order::BasicResult result, bool fromCache = false);
    void remember(const Request& request, const hand_order::BasicResult& result);
    void retainPatternSuggestions(const Request& request, const hand_order::BasicResult& result);
    void refreshPatternSuggestions();
    void pruneIgnoredPatterns(hand_order::BasicResult& result, const QString& text) const;
    void resetPatternState();
    void dispatch();
    void reportProgress(const Request& request, const hand_order::HandOrderProgress& progress,
        int percent, qint64 remainingSeconds, const QVector<int>& stages = {});
    QPointer<JobProgressService> progress_;
    quint64 progressToken_ = 0;
    void endProgress();
    HandOrderExperimentService* experiments_ = nullptr;
    ChartWorkspace* workspace_;
    AnalysisService* analysis_;
    hand_order::BasicParameters parameters_;
    quint64 sequence_ = 0, parameterRevision_ = 1;
    bool pending_ = false, running_ = false, taken_ = false;
    bool optimizingPatterns_ = false, preservePatternAnnotations_ = false;
    bool reviewingPlay_ = false;
    LocalizedText message_;
    task::CancellationFlag cancellation_;
    std::shared_ptr<task::ExecutionControl> execution_;
    std::optional<Request> queued_, completed_;
    std::optional<Request> active_;
    QVector<Cached> cache_;
    QThreadPool worker_;
    QTimer idleTimer_;
    bool backgroundEnabled_ = false, playbackActive_ = false, backgroundOnly_ = false;
    bool pauseRequested_ = false, cacheHit_ = false;
    bool userStopped_ = false;
    bool parserPolicy_ = SimaiParser::allowNegativeHsEnabled();
    int backgroundPercent_ = -1;
    LocalizedText backgroundStatus_, backgroundDetail_;
    quint64 computationCount_ = 0;
    hand_order::BasicResult result_;
    QVector<hand_order::PatternSuggestion> patternSuggestions_;
    QString patternText_;
    ChartWorkspaceSnapshot patternIdentity_;
    simai::SimaiTimingMetadata patternTiming_;
    bool patternFullScan_ = false;
    bool patternManualReview_ = false;
    QSet<QString> ignoredPatternKeys_;
    QVector<hand_order::PatternSuggestion> ignoredPatternSuggestions_;
    struct PatternState {
        QVector<hand_order::PatternSuggestion> suggestions, ignored;
        QSet<QString> ignoredKeys;
        QString text;
        ChartWorkspaceSnapshot identity;
        simai::SimaiTimingMetadata timing;
        bool fullScan = false, manualReview = false;
    };
    QHash<int, PatternState> patternStates_;
    quint64 patternInstanceSequence_ = 0;
};
} // namespace miacode
