#include "preview/stage_media/PreviewStageMediaHost.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTextStream>
#include <cmath>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 3) return 2;
    const QString chart = QString::fromLocal8Bit(argv[1]);
    const double expected = QString::fromLocal8Bit(argv[2]).toDouble();
    PreviewStageMediaHost host;
    host.setVideoDecodePreference(true);
    host.setPreviewFrameRateLimit(0.0);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&host, &PreviewStageMediaHost::diagnosticsChanged, &loop, [&] {
        if (host.sourceFrameRate() > 0 && host.hasVideoFrame()) loop.quit();
    });
    host.initializeBackendObjects();
    host.setChartPath(chart);
    deadline.start(10000);
    loop.exec();
    const bool ok = host.hasVideoFrame() && std::abs(host.sourceFrameRate() - expected) < 0.001;
    QTextStream(stdout) << (ok ? "PASS" : "FAIL") << " detected=" << host.sourceFrameRate()
        << " expected=" << expected << " frames=" << host.videoFrameCountTotal() << Qt::endl;
    host.setChartPath({});
    const bool cleared = host.sourceFrameRate() == 0.0 && !host.hasVideoMedia();
    QTextStream(stdout) << (cleared ? "PASS" : "FAIL") << " source reset" << Qt::endl;
    return ok && cleared ? 0 : 1;
}
