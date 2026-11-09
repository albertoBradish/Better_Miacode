#pragma once

#include <QString>
#include <optional>

class SimaiDocument;

namespace miacode::chart_preview {

inline constexpr int kDropAlgorithmVersion = 3;

struct MediaRange {
    double startSeconds = 0.0;
    double endSeconds = 0.0;

    bool isValid() const;
    bool operator==(const MediaRange&) const = default;
};

enum class DropSource { Auto, Manual };

struct DropSelection {
    MediaRange range;
    DropSource source = DropSource::Auto;
    QString inputFingerprint;

    bool operator==(const DropSelection&) const = default;
};

enum class IntroWindowMode { Automatic, FollowDrop, Default };
enum class IntroWindowSource { Explicit, Demo, Drop, Default };

struct IntroWindow {
    MediaRange range;
    IntroWindowSource source = IntroWindowSource::Default;
};

double toMediaSeconds(double chartSeconds, double firstSeconds);
double toChartSeconds(double mediaSeconds, double firstSeconds);
QString dropChartFingerprint(const SimaiDocument& document, int difficultyId);
QString combineDropFingerprints(const QString& chartFingerprint, const QString& audioFingerprint);
QString dropInputFingerprint(const SimaiDocument& document, int difficultyId, const QString& audioFingerprint);

// Stored ranges are media seconds. Auto requires a matching current input fingerprint.
std::optional<DropSelection> storedDropSelection(const SimaiDocument& document, int difficultyId);
bool hasManualDropMarker(const SimaiDocument& document, int difficultyId);
std::optional<DropSelection> readDropSelection(const SimaiDocument& document, int difficultyId,
    const QString& currentFingerprint, double mediaDuration);
bool writeDropSelection(SimaiDocument& document, int difficultyId,
    std::optional<DropSelection> selection);
bool seedDemoWindow(SimaiDocument& document, MediaRange range, double mediaDuration);

IntroWindow resolveIntroWindow(const SimaiDocument& document, int difficultyId,
    std::optional<MediaRange> explicitOverride, IntroWindowMode mode, double mediaDuration,
    const QString& currentFingerprint = {}, std::optional<DropSelection> sessionDrop = {},
    bool preferSessionDrop = false);

} // namespace miacode::chart_preview
