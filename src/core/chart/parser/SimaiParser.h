#pragma once

#include <QString>
#include <QVector>

#include "core/chart/document/SimaiTimingMetadata.h"
#include "core/chart/model/TimelineData.h"

struct SimaiMessage {
    int line = 1;
    int col = 1;
    int endCol = 1;
    QString message;
};

// UTF-16 token ranges produced by the native parser's lexer.
struct SimaiSourceToken {
    int line = 1;
    int col = 1;
    int length = 0;
    int firstParseOrder = 0;
    int markerCount = 0;
    bool nativeValid = false;
};

struct SimaiParseResult {
    bool ok = true;
    QVector<SimaiMessage> errors;
    // Annotation diagnostics do not change the validity of the base chart.
    QVector<SimaiMessage> annotationErrors;
    QVector<SimaiMessage> warnings;
    QVector<double> measureLineSeconds;
    QVector<double> textLineStartSeconds;
    QVector<TimelineBeatMarker> beatMarkers;
    QVector<TimelineTimingPoint> timingPoints;
    double endSecond = 0.0;
    double endQuarterBeat = 0.0;
    QVector<TimelineNoteMarker> noteMarkers;
    QVector<SimaiSourceToken> sourceTokens;
    double durationSeconds = 0.0;
};

enum class SimaiValidationLocale {
    English,
    Chinese,
    Japanese,
};

enum class SimaiValidationSeverity {
    Error,
    Warning,
};

struct SimaiValidationIssue {
    int line = 1;
    int col = 1;
    int endCol = 1;
    SimaiValidationSeverity severity = SimaiValidationSeverity::Error;
    bool isAnnotation = false;
    QString rawMessage;
    QString displayMessage;
};

struct SimaiValidationReport {
    bool ok = true;
    int errorCount = 0;
    int warningCount = 0;
    int strictNoteCount = 0;
    int strictErrorCount = 0;
    QVector<SimaiValidationIssue> issues;
};

class SimaiParser
{
public:
    // Shares the hold parser's duration grammar without parsing a chart.
    static bool isValidHoldDurationSignature(const QString& signature);
    static SimaiParseResult parseForTimeline(
        const QString& text,
        const miacode::simai::SimaiTimingMetadata& timingMetadata = miacode::simai::SimaiTimingMetadata());
    static SimaiParseResult validateSyntax(
        const QString& text,
        const miacode::simai::SimaiTimingMetadata& timingMetadata = miacode::simai::SimaiTimingMetadata());
    // Negative-HS compat switch (`<HS*-N>`). Default off → hs <= 0 rejected.
    static void setAllowNegativeHsEnabled(bool enabled);
    static bool allowNegativeHsEnabled();
    static void localizeValidationReport(SimaiValidationReport& report, SimaiValidationLocale locale);
    static SimaiValidationReport buildValidationReport(
        const QString& text,
        SimaiValidationLocale locale,
        const miacode::simai::SimaiTimingMetadata& timingMetadata = miacode::simai::SimaiTimingMetadata()
    );
};
