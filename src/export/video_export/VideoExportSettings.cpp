#include "export/video_export/VideoExportSettings.h"

#include "export/video_export/VideoExportRuntimePolicy.h"

#include <QRegularExpression>
#include <QtNumeric>

namespace miacode::video_export {

namespace {

template <std::size_t N>
int closestOption(int requested, const std::array<int, N>& options, bool preferHigherOnTie)
{
    int closest = options.front();
    int closestDelta = qAbs(requested - closest);
    for (int candidate : options) {
        const int delta = qAbs(requested - candidate);
        if (delta < closestDelta || (preferHigherOnTie && delta == closestDelta && candidate > closest)) {
            closest = candidate;
            closestDelta = delta;
        }
    }
    return closest;
}

}  // namespace

int normalizedVideoExportAudioBitrateKbps(int requested)
{
    return closestOption(requested, kVideoExportAudioBitrateOptionsKbps, false);
}

int normalizedVideoExportFps(int requested)
{
    return closestOption(requested, kVideoExportFpsOptions, true);
}

QString videoExportPreferencePresetToken(VideoExportPreset preset)
{
    return preset == VideoExportPreset::Fast
        ? QStringLiteral("fast")
        : QStringLiteral("high_quality");
}

VideoExportPreset videoExportPresetFromPreference(const QJsonValue& value, VideoExportPreset fallback)
{
    if (!value.isString()) {
        return fallback;
    }
    const QString token = value.toString().trimmed();
    if (token.compare(QStringLiteral("high_quality"), Qt::CaseInsensitive) == 0
        || token.compare(QStringLiteral("high_compression"), Qt::CaseInsensitive) == 0) {
        return VideoExportPreset::HighQuality;
    }
    if (token.compare(QStringLiteral("fast"), Qt::CaseInsensitive) == 0) {
        return VideoExportPreset::Fast;
    }
    return fallback;
}

VideoExportSizePreset videoExportSizePresetFromPreference(
    const QJsonValue& value,
    VideoExportSizePreset fallback)
{
    const QString token = value.toString().trimmed();
    if (token.compare(QStringLiteral("target_30mb"), Qt::CaseInsensitive) == 0) return VideoExportSizePreset::Target30Mb;
    if (token.compare(QStringLiteral("target_100mb"), Qt::CaseInsensitive) == 0) return VideoExportSizePreset::Target100Mb;
    if (token.compare(QStringLiteral("target_200mb"), Qt::CaseInsensitive) == 0) return VideoExportSizePreset::Target200Mb;

    if (token.compare(QStringLiteral("compact"), Qt::CaseInsensitive) == 0) {
        return VideoExportSizePreset::Compact;
    }
    if (token.compare(QStringLiteral("ultra_compact"), Qt::CaseInsensitive) == 0) {
        return VideoExportSizePreset::UltraCompact;
    }
    if (token.compare(QStringLiteral("ultra_compact_with_pv"), Qt::CaseInsensitive) == 0) {
        return VideoExportSizePreset::UltraCompactWithPv;
    }
    if (token.compare(QStringLiteral("standard"), Qt::CaseInsensitive) == 0) {
        return VideoExportSizePreset::Standard;
    }
    return fallback;
}

void applyVideoExportPreferences(const QJsonObject& settings, VideoExportTask* task)
{
    if (task == nullptr) {
        return;
    }
    const int width = settings.value(QStringLiteral("resolution_width")).toInt(task->outputWidth);
    const int height = settings.value(QStringLiteral("resolution_height")).toInt(task->outputHeight);
    if (width > 0 && height > 0) {
        const bool removedPreset = width == 1024 && height == 1024;
        task->outputWidth = removedPreset ? 1080 : width;
        task->outputHeight = removedPreset ? 1080 : height;
    }
    task->fps = normalizedVideoExportFps(settings.value(QStringLiteral("fps")).toInt(task->fps));
    task->audioBitrateKbps = normalizedVideoExportAudioBitrateKbps(
        settings.value(QStringLiteral("audio_bitrate_kbps")).toInt(task->audioBitrateKbps));
    task->outputMode = videoExportOutputModeFromToken(settings.value(QStringLiteral("output_mode")).toString());
    task->preset = videoExportPresetFromPreference(settings.value(QStringLiteral("preset")), task->preset);
    task->sizePreset = videoExportSizePresetFromPreference(
        settings.value(QStringLiteral("size_preset")), task->sizePreset);
    task->clockCountEnabled = settings.value(QStringLiteral("clock_count_enabled"))
                                  .toBool(task->clockCountEnabled);
    task->intro.enabled = settings.value(QStringLiteral("add_intro")).toBool(task->intro.enabled);
    task->intro.pvPreview = settings.value(QStringLiteral("intro_style"))
                                .toString(task->intro.pvPreview ? QStringLiteral("pv_preview")
                                                                : QStringLiteral("classic"))
        == QStringLiteral("pv_preview");
    task->intro.pvPreviewBrightness = qBound(
        0.0, settings.value(QStringLiteral("intro_pv_brightness")).toDouble(task->intro.pvPreviewBrightness), 1.0);
    task->intro.backgroundMode = settings.value(QStringLiteral("intro_background_mode"))
                                     .toString(task->intro.backgroundMode);
    task->intro.customBackgroundPath = settings.value(QStringLiteral("intro_background_custom_path"))
                                           .toString(task->intro.customBackgroundPath);
    task->intro.blurBackground = settings.value(QStringLiteral("intro_background_blur"))
                                     .toBool(task->intro.blurBackground);
    task->intro.mode = settings.value(QStringLiteral("intro_card_type")).toString(task->intro.mode);
    task->intro.cardShadow = settings.value(QStringLiteral("intro_card_shadow"))
                                 .toBool(task->intro.cardShadow);
    task->intro.lvRenderMode = settings.value(QStringLiteral("intro_level_text_render"))
                                   .toBool(task->intro.lvRenderMode.compare(
                                       QStringLiteral("text"), Qt::CaseInsensitive) == 0)
        ? QStringLiteral("text")
        : QStringLiteral("atlas");
    task->intro.fontDisplayPath = settings.value(QStringLiteral("intro_card_font_display"))
                                      .toString(task->intro.fontDisplayPath);
    task->intro.fontBodyPath = settings.value(QStringLiteral("intro_card_font_body"))
                                   .toString(task->intro.fontBodyPath);
}

void appendVideoExportPreferences(QJsonObject* settings, const VideoExportTask& task)
{
    if (settings == nullptr) {
        return;
    }
    settings->insert(QStringLiteral("resolution_width"), task.outputWidth);
    settings->insert(QStringLiteral("resolution_height"), task.outputHeight);
    settings->insert(QStringLiteral("fps"), task.fps);
    settings->insert(QStringLiteral("audio_bitrate_kbps"), task.audioBitrateKbps);
    settings->insert(QStringLiteral("output_mode"), videoExportOutputModeToken(task.outputMode));
    settings->insert(QStringLiteral("preset"), videoExportPreferencePresetToken(task.preset));
    settings->insert(QStringLiteral("size_preset"), videoExportSizePresetToken(task.sizePreset));
    settings->insert(QStringLiteral("clock_count_enabled"), task.clockCountEnabled);
    settings->insert(QStringLiteral("add_intro"), task.intro.enabled);
    settings->insert(QStringLiteral("intro_style"),
                     task.intro.pvPreview ? QStringLiteral("pv_preview") : QStringLiteral("classic"));
    settings->insert(QStringLiteral("intro_pv_brightness"), task.intro.pvPreviewBrightness);
    settings->insert(QStringLiteral("intro_background_mode"), task.intro.backgroundMode);
    settings->insert(QStringLiteral("intro_background_custom_path"), task.intro.customBackgroundPath.trimmed());
    settings->insert(QStringLiteral("intro_background_blur"), task.intro.blurBackground);
    settings->insert(QStringLiteral("intro_card_type"), task.intro.mode);
    settings->insert(QStringLiteral("intro_card_shadow"), task.intro.cardShadow);
    settings->insert(
        QStringLiteral("intro_level_text_render"),
        task.intro.lvRenderMode.compare(QStringLiteral("text"), Qt::CaseInsensitive) == 0);
    settings->insert(QStringLiteral("intro_card_font_display"), task.intro.fontDisplayPath);
    settings->insert(QStringLiteral("intro_card_font_body"), task.intro.fontBodyPath);
}

QJsonObject videoExportTemplateSettings(const VideoExportTask& task)
{
    QJsonObject settings;
    appendVideoExportPreferences(&settings, task);
    settings.insert(QStringLiteral("smoothBrightness"), task.smoothBrightness);
    settings.insert(QStringLiteral("showTimestamp"), task.showTimestamp);
    settings.insert(QStringLiteral("showMeasureLines"), task.showMeasureLines);
    settings.insert(QStringLiteral("showJudgementSkeleton"), task.showJudgementSkeleton);
    settings.insert(QStringLiteral("showSlideJudgementTiming"), task.showSlideJudgementTiming);
    settings.insert(QStringLiteral("slideTimingFromLaunchBeat"), task.slideTimingFromLaunchBeat);
    settings.insert(QStringLiteral("slideTimingEarlyPerfectFromHead"), task.slideTimingEarlyPerfectFromHead);
    settings.insert(QStringLiteral("showObjectStatsHud"), task.showObjectStatsHud);
    settings.insert(QStringLiteral("showChartInfoHud"), task.showChartInfoHud);
    settings.insert(QStringLiteral("showBpmHud"), task.showBpmHud);
    settings.insert(QStringLiteral("showCpsHud"), task.showCpsHud);
    settings.insert(QStringLiteral("showMeterHud"), task.showMeterHud);
    settings.insert(QStringLiteral("showModsHud"), task.showModsHud);
    settings.insert(QStringLiteral("slideEarlierSecondAndTextOnTop"), task.slideEarlierSecondAndTextOnTop);
    settings.insert(QStringLiteral("backgroundBrightnessOuter"), task.backgroundBrightnessOuter);
    settings.insert(QStringLiteral("backgroundBrightnessInner"), task.backgroundBrightnessInner);
    settings.insert(QStringLiteral("layoutSquareScale"), task.layoutSquareScale);
    settings.insert(QStringLiteral("fireworkBrightness"), task.fireworkBrightness);
    settings.insert(QStringLiteral("tapFlowSpeed"), task.tapFlowSpeed);
    settings.insert(QStringLiteral("touchFlowSpeed"), task.touchFlowSpeed);
    settings.insert(QStringLiteral("measureLineMode"), task.measureLineMode);
    settings.insert(QStringLiteral("measureLineCustomBeats"), task.measureLineCustomBeats);
    settings.insert(QStringLiteral("introSoundFileName"), task.introSoundFileName);
    settings.insert(QStringLiteral("backgroundScaleMode"), static_cast<int>(task.backgroundScaleMode));
    settings.insert(QStringLiteral("outlineVariant"), static_cast<int>(task.outlineVariant));
    settings.insert(QStringLiteral("tapJudgeTextDistance"), static_cast<int>(task.tapJudgeTextDistance));
    settings.insert(QStringLiteral("judgeEffectStyle"), static_cast<int>(task.judgeEffectStyle));
    settings.insert(QStringLiteral("centerDisplayMode"), static_cast<int>(task.centerDisplayMode));
    settings.insert(QStringLiteral("intro_pv_brightness"), task.intro.pvPreviewBrightness);
    return settings;
}

bool applyVideoExportTemplateSettings(const QJsonObject& settings, VideoExportTask* task)
{
    if (!task || settings.isEmpty()) return false;
    for (const char* key : {"resolution_width", "resolution_height", "fps"})
        if (!settings.value(QLatin1String(key)).isDouble()) return false;
    VideoExportTask candidate = *task;
    applyVideoExportPreferences(settings, &candidate);
    if (settings.contains(QStringLiteral("smoothBrightness"))) { if (!settings.value(QStringLiteral("smoothBrightness")).isBool()) return false; candidate.smoothBrightness = settings.value(QStringLiteral("smoothBrightness")).toBool(); }
    if (settings.contains(QStringLiteral("showTimestamp"))) { if (!settings.value(QStringLiteral("showTimestamp")).isBool()) return false; candidate.showTimestamp = settings.value(QStringLiteral("showTimestamp")).toBool(); }
    if (settings.contains(QStringLiteral("showMeasureLines"))) { if (!settings.value(QStringLiteral("showMeasureLines")).isBool()) return false; candidate.showMeasureLines = settings.value(QStringLiteral("showMeasureLines")).toBool(); }
    if (settings.contains(QStringLiteral("showJudgementSkeleton"))) { if (!settings.value(QStringLiteral("showJudgementSkeleton")).isBool()) return false; candidate.showJudgementSkeleton = settings.value(QStringLiteral("showJudgementSkeleton")).toBool(); }
    if (settings.contains(QStringLiteral("showSlideJudgementTiming"))) { if (!settings.value(QStringLiteral("showSlideJudgementTiming")).isBool()) return false; candidate.showSlideJudgementTiming = settings.value(QStringLiteral("showSlideJudgementTiming")).toBool(); }
    if (settings.contains(QStringLiteral("slideTimingFromLaunchBeat"))) { if (!settings.value(QStringLiteral("slideTimingFromLaunchBeat")).isBool()) return false; candidate.slideTimingFromLaunchBeat = settings.value(QStringLiteral("slideTimingFromLaunchBeat")).toBool(); }
    if (settings.contains(QStringLiteral("slideTimingEarlyPerfectFromHead"))) { if (!settings.value(QStringLiteral("slideTimingEarlyPerfectFromHead")).isBool()) return false; candidate.slideTimingEarlyPerfectFromHead = settings.value(QStringLiteral("slideTimingEarlyPerfectFromHead")).toBool(); }
    if (settings.contains(QStringLiteral("showObjectStatsHud"))) { if (!settings.value(QStringLiteral("showObjectStatsHud")).isBool()) return false; candidate.showObjectStatsHud = settings.value(QStringLiteral("showObjectStatsHud")).toBool(); }
    if (settings.contains(QStringLiteral("showChartInfoHud"))) { if (!settings.value(QStringLiteral("showChartInfoHud")).isBool()) return false; candidate.showChartInfoHud = settings.value(QStringLiteral("showChartInfoHud")).toBool(); }
    if (settings.contains(QStringLiteral("showBpmHud"))) { if (!settings.value(QStringLiteral("showBpmHud")).isBool()) return false; candidate.showBpmHud = settings.value(QStringLiteral("showBpmHud")).toBool(); }
    if (settings.contains(QStringLiteral("showCpsHud"))) { if (!settings.value(QStringLiteral("showCpsHud")).isBool()) return false; candidate.showCpsHud = settings.value(QStringLiteral("showCpsHud")).toBool(); }
    if (settings.contains(QStringLiteral("showMeterHud"))) { if (!settings.value(QStringLiteral("showMeterHud")).isBool()) return false; candidate.showMeterHud = settings.value(QStringLiteral("showMeterHud")).toBool(); }
    if (settings.contains(QStringLiteral("showModsHud"))) { if (!settings.value(QStringLiteral("showModsHud")).isBool()) return false; candidate.showModsHud = settings.value(QStringLiteral("showModsHud")).toBool(); }
    if (settings.contains(QStringLiteral("slideEarlierSecondAndTextOnTop"))) { if (!settings.value(QStringLiteral("slideEarlierSecondAndTextOnTop")).isBool()) return false; candidate.slideEarlierSecondAndTextOnTop = settings.value(QStringLiteral("slideEarlierSecondAndTextOnTop")).toBool(); }
    if (settings.contains(QStringLiteral("backgroundBrightnessOuter"))) { if (!settings.value(QStringLiteral("backgroundBrightnessOuter")).isDouble()) return false; candidate.backgroundBrightnessOuter = settings.value(QStringLiteral("backgroundBrightnessOuter")).toDouble(); }
    if (settings.contains(QStringLiteral("backgroundBrightnessInner"))) { if (!settings.value(QStringLiteral("backgroundBrightnessInner")).isDouble()) return false; candidate.backgroundBrightnessInner = settings.value(QStringLiteral("backgroundBrightnessInner")).toDouble(); }
    if (settings.contains(QStringLiteral("layoutSquareScale"))) { if (!settings.value(QStringLiteral("layoutSquareScale")).isDouble()) return false; candidate.layoutSquareScale = settings.value(QStringLiteral("layoutSquareScale")).toDouble(); }
    if (settings.contains(QStringLiteral("fireworkBrightness"))) { if (!settings.value(QStringLiteral("fireworkBrightness")).isDouble()) return false; candidate.fireworkBrightness = settings.value(QStringLiteral("fireworkBrightness")).toDouble(); }
    if (settings.contains(QStringLiteral("tapFlowSpeed"))) { if (!settings.value(QStringLiteral("tapFlowSpeed")).isDouble()) return false; candidate.tapFlowSpeed = settings.value(QStringLiteral("tapFlowSpeed")).toDouble(); }
    if (settings.contains(QStringLiteral("touchFlowSpeed"))) { if (!settings.value(QStringLiteral("touchFlowSpeed")).isDouble()) return false; candidate.touchFlowSpeed = settings.value(QStringLiteral("touchFlowSpeed")).toDouble(); }
    if (settings.contains(QStringLiteral("measureLineMode"))) { if (!settings.value(QStringLiteral("measureLineMode")).isDouble()) return false; candidate.measureLineMode = settings.value(QStringLiteral("measureLineMode")).toInt(-1); }
    if (settings.contains(QStringLiteral("measureLineCustomBeats"))) { if (!settings.value(QStringLiteral("measureLineCustomBeats")).isDouble()) return false; candidate.measureLineCustomBeats = settings.value(QStringLiteral("measureLineCustomBeats")).toInt(-1); }
    candidate.introSoundFileName = settings.value(QStringLiteral("introSoundFileName")).toString(candidate.introSoundFileName);
    if (settings.contains(QStringLiteral("backgroundScaleMode"))) { const int value = settings.value(QStringLiteral("backgroundScaleMode")).toInt(-1); if (value < 0 || value > 3) return false; candidate.backgroundScaleMode = static_cast<decltype(candidate.backgroundScaleMode)>(value); }
    if (settings.contains(QStringLiteral("outlineVariant"))) { const int value = settings.value(QStringLiteral("outlineVariant")).toInt(-1); if (value < 0 || value > 4) return false; candidate.outlineVariant = static_cast<decltype(candidate.outlineVariant)>(value); }
    if (settings.contains(QStringLiteral("tapJudgeTextDistance"))) { const int value = settings.value(QStringLiteral("tapJudgeTextDistance")).toInt(-1); if (value < 0 || value > 2) return false; candidate.tapJudgeTextDistance = static_cast<decltype(candidate.tapJudgeTextDistance)>(value); }
    if (settings.contains(QStringLiteral("judgeEffectStyle"))) { const int value = settings.value(QStringLiteral("judgeEffectStyle")).toInt(-1); if (value < 0 || value > 1) return false; candidate.judgeEffectStyle = static_cast<decltype(candidate.judgeEffectStyle)>(value); }
    if (settings.contains(QStringLiteral("centerDisplayMode"))) { const int value = settings.value(QStringLiteral("centerDisplayMode")).toInt(-1); if (value < 0 || value > 7) return false; candidate.centerDisplayMode = static_cast<decltype(candidate.centerDisplayMode)>(value); }
    candidate.intro.pvPreviewBrightness = settings.value(QStringLiteral("intro_pv_brightness")).toDouble(candidate.intro.pvPreviewBrightness);
    bool resolutionValid = false;
    for (const auto& preset : kVideoExportResolutionPresets)
        resolutionValid |= preset.width == candidate.outputWidth && preset.height == candidate.outputHeight;
    if (!resolutionValid || candidate.backgroundBrightnessOuter < 0 || candidate.backgroundBrightnessOuter > 1
        || candidate.backgroundBrightnessInner < 0 || candidate.backgroundBrightnessInner > 1
        || candidate.tapFlowSpeed <= 0 || candidate.tapFlowSpeed > 100
        || candidate.touchFlowSpeed <= 0 || candidate.touchFlowSpeed > 100
        || candidate.measureLineCustomBeats < 1 || candidate.measureLineCustomBeats > 10000) return false;
    if (candidate.layoutSquareScale < 0.1 || candidate.layoutSquareScale > 1
        || candidate.fireworkBrightness < 0.5 || candidate.fireworkBrightness > 1.2
        || candidate.intro.pvPreviewBrightness < 0 || candidate.intro.pvPreviewBrightness > 1
        || candidate.measureLineMode < 0 || candidate.measureLineMode > 2) return false;
    if (candidate.sizePreset == VideoExportSizePreset::Target30Mb) {
        candidate.outputWidth = qMax(480, qRound(480.0 * candidate.outputWidth / candidate.outputHeight / 2) * 2);
        candidate.outputHeight = 480;
        candidate.intro.pvPreview = false;
    }
    *task = std::move(candidate);
    return true;
}

QString sanitizeVideoExportTimestamp(QString text)
{
    text.replace(QChar(0xff1a), QLatin1Char(':'));
    return text.trimmed();
}

QString formatVideoExportTimestamp(double seconds)
{
    const qint64 totalMs = qMax<qint64>(0, qRound64(seconds * 1000.0));
    const qint64 minutes = totalMs / 60000;
    const qint64 sec = (totalMs / 1000) % 60;
    const qint64 ms = totalMs % 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

bool parseVideoExportTimestamp(const QString& text, double* seconds)
{
    const QString sanitized = sanitizeVideoExportTimestamp(text);
    static const QRegularExpression re(QStringLiteral("^(\\d+)(?::(\\d{1,2}))?(?::(\\d{1,3}))?$"));
    const QRegularExpressionMatch match = re.match(sanitized);
    if (!match.hasMatch()) {
        return false;
    }
    bool minOk = false;
    const int minutes = match.captured(1).toInt(&minOk);
    if (!minOk || minutes < 0) {
        return false;
    }
    int sec = 0;
    if (!match.captured(2).isEmpty()) {
        bool secOk = false;
        sec = match.captured(2).toInt(&secOk);
        if (!secOk || sec < 0 || sec > 59) {
            return false;
        }
    }
    int ms = 0;
    if (!match.captured(3).isEmpty()) {
        bool msOk = false;
        ms = match.captured(3).toInt(&msOk);
        if (!msOk || ms < 0 || ms > 999) {
            return false;
        }
    }
    if (seconds != nullptr) {
        *seconds = static_cast<double>(minutes) * 60.0
            + static_cast<double>(sec)
            + static_cast<double>(ms) / 1000.0;
    }
    return true;
}

bool isFullRangeVideoExport(double exportStartSeconds)
{
    return exportStartSeconds <= kFullRangeVideoExportStartEpsilonSeconds;
}

void copyVideoExportUserSettings(const VideoExportTask& source, VideoExportTask* target)
{
    if (target == nullptr) {
        return;
    }
    target->outputWidth = source.outputWidth;
    target->outputHeight = source.outputHeight;
    target->fps = source.fps;
    target->audioBitrateKbps = source.audioBitrateKbps;
    target->outputMode = source.outputMode;
    target->preset = source.preset;
    target->sizePreset = source.sizePreset;
    target->backgroundBrightnessOuter = source.backgroundBrightnessOuter;
    target->backgroundBrightnessInner = source.backgroundBrightnessInner;
    target->layoutSquareScale = source.layoutSquareScale;
    target->fireworkBrightness = source.fireworkBrightness;
    target->smoothBrightness = source.smoothBrightness;
    target->backgroundScaleMode = source.backgroundScaleMode;
    target->tapFlowSpeed = source.tapFlowSpeed;
    target->touchFlowSpeed = source.touchFlowSpeed;
    target->showTimestamp = source.showTimestamp;
    target->showMeasureLines = source.showMeasureLines;
    target->showJudgementSkeleton = source.showJudgementSkeleton;
    target->showSlideJudgementTiming = source.showSlideJudgementTiming;
    target->slideTimingFromLaunchBeat = source.slideTimingFromLaunchBeat;
    target->slideTimingEarlyPerfectFromHead = source.slideTimingEarlyPerfectFromHead;
    target->measureLineMode = source.measureLineMode;
    target->measureLineCustomBeats = source.measureLineCustomBeats;
    target->showObjectStatsHud = source.showObjectStatsHud;
    target->showChartInfoHud = source.showChartInfoHud;
    target->showBpmHud = source.showBpmHud;
    target->showCpsHud = source.showCpsHud;
    target->showMeterHud = source.showMeterHud;
    target->showModsHud = source.showModsHud;
    target->clockCountEnabled = source.clockCountEnabled;
    target->intro.enabled = source.intro.enabled;
    target->intro.pvPreview = source.intro.pvPreview;
    target->intro.pvPreviewBrightness = source.intro.pvPreviewBrightness;
    target->intro.backgroundMode = source.intro.backgroundMode;
    target->intro.customBackgroundPath = source.intro.customBackgroundPath;
    target->intro.blurBackground = source.intro.blurBackground;
    target->intro.mode = source.intro.mode;
    target->intro.cardShadow = source.intro.cardShadow;
    target->intro.lvRenderMode = source.intro.lvRenderMode;
    target->intro.fontDisplayPath = source.intro.fontDisplayPath;
    target->intro.fontBodyPath = source.intro.fontBodyPath;
    target->introSoundFileName = source.introSoundFileName;
}

}  // namespace miacode::video_export
