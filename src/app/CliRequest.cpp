#include "app/CliRequest.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <cmath>
#include <cstdio>

namespace miacode::app::entry {
bool CliRequest::metadataOnly() const
{
    const QString command = values.value(QStringLiteral("command")).toString();
    return command == QLatin1String("help") || command == QLatin1String("version") || command == QLatin1String("capabilities");
}
bool CliRequest::needsGui() const
{
    return values.value(QStringLiteral("command")).toString() == QLatin1String("export")
        && (values.value(QStringLiteral("type")).toString() == QLatin1String("video") || values.value(QStringLiteral("type")).toString() == QLatin1String("cover"));
}
bool isPublicCliInvocation(const QStringList& arguments)
{
    if (arguments.contains(QStringLiteral("--export-video-worker"))) return false;
    if (arguments.contains(QStringLiteral("--export-video"))) return false;
    if (arguments.size() < 2) return false;
    static const QSet<QString> commands{QStringLiteral("help"), QStringLiteral("version"), QStringLiteral("capabilities"),
        QStringLiteral("stats"), QStringLiteral("presets"), QStringLiteral("export"), QStringLiteral("--help"),
        QStringLiteral("-h"), QStringLiteral("--version"), QStringLiteral("-v"), QStringLiteral("--request")};
    for (int i = 1; i < arguments.size(); ++i)
        if (commands.contains(arguments[i]) || arguments[i].startsWith(QLatin1String("--request="))) return true;
    return arguments.contains(QStringLiteral("--json")) || arguments.contains(QStringLiteral("--ndjson"));
}

CliRequest parseCliRequest(const QStringList& arguments, const QByteArray* suppliedRequest)
{
    CliRequest result;
    result.json = arguments.contains(QStringLiteral("--json"));
    result.ndjson = arguments.contains(QStringLiteral("--ndjson"));
    QJsonObject flags;
    QStringList positional;
    QString requestPath;
    static const QSet<QString> booleans{QStringLiteral("overwrite"), QStringLiteral("json"), QStringLiteral("ndjson"),
        QStringLiteral("show-bpm"), QStringLiteral("show-cps"), QStringLiteral("show-meter"), QStringLiteral("show-mods"),
        QStringLiteral("slide-judgement-timing"), QStringLiteral("slide-timing-from-launch-beat"),
        QStringLiteral("hide-timestamp"), QStringLiteral("show-object-stats"), QStringLiteral("show-chart-info"), QStringLiteral("intro")};
    static const QSet<QString> strings{QStringLiteral("chart"), QStringLiteral("difficulty"), QStringLiteral("output"),
        QStringLiteral("format"), QStringLiteral("preset"), QStringLiteral("skin"), QStringLiteral("track"), QStringLiteral("request")};
    static const QSet<QString> numbers{QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("start"), QStringLiteral("duration"), QStringLiteral("time"), QStringLiteral("flow-speed")};
    for (int i = 1; i < arguments.size(); ++i) {
        QString argument = arguments[i];
        if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) { positional = {QStringLiteral("help")}; break; }
        if (argument == QLatin1String("--version") || argument == QLatin1String("-v")) { positional = {QStringLiteral("version")}; break; }
        if (!argument.startsWith(QLatin1Char('-'))) { positional.append(argument); continue; }
        if (!argument.startsWith(QLatin1String("--"))) {
            result.error = QStringLiteral("Unknown option: %1").arg(argument); return result;
        }
        argument.remove(0, 2);
        const int equals = argument.indexOf(QLatin1Char('='));
        const QString name = equals >= 0 ? argument.left(equals) : argument;
        if (booleans.contains(name)) {
            if (equals >= 0) { result.error = QStringLiteral("Flag --%1 does not take a value.").arg(name); return result; }
            flags.insert(name, true); continue;
        }
        if (!strings.contains(name) && !numbers.contains(name)) {
            result.error = QStringLiteral("Unknown option: --%1").arg(name); return result;
        }
        if (equals < 0 && ++i >= arguments.size()) {
            result.error = QStringLiteral("Missing value for --%1.").arg(name); return result;
        }
        const QString value = equals >= 0 ? argument.mid(equals + 1) : arguments[i];
        if (name == QLatin1String("request")) { requestPath = value; result.json = true; continue; }
        if (numbers.contains(name)) {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || !std::isfinite(number)) { result.error = QStringLiteral("--%1 requires a finite number.").arg(name); return result; }
            flags.insert(name, number);
        } else flags.insert(name, value);
    }
    QJsonObject values;
    if (!requestPath.isEmpty()) {
        QByteArray bytes;
        if (suppliedRequest) bytes = *suppliedRequest;
        else {
            QFile file;
            const bool opened = requestPath == QLatin1String("-") ? file.open(stdin, QIODevice::ReadOnly)
                : (file.setFileName(requestPath), file.open(QIODevice::ReadOnly));
            if (!opened) { result.error = QStringLiteral("Cannot read request: %1").arg(requestPath); return result; }
            bytes = file.read(16 * 1024 * 1024 + 1);
        }
        if (bytes.size() > 16 * 1024 * 1024) { result.error = QStringLiteral("Request exceeds 16 MiB."); return result; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            result.error = QStringLiteral("Request must be a JSON object: %1").arg(error.errorString()); return result;
        }
        values = document.object();
        if (values.value(QStringLiteral("schemaVersion")) != QJsonValue(1)) {
            result.error = QStringLiteral("Unsupported request schemaVersion; expected 1."); return result;
        }
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const QString name = it.key();
            if (name == QLatin1String("schemaVersion")) continue;
            const bool stringField = (strings.contains(name) && name != QLatin1String("request"))
                || name == QLatin1String("command") || name == QLatin1String("type");
            if (name == QLatin1String("difficulty") && it.value().isDouble()) continue;
            if ((booleans.contains(name) && it.value().isBool()) || (stringField && it.value().isString())
                || (numbers.contains(name) && it.value().isDouble() && std::isfinite(it.value().toDouble()))) continue;
            result.error = QStringLiteral("Unknown request property or invalid type: %1").arg(name); return result;
        }
        if (requestPath != QLatin1String("-")) {
            const QDir requestDirectory(QFileInfo(requestPath).absolutePath());
            for (const QString& key : {QStringLiteral("chart"), QStringLiteral("output"), QStringLiteral("skin"), QStringLiteral("track")}) {
                const QString path = values.value(key).toString();
                if (!path.isEmpty() && QDir::isRelativePath(path)) values.insert(key, requestDirectory.absoluteFilePath(path));
            }
            const QString preset = values.value(QStringLiteral("preset")).toString();
            if (preset.startsWith(QLatin1String("file:")) && QDir::isRelativePath(preset.mid(5)))
                values.insert(QStringLiteral("preset"), QStringLiteral("file:") + requestDirectory.absoluteFilePath(preset.mid(5)));
        }
    }
    if (!positional.isEmpty()) {
        values.insert(QStringLiteral("command"), positional.takeFirst());
        if (values.value(QStringLiteral("command")).toString() == QLatin1String("export") && !positional.isEmpty())
            values.insert(QStringLiteral("type"), positional.takeFirst());
        else if (values.value(QStringLiteral("command")).toString() == QLatin1String("presets") && positional == QStringList{QStringLiteral("list")})
            positional.clear();
        if (!positional.isEmpty()) { result.error = QStringLiteral("Unexpected positional argument: %1").arg(positional.first()); return result; }
    }
    for (auto it = flags.constBegin(); it != flags.constEnd(); ++it) values.insert(it.key(), it.value());
    values.insert(QStringLiteral("schemaVersion"), 1);
    result.values = values;
    result.json = values.value(QStringLiteral("json")).toBool(result.json);
    result.ndjson = values.value(QStringLiteral("ndjson")).toBool(result.ndjson);
    const QString command = values.value(QStringLiteral("command")).toString();
    if (result.metadataOnly()) return result;
    if (command != QLatin1String("stats") && command != QLatin1String("presets") && command != QLatin1String("export")) {
        result.error = QStringLiteral("Expected help, version, capabilities, stats, presets or export."); return result;
    }
    if (command == QLatin1String("presets")) return result;
    if (values.value(QStringLiteral("chart")).toString().trimmed().isEmpty()) {
        result.error = QStringLiteral("--chart is required."); return result;
    }
    if (command == QLatin1String("export")) {
        const QString type = values.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("video") && type != QLatin1String("cover") && type != QLatin1String("bpm"))
            result.error = QStringLiteral("Expected export video, cover or bpm.");
        else if (values.value(QStringLiteral("output")).toString().trimmed().isEmpty())
            result.error = QStringLiteral("--output is required.");
    }
    for (const QString& key : {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("fps")}) {
        if (!values.contains(key)) continue;
        const double number = values[key].toDouble();
        const double maximum = key == QLatin1String("fps") ? 240 : 8192;
        if (number < 1 || number > maximum || number != std::floor(number))
            result.error = QStringLiteral("%1 must be an integer between 1 and %2.").arg(key).arg(maximum);
    }
    for (const QString& key : {QStringLiteral("start"), QStringLiteral("duration"), QStringLiteral("flow-speed")}) {
        if (values.contains(key) && (values[key].toDouble() < 0
            || (key != QLatin1String("start") && values[key].toDouble() == 0)))
            result.error = QStringLiteral("%1 must be %2.").arg(key, key == QLatin1String("start") ? QStringLiteral("nonnegative") : QStringLiteral("positive"));
    }
    if (values.value(QStringLiteral("type")).toString() == QLatin1String("video")
        && values.value(QStringLiteral("width")).toInt(1080) < values.value(QStringLiteral("height")).toInt(1080))
        result.error = QStringLiteral("Video width must be at least its height.");
    for (const QString& key : {QStringLiteral("start"), QStringLiteral("duration"), QStringLiteral("time")})
        if (values.contains(key) && qAbs(values[key].toDouble()) > 86400)
            result.error = QStringLiteral("%1 exceeds the supported 24-hour timeline.").arg(key);
    return result;
}
}
