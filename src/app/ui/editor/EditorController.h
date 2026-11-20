#pragma once

#include "editor/SimaiTextEditPolicy.h"
#include "core/chart/transform/HandOrderEdit.h"
#include "core/chart/transform/ChartNoteSelection.h"
#include "core/chart/model/HandOrderSweepThreshold.h"
#include "core/chart/transform/NativeSlideSplit.h"
#include "app/services/ChartWorkspace.h"
#include "common/LocalizedText.h"

#include <QObject>
#include <QString>
#include <QHash>
#include <QVariantList>

namespace miacode::ui {
struct NativeSlideSplitContext {
    ChartWorkspaceSnapshot workspace;
    QString scope;
    QString chartText;
    miacode::simai::SimaiTimingMetadata timing;
};

// DSL smart input, completion catalog and bookmark comment commands.
class EditorController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool handOrderFeaturesEnabled READ handOrderFeaturesEnabled NOTIFY handOrderChanged)
    Q_PROPERTY(bool halfWidthInputEnabled READ halfWidthInputEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool overwriteMode READ overwriteMode NOTIFY settingsChanged)
    Q_PROPERTY(bool autoCompletionEnabled READ autoCompletionEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool imeInputDisabled READ imeInputDisabled NOTIFY settingsChanged)
    Q_PROPERTY(QString wholeBpm READ wholeBpm NOTIFY settingsChanged)
    Q_PROPERTY(bool completionActive READ completionActive NOTIFY completionChanged)
    Q_PROPERTY(QStringList completionCandidates READ completionCandidates NOTIFY completionChanged)
    Q_PROPERTY(int completionIndex READ completionIndex NOTIFY completionChanged)
    Q_PROPERTY(QString completionGroup READ completionGroup NOTIFY completionChanged)
    Q_PROPERTY(QVariantList handOrderRows READ handOrderRows NOTIFY handOrderChanged)
    Q_PROPERTY(bool handOrderSplitAvailable READ handOrderSplitAvailable NOTIFY handOrderChanged)
    Q_PROPERTY(bool handOrderCanApply READ handOrderCanApply NOTIFY handOrderChanged)
    Q_PROPERTY(QString handOrderFeedback READ handOrderFeedback NOTIFY handOrderChanged)
    Q_PROPERTY(bool nativeSlideSplitAvailable READ nativeSlideSplitAvailable NOTIFY nativeSlideSplitChanged)
    Q_PROPERTY(bool nativeSlideSplitCanApply READ nativeSlideSplitCanApply NOTIFY nativeSlideSplitChanged)
    Q_PROPERTY(QVariantList nativeSlideSplitRows READ nativeSlideSplitRows NOTIFY nativeSlideSplitChanged)
    Q_PROPERTY(QString nativeSlideSplitMessage READ nativeSlideSplitMessage NOTIFY nativeSlideSplitChanged)

public:
    explicit EditorController(QObject* parent = nullptr);

    bool halfWidthInputEnabled() const;
    bool overwriteMode() const;
    bool autoCompletionEnabled() const;
    bool imeInputDisabled() const;
    QString wholeBpm() const;
    bool completionActive() const;
    QStringList completionCandidates() const;
    int completionIndex() const;
    QString completionGroup() const;

    void setHalfWidthInputEnabled(bool enabled);
    void setOverwriteMode(bool enabled);
    void setAutoCompletionEnabled(bool enabled);
    void setCompletionConfig(const miacode::editor::SimaiCompletionConfig& config);
    void setImeInputDisabled(bool disabled);
    void setWholeBpm(const QString& bpm);
    bool handOrderFeaturesEnabled() const { return handOrderFeaturesEnabled_; }
    void setHandOrderFeaturesEnabled(bool enabled);
    void setHandOrderReturnSpeed(double speed) { handOrderReturnSpeed_ = speed; }
    void setHandOrderSweepThreshold(int threshold) { handOrderSweepThreshold_ = threshold; }
    void setHandOrderColorUnresolved(bool enabled) { handOrderColorUnresolved_ = enabled; }

    miacode::editor::SimaiTextEditResult processKey(
        const QString& text, int anchor, int position, const QString& input, int key, int modifiers);
    miacode::editor::SimaiTextEditResult acceptCompletion(const QString& text, int anchor, int position);
    void setDifficulty(int difficultyId);
    void triggerCompletion(QChar glyph, const QString& text, int position, bool closingPresent);
    void activateHandOrderDocument(const QString& scope, const QString& text);
    miacode::editor::SimaiTextEditResult handOrderCommand(
        const QString& text, int anchor, int position, const QString& command,
        const miacode::simai::SimaiTimingMetadata& metadata = {});
    miacode::editor::SimaiTextEditResult alternateHandOrderSelection(const QString& text,
        const miacode::chart_selection::NoteRanges& ranges,
        const miacode::simai::SimaiTimingMetadata& metadata = {});
    miacode::editor::SimaiTextEditResult screenSplitHandOrderSelection(const QString& text,
        const miacode::chart_selection::NoteRanges& ranges,
        const miacode::simai::SimaiTimingMetadata& metadata = {});
    miacode::editor::SimaiTextEditResult applyHandOrderIntervals(const QString& text);
    QVariantList handOrderRows() const;
    bool handOrderCanApply() const;
    bool handOrderSplitAvailable() const { return handOrderSplitAvailable_; }
    QString handOrderFeedback() const { return handOrderFeedback_.text(); }
    void retranslate();
    Q_INVOKABLE void setHandOrderInterval(int row, int hand);
    Q_INVOKABLE void setHandOrderWifiMode(int row, int mode);
    void updateNativeSlideSplitContext(const NativeSlideSplitContext& context);
    bool prepareNativeSlideSplit(const NativeSlideSplitContext& context, int anchor, int position);
    miacode::editor::SimaiTextEditResult applyNativeSlideSplit(const NativeSlideSplitContext& context);
    Q_INVOKABLE void cancelNativeSlideSplit();
    bool nativeSlideSplitAvailable() const { return nativeSlideSplitAvailable_; }
    bool nativeSlideSplitCanApply() const;
    QVariantList nativeSlideSplitRows() const;
    QString nativeSlideSplitMessage() const;

    miacode::editor::SimaiTextEditResult createBookmark(const QString& lineText, int offset, const QString& title) const;
    miacode::editor::SimaiTextEditResult renameBookmark(const QString& lineText, int offset, const QString& title) const;
    miacode::editor::SimaiTextEditResult deleteBookmark(const QString& lineText, int offset) const;
    miacode::editor::SimaiTextEditResult touchPadAuthoring(const QString& text, int anchor,
                                                        int position, const QString& pad, QChar separator) const;
    void updateCompletion(const QString& text, int position);
    Q_INVOKABLE void moveCompletionSelection(int delta);
    Q_INVOKABLE void selectCompletionIndex(int index);
    Q_INVOKABLE void closeCompletion();

signals:
    void settingsChanged();
    void completionChanged();
    void handOrderChanged();
    void nativeSlideSplitChanged();

private:
    miacode::editor::SimaiTextEditResult process(const miacode::editor::SimaiTextEditRequest& request);
    void setCompletion(const miacode::editor::SimaiCompletionSession& completion);
    void filterCompletion(const QString& text, int position);
    bool halfWidthInputEnabled_ = true;
    bool overwriteMode_ = false;
    bool autoCompletionEnabled_ = true;
    bool imeInputDisabled_ = true;
    QString wholeBpm_;
    miacode::editor::SimaiCompletionConfig completionConfig_ = miacode::editor::defaultCompletionConfig();
    miacode::editor::SimaiCompletionSession completion_;
    QStringList visibleCandidates_;
    int completionReplacementOffset_ = 0;
    int completionIndex_ = -1;
    int activeDifficultyId_ = -1;
    bool handOrderFeaturesEnabled_ = false;
    QString handOrderScope_;
    double handOrderReturnSpeed_ = 3.0;
    int handOrderSweepThreshold_ = chart::handOrderDefaultSweepThreshold;
    bool handOrderColorUnresolved_ = false;
    QString handOrderText_;
    QString handOrderKey_;
    miacode::LocalizedText handOrderFeedback_;
    bool handOrderSplitAvailable_ = false;
    QHash<QString, QVector<miacode::chart_transform::HandOrderTokenEdit>> handOrderDrafts_;
    QHash<QString, QVector<int>> handOrderVisibleStarts_;
    QStringList handOrderDraftOrder_;
    NativeSlideSplitContext nativeSlideSplitContext_, preparedNativeSlideSplitContext_;
    miacode::chart_transform::NativeSlideSplitPlan nativeSlideSplitPlan_;
    bool nativeSlideSplitAvailable_ = false, nativeSlideSplitStale_ = false;
    bool nativeSlideSplitAllowNegativeHs_ = false;
    int nativeSlideSplitBegin_ = 0, nativeSlideSplitEnd_ = 0;
    void rememberHandOrderDraft(const QString& text,
        const QVector<miacode::chart_transform::HandOrderTokenEdit>& tokens,
        const QVector<miacode::chart_transform::HandOrderEditPlan::Patch>& patches = {});
};

} // namespace miacode::ui
