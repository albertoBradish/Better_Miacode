#include "app/CliRequest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QDir>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::app::entry;
    int failures = 0;
    const auto expect = [&](bool condition, const char* label) {
        if (!condition) { ++failures; qWarning() << "FAIL:" << label; }
    };
    const auto parse = [](const QStringList& tail, const QByteArray* payload = nullptr) {
        return parseCliRequest(QStringList{QStringLiteral("MiaCode")} + tail, payload);
    };
    for (const QString& command : {QStringLiteral("help"), QStringLiteral("version"), QStringLiteral("capabilities")}) {
        const auto request = parse({command, QStringLiteral("--json")});
        expect(request.error.isEmpty() && request.metadataOnly() && !request.needsGui() && request.json,
            "metadata commands require neither runtime tasks nor GUI");
    }
    expect(!isPublicCliInvocation({QStringLiteral("MiaCode"), QStringLiteral("--export-video-worker"), QStringLiteral("--help")}),
        "internal worker dispatch never enters the public CLI gate");
    expect(!isPublicCliInvocation({QStringLiteral("MiaCode"), QStringLiteral("D:/charts/song/maidata.txt")}),
        "ordinary chart opening remains a GUI command");
    expect(isPublicCliInvocation({QStringLiteral("MiaCode"), QStringLiteral("--json"), QStringLiteral("stats")}),
        "global machine-output options can precede the subcommand");
    const QByteArray json(R"({"schemaVersion":1,"command":"stats","chart":"D:/my songs/maidata.txt","difficulty":4})");
    const auto request = parse({QStringLiteral("--request"), QStringLiteral("-")}, &json);
    expect(request.error.isEmpty() && !request.needsGui() && request.json
        && request.values.value(QStringLiteral("difficulty")).toInt() == 4,
        "versioned stdin request preserves typed fields and defaults to JSON output");
    const QByteArray relativeJson(R"({"schemaVersion":1,"command":"stats","chart":"song/maidata.txt"})");
    const auto relative = parse({QStringLiteral("--request=fixtures/request.json")}, &relativeJson);
    expect(relative.values.value(QStringLiteral("chart")).toString()
        == QDir(QFileInfo(QStringLiteral("fixtures/request.json")).absolutePath()).absoluteFilePath(QStringLiteral("song/maidata.txt")),
        "structured file paths resolve beside the request file");
    const QByteArray future(R"({"schemaVersion":2,"command":"stats","chart":"x"})");
    expect(!parse({QStringLiteral("--request"), QStringLiteral("-")}, &future).error.isEmpty(),
        "unsupported request versions fail before executing a task");
    const QByteArray unknown(R"({"schemaVersion":1,"command":"stats","chart":"x","widht":1080})");
    expect(!parse({QStringLiteral("--request"), QStringLiteral("-")}, &unknown).error.isEmpty(),
        "unknown structured properties are diagnosed rather than ignored");
    const auto video = parse({QStringLiteral("export"), QStringLiteral("video"), QStringLiteral("--chart"), QStringLiteral("song"),
        QStringLiteral("--output=movie.mp4"), QStringLiteral("--width"), QStringLiteral("640"), QStringLiteral("--height"), QStringLiteral("360"),
        QStringLiteral("--fps"), QStringLiteral("30"), QStringLiteral("--duration"), QStringLiteral("1"), QStringLiteral("--overwrite"), QStringLiteral("--json"), QStringLiteral("--slide-judgement-timing"), QStringLiteral("--slide-timing-from-launch-beat")});
    expect(video.error.isEmpty() && video.needsGui() && video.json && video.values.value(QStringLiteral("overwrite")).toBool()
        && video.values.value(QStringLiteral("width")).toInt() == 640
        && video.values.value(QStringLiteral("slide-judgement-timing")).toBool()
        && video.values.value(QStringLiteral("slide-timing-from-launch-beat")).toBool(), "video arguments preserve explicit geometry/output/overwrite and slide timing");
    expect(!parse({QStringLiteral("export"), QStringLiteral("video"), QStringLiteral("--chart"), QStringLiteral("song")}).error.isEmpty(),
        "export requires an explicit output path");
    for (const QString& number : {QStringLiteral("0"), QStringLiteral("3.5"), QStringLiteral("NaN"), QStringLiteral("90000")})
        expect(!parse({QStringLiteral("export"), QStringLiteral("video"), QStringLiteral("--chart=x"), QStringLiteral("--output=x.mp4"),
            QStringLiteral("--width=") + number}).error.isEmpty(), "invalid geometry is rejected before renderer creation");
    const auto negativeCover = parse({QStringLiteral("export"), QStringLiteral("cover"), QStringLiteral("--chart=x"),
        QStringLiteral("--output=x.png"), QStringLiteral("--time=-1.5"), QStringLiteral("--preset=builtin:pure_chart_frame")});
    expect(negativeCover.error.isEmpty() && negativeCover.values.value(QStringLiteral("time")).toDouble() == -1.5,
        "cover frame time permits the production intro axis");
    const auto bpm = parse({QStringLiteral("export"), QStringLiteral("bpm"), QStringLiteral("--chart=x"),
        QStringLiteral("--output=x.txt"), QStringLiteral("--format=audio-time")});
    expect(bpm.error.isEmpty() && !bpm.needsGui(), "BPM export is a text-only task");
    return failures == 0 ? 0 : 1;
}
