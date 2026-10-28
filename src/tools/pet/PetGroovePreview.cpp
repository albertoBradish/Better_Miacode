// Explicit-time, windowless authoring preview of the production Fox/PetScene.
// No application session, pet overlay window, desktop input or real-time clock.
#include "app/ui/pet/PetScene.h"

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSGRendererInterface>
#include <QTextStream>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#ifndef MIACODE_SOURCE_ROOT
#error "MIACODE_SOURCE_ROOT must identify the workspace source tree"
#endif

namespace {
using namespace miacode::ui;

struct Pose {
    QVector<QVector3D> vertices;
    double radius = 0;
    bool valid = true;
};

bool isFinite(double value) { return std::isfinite(value); }
bool isFinite(const QVector3D& value) { return isFinite(value.x()) && isFinite(value.y()) && isFinite(value.z()); }

void appendPose(const PetModelNode* node, const QMatrix4x4& parent, Pose* pose)
{
    if (!node) return;
    const auto& rotation = node->rotation;
    pose->valid &= isFinite(node->position) && isFinite(node->eulerRotation) && isFinite(node->scale)
        && isFinite(rotation.scalar()) && isFinite(rotation.x()) && isFinite(rotation.y()) && isFinite(rotation.z());
    pose->valid &= qAbs(node->scale.x()) <= 4 && qAbs(node->scale.y()) <= 4 && qAbs(node->scale.z()) <= 4;
    if (!node->visible) return;
    QMatrix4x4 matrix = parent;
    matrix.translate(node->position);
    matrix.rotate(rotation);
    matrix.rotate(QQuaternion::fromEulerAngles(node->eulerRotation));
    matrix.scale(node->scale);
    if (const auto* mesh = node->geometry) {
        for (int index : mesh->indexes) {
            if (index < 0 || index >= mesh->positions.size() || index >= mesh->uv0s.size()) {
                pose->valid = false; continue;
            }
            const auto vertex = matrix.map(mesh->positions[index]);
            pose->valid &= isFinite(vertex);
            pose->radius = qMax(pose->radius, double(vertex.length()));
            pose->vertices.append(vertex);
        }
    }
    for (QObject* child : node->childNodes())
        if (const auto* next = qobject_cast<PetModelNode*>(child)) appendPose(next, matrix, pose);
}

double poseDistance(const Pose& first, const Pose& second)
{
    if (!first.valid || !second.valid || first.vertices.size() != second.vertices.size())
        return std::numeric_limits<double>::infinity();
    double distance = 0;
    for (int i = 0; i < first.vertices.size(); ++i)
        distance = qMax(distance, double((first.vertices[i] - second.vertices[i]).length()));
    return distance;
}

class OffscreenPet final {
public:
    ~OffscreenPet()
    {
        if (rhi_) rhi_->makeThreadLocalNativeContextCurrent();
        if (window_) { window_->setRenderTarget({}); window_->releaseResources(); }
        if (control_) control_->invalidate();
        delete window_;
        delete control_;
        model_.reset();
        engine_.reset();
        target_.reset();
        pass_.reset();
        depth_.reset();
        color_.reset();
        rhi_.reset();
    }

    bool initialize(const QString& modelPath, const QString& backend, int size, bool mirrored, QString* error)
    {
        for (const QString& resource : {QStringLiteral(":/qt/qml/MiaCode/UI/pet/minecraft/fox.png"),
            QStringLiteral(":/src/app/ui/pet/shaders/pet.vert.qsb"), QStringLiteral(":/src/app/ui/pet/shaders/pet.frag.qsb")}) {
            if (!QFileInfo::exists(resource)) { *error = QStringLiteral("Missing production PetScene resource: %1").arg(resource); return false; }
        }
        size_ = QSize(size, size);
        if (backend == QLatin1String("d3d11")) {
#ifdef Q_OS_WIN
            QRhiD3D11InitParams params;
            rhi_.reset(QRhi::create(QRhi::D3D11, &params));
#else
            *error = QStringLiteral("D3D11 is available only on Windows."); return false;
#endif
        } else if (backend == QLatin1String("opengl")) {
#if QT_CONFIG(opengl)
            QRhiGles2InitParams params;
            surface_.reset(QRhiGles2InitParams::newFallbackSurface());
            params.fallbackSurface = surface_.get();
            rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params));
#else
            *error = QStringLiteral("Qt was built without OpenGL."); return false;
#endif
        } else if (backend == QLatin1String("metal")) {
#if QT_CONFIG(metal)
            QRhiMetalInitParams params;
            rhi_.reset(QRhi::create(QRhi::Metal, &params));
#else
            *error = QStringLiteral("Qt was built without Metal."); return false;
#endif
        } else { *error = QStringLiteral("Backend must be d3d11, opengl or metal."); return false; }
        if (!rhi_) { *error = QStringLiteral("Cannot initialize the requested offscreen QRhi backend."); return false; }

        engine_ = std::make_unique<QQmlEngine>();
        QQmlComponent component(engine_.get(), QUrl::fromLocalFile(modelPath), QQmlComponent::PreferSynchronous);
        if (component.status() != QQmlComponent::Ready) { *error = component.errorString(); return false; }
        std::unique_ptr<QObject> object(component.create());
        auto* model = qobject_cast<PetModelNode*>(object.get());
        if (!model) { *error = QStringLiteral("Fox.qml must create a PetModelNode: %1").arg(component.errorString()); return false; }
        object.release();
        model_.reset(model);
        if (!model_->setProperty("clip", QString()) || !model_->setProperty("lifeTime", 0.0)
            || !model_->setProperty("rhythmWeight", 1.0) || !model_->setProperty("rhythmQuarterBeat", 0.0)) {
            *error = QStringLiteral("The model must expose clip, lifeTime, rhythmWeight and rhythmQuarterBeat."); return false;
        }

        control_ = new QQuickRenderControl;
        window_ = new QQuickWindow(control_);
        window_->setColor(Qt::transparent);
        window_->setGeometry(0, 0, size, size);
        window_->contentItem()->setSize(QSizeF(size_));
        window_->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi_.get()));
        if (!control_->initialize()) { *error = QStringLiteral("Cannot initialize QQuickRenderControl."); return false; }
        color_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size_, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        depth_.reset(rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size_));
        if (!color_->create() || !depth_->create()) { *error = QStringLiteral("Cannot allocate the offscreen color/depth textures."); return false; }
        QRhiTextureRenderTargetDescription description(QRhiColorAttachment(color_.get()));
        description.setDepthStencilBuffer(depth_.get());
        target_.reset(rhi_->newTextureRenderTarget(description));
        pass_.reset(target_->newCompatibleRenderPassDescriptor());
        target_->setRenderPassDescriptor(pass_.get());
        if (!target_->create()) { *error = QStringLiteral("Cannot create the offscreen render target."); return false; }
        window_->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target_.get()));
        scene_ = new PetScene(window_->contentItem());
        scene_->setSize(QSizeF(size_));
        scene_->setCameraMirrored(mirrored);
        scene_->setModel(model_.get());
        return true;
    }

    Pose sample(double quarterBeat, double weight)
    {
        model_->setProperty("rhythmQuarterBeat", quarterBeat);
        model_->setProperty("rhythmWeight", weight);
        model_->setProperty("lifeTime", 0.0);
        Pose pose;
        appendPose(model_.get(), {}, &pose);
        pose.valid &= !pose.vertices.isEmpty();
        return pose;
    }

    QImage render(QString* error)
    {
        scene_->advanceFrame();
        control_->polishItems();
        control_->beginFrame();
        control_->sync();
        control_->render();
        readback_ = {};
        readbackCompleted_ = false;
        auto& result = readback_;
        result.completed = [this] { readbackCompleted_ = true; };
        auto* updates = rhi_->nextResourceUpdateBatch();
        updates->readBackTexture(QRhiReadbackDescription(color_.get()), &result);
        auto* commandBuffer = control_->commandBuffer();
        if (!commandBuffer) { *error = QStringLiteral("QQuickRenderControl did not begin a GPU frame."); updates->release(); return {}; }
        commandBuffer->resourceUpdate(updates);
        control_->endFrame();
        if (rhi_->finish() != QRhi::FrameOpSuccess || !readbackCompleted_ || result.pixelSize != size_
            || result.data.size() != size_.width() * size_.height() * 4) {
            *error = QStringLiteral("Offscreen texture readback did not complete with the requested size."); return {};
        }
        QImage image(reinterpret_cast<const uchar*>(result.data.constData()), size_.width(), size_.height(),
            size_.width() * 4, QImage::Format_RGBA8888_Premultiplied);
        image = image.copy();
        if (result.format == QRhiTexture::BGRA8) image = image.rgbSwapped();
        else if (result.format != QRhiTexture::RGBA8) { *error = QStringLiteral("Unexpected readback texture format."); return {}; }
        if (rhi_->isYUpInFramebuffer()) image = image.mirrored();
        if (window_->isVisible()) { *error = QStringLiteral("Offscreen invariant failed: a window became visible."); return {}; }
        return image.convertToFormat(QImage::Format_RGBA8888);
    }

    QString backendName() const { return QString::fromLatin1(rhi_->backendName()); }
private:
    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<QRhi> rhi_;
    std::unique_ptr<QRhiTexture> color_;
    std::unique_ptr<QRhiRenderBuffer> depth_;
    std::unique_ptr<QRhiRenderPassDescriptor> pass_;
    std::unique_ptr<QRhiTextureRenderTarget> target_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<PetModelNode> model_;
    QQuickRenderControl* control_ = nullptr;
    QQuickWindow* window_ = nullptr;
    PetScene* scene_ = nullptr;
    QSize size_;
    QRhiReadbackResult readback_;
    bool readbackCompleted_ = false;
};

QByteArray pixelHash(const QImage& image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y) hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4));
    return hash.result().toHex();
}

QJsonObject imageMetrics(const QImage& image)
{
    int count = 0, left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        if (image.constScanLine(y)[x * 4 + 3] == 0) continue;
        ++count; left = qMin(left, x); right = qMax(right, x); top = qMin(top, y); bottom = qMax(bottom, y);
    }
    return {{QStringLiteral("visiblePixels"), count}, {QStringLiteral("bounds"), QJsonArray{left, top, right, bottom}},
        {QStringLiteral("sha256"), QString::fromLatin1(pixelHash(image))}};
}

bool writeJson(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool generate(OffscreenPet& renderer, double bpm, int fps, const QString& directory, const QString& modelPath,
    const QString& sourceHash, int size, QString* error)
{
    if (!QDir().mkpath(directory) || !QDir().mkpath(QDir(directory).filePath(QStringLiteral("weights")))) {
        *error = QStringLiteral("Cannot create output directories."); return false;
    }
    const auto idle = renderer.sample(0, 0);
    const double radiusLimit = qMax(4.0, idle.radius * 4.0);
    bool valid = idle.valid;
    bool allTransformsFinite = idle.valid;
    const auto checkPose = [&](const Pose& pose) {
        allTransformsFinite &= pose.valid;
        return pose.valid && pose.radius <= radiusLimit;
    };
    const auto zero = renderer.sample(0, 1);
    const auto two = renderer.sample(2, 1);
    const auto four = renderer.sample(4, 1);
    double cycleError = qMax(poseDistance(zero, two), poseDistance(zero, four));
    const double wrapError = poseDistance(renderer.sample(2 - 1e-4, 1), renderer.sample(2 + 1e-4, 1));
    double idleError = 0;
    for (double beat : {0.125, 0.5, 1.0, 1.75, 2.0, 4.0})
        idleError = qMax(idleError, poseDistance(idle, renderer.sample(beat, 0)));
    const bool phasePosesSafe = checkPose(zero) & checkPose(two) & checkPose(four);
    valid &= phasePosesSafe && cycleError <= 1e-5 && wrapError < 0.005 && idleError <= 1e-6;
    if (!idle.valid || !phasePosesSafe) {
        *error = QStringLiteral("Unsafe model pose: invalid mesh/transform, non-finite values or excessive deformation.");
        return false;
    }

    renderer.sample(0, 0);
    QImage idleImage = renderer.render(error);
    if (idleImage.isNull() || !idleImage.save(QDir(directory).filePath(QStringLiteral("idle.png")))) return false;
    renderer.sample(0, 1);
    // Warm QQuickRhiItem's first allocation/upload, then capture explicit phase.
    if (renderer.render(error).isNull()) return false;
    const QImage phaseZero = renderer.render(error);
    if (phaseZero.isNull()) return false;
    renderer.sample(2, 1);
    const QImage phaseTwo = renderer.render(error);
    renderer.sample(4, 1);
    const QImage phaseFour = renderer.render(error);
    if (phaseTwo.isNull() || phaseFour.isNull()) return false;
    bool pixelsRepeat = pixelHash(phaseZero) == pixelHash(phaseTwo) && pixelHash(phaseZero) == pixelHash(phaseFour);
    valid &= pixelsRepeat;

    const int frameCount = qCeil(4.0 * 60.0 / bpm * fps);
    QJsonArray frames;
    QImage sheet(4 * 256, 2 * 280, QImage::Format_RGB32);
    sheet.fill(QColor(QStringLiteral("#e8edf2")));
    QPainter contact(&sheet);
    contact.setPen(QColor(QStringLiteral("#202730")));
    int nextContact = 0;
    bool allFramesVisible = true;
    double maximumRadius = 0;
    const double cycleFrameCount = 120.0 / bpm * fps;
    const int cycleFrames = qRound(cycleFrameCount);
    const bool alignedCycle = qAbs(cycleFrameCount - cycleFrames) < 1e-8;
    QVector<QByteArray> firstCycleHashes;
    for (int index = 0; index < frameCount; ++index) {
        const double seconds = double(index) / fps;
        const double quarterBeat = seconds * bpm / 60.0;
        const auto pose = renderer.sample(quarterBeat, 1);
        if (!checkPose(pose)) { *error = QStringLiteral("Unsafe model pose at frame %1.").arg(index); return false; }
        const auto repeatedPose = renderer.sample(quarterBeat + 2, 1);
        valid &= checkPose(repeatedPose);
        cycleError = qMax(cycleError, poseDistance(pose, repeatedPose));
        renderer.sample(quarterBeat, 1);
        maximumRadius = qMax(maximumRadius, pose.radius);
        const QImage image = renderer.render(error);
        if (image.isNull()) return false;
        const QString name = QStringLiteral("frame_%1.png").arg(index, 5, 10, QLatin1Char('0'));
        if (!image.save(QDir(directory).filePath(name))) { *error = QStringLiteral("Cannot save %1.").arg(name); return false; }
        auto metrics = imageMetrics(image);
        if (alignedCycle && index < cycleFrames) firstCycleHashes.append(pixelHash(image));
        else if (alignedCycle && index < cycleFrames * 2)
            pixelsRepeat &= firstCycleHashes[index - cycleFrames] == pixelHash(image);
        allFramesVisible &= metrics.value(QStringLiteral("visiblePixels")).toInt() > 32;
        metrics.insert(QStringLiteral("index"), index);
        metrics.insert(QStringLiteral("seconds"), seconds);
        metrics.insert(QStringLiteral("quarterBeat"), quarterBeat);
        metrics.insert(QStringLiteral("file"), name);
        frames.append(metrics);
        if (nextContact < 8 && quarterBeat + 1e-9 >= nextContact * 0.25) {
            const int x = nextContact % 4 * 256, y = nextContact / 4 * 280;
            contact.drawImage(QRect(x, y, 256, 256), image);
            contact.drawText(QRect(x, y + 256, 256, 24), Qt::AlignCenter,
                QStringLiteral("beat %1").arg(quarterBeat, 0, 'f', 3));
            ++nextContact;
        }
    }
    contact.end();
    if (!sheet.save(QDir(directory).filePath(QStringLiteral("contact.png")))) { *error = QStringLiteral("Cannot save contact sheet."); return false; }
    valid &= allFramesVisible && pixelsRepeat && cycleError <= 1e-5;

    double maxWeightStep = 0;
    Pose previous = renderer.sample(0.37, 0);
    bool weightSafe = checkPose(previous);
    for (int index = 0; index <= 40; ++index) {
        const double weight = index <= 20 ? index / 20.0 : (40 - index) / 20.0;
        const auto pose = renderer.sample(0.37, weight);
        maxWeightStep = qMax(maxWeightStep, poseDistance(previous, pose));
        weightSafe &= checkPose(pose);
        if (!weightSafe) { *error = QStringLiteral("Unsafe model pose during the start/stop weight sweep."); return false; }
        previous = pose;
        const QImage image = renderer.render(error);
        if (image.isNull() || !image.save(QDir(directory).filePath(QStringLiteral("weights/weight_%1.png").arg(index, 3, 10, QLatin1Char('0'))))) return false;
    }
    const double returnToIdleError = poseDistance(idle, previous);
    weightSafe &= maxWeightStep < 0.15 && returnToIdleError <= 1e-6;
    valid &= weightSafe;
    QJsonObject validation{{QStringLiteral("passed"), valid}, {QStringLiteral("cyclePoseError"), cycleError},
        {QStringLiteral("wrapPoseError"), wrapError}, {QStringLiteral("idlePhaseError"), idleError},
        {QStringLiteral("pixelsRepeatEveryTwoBeats"), pixelsRepeat}, {QStringLiteral("allFramesVisible"), allFramesVisible},
        {QStringLiteral("weightTransitionSafe"), weightSafe}, {QStringLiteral("maxWeightStepVertexDistance"), maxWeightStep},
        {QStringLiteral("returnToIdleError"), returnToIdleError}, {QStringLiteral("maximumVertexRadius"), maximumRadius},
        {QStringLiteral("radiusLimit"), radiusLimit}, {QStringLiteral("allTransformsFinite"), allTransformsFinite}};
    QJsonObject manifest{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("model"), modelPath}, {QStringLiteral("modelSha256"), sourceHash},
        {QStringLiteral("backend"), renderer.backendName()}, {QStringLiteral("bpm"), bpm}, {QStringLiteral("fps"), fps},
        {QStringLiteral("cycleQuarterBeats"), 2}, {QStringLiteral("cycles"), 2}, {QStringLiteral("frameCount"), frameCount},
        {QStringLiteral("frameSize"), QJsonArray{size, size}}, {QStringLiteral("lifeTime"), 0},
        {QStringLiteral("windowsShown"), 0}, {QStringLiteral("validation"), validation}, {QStringLiteral("frames"), frames}};
    if (!writeJson(QDir(directory).filePath(QStringLiteral("manifest.json")), manifest)) { *error = QStringLiteral("Cannot write manifest.json."); return false; }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("output"), directory}, {QStringLiteral("bpm"), bpm},
        {QStringLiteral("frames"), frameCount}, {QStringLiteral("validation"), validation}}).toJson(QJsonDocument::Compact) << Qt::endl;
    if (!valid) *error = QStringLiteral("Pose/pixel validation failed; inspect manifest.json and the captured frames.");
    return valid;
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pet_groove_preview"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Render the real Fox.qml through production PetScene into offscreen PNG textures. No window is shown."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("model"), QStringLiteral("Production Fox.qml or an old-version fixture to compare."), QStringLiteral("path"),
        QStringLiteral(MIACODE_SOURCE_ROOT "/src/app/ui/pet/model/Fox.qml")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("Output root; each BPM gets a separate subdirectory."), QStringLiteral("path"),
        QStringLiteral(MIACODE_SOURCE_ROOT "/output/pet_groove")});
    parser.addOption({QStringLiteral("bpm"), QStringLiteral("BPM or a comma-separated list, e.g. 60,120,150,240."), QStringLiteral("values"), QStringLiteral("120")});
    parser.addOption({QStringLiteral("fps"), QStringLiteral("Explicit sampling rate."), QStringLiteral("number"), QStringLiteral("60")});
    parser.addOption({QStringLiteral("size"), QStringLiteral("Square PNG side in pixels."), QStringLiteral("pixels"), QStringLiteral("512")});
#ifdef Q_OS_WIN
    const QString defaultBackend = QStringLiteral("d3d11");
#elif defined(Q_OS_MACOS)
    const QString defaultBackend = QStringLiteral("metal");
#else
    const QString defaultBackend = QStringLiteral("opengl");
#endif
    parser.addOption({QStringLiteral("backend"), QStringLiteral("d3d11, opengl or metal."), QStringLiteral("name"), defaultBackend});
    parser.addOption({QStringLiteral("mirrored"), QStringLiteral("Use the production mirrored camera." )});
    parser.addOption({QStringLiteral("overwrite"), QStringLiteral("Overwrite an existing preview generated at these exact settings.")});
    parser.process(app);
    bool fpsOk = false, sizeOk = false;
    const int fps = parser.value(QStringLiteral("fps")).toInt(&fpsOk);
    const int size = parser.value(QStringLiteral("size")).toInt(&sizeOk);
    if (!fpsOk || fps < 1 || fps > 240 || !sizeOk || size < 64 || size > 2048) {
        QTextStream(stderr) << "fps must be 1..240 and size must be 64..2048.\n"; return 2;
    }
    QVector<double> bpms;
    for (const QString& token : parser.value(QStringLiteral("bpm")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const double bpm = token.toDouble(&ok);
        if (!ok || !isFinite(bpm) || bpm < 20 || bpm > 600) { QTextStream(stderr) << "BPM must be 20..600.\n"; return 2; }
        if (!bpms.contains(bpm)) bpms.append(bpm);
    }
    if (bpms.isEmpty()) { QTextStream(stderr) << "At least one BPM is required.\n"; return 2; }
    const QString modelPath = QFileInfo(parser.value(QStringLiteral("model"))).absoluteFilePath();
    QFile source(modelPath);
    if (!source.open(QIODevice::ReadOnly)) { QTextStream(stderr) << source.errorString() << Qt::endl; return 2; }
    const QString sourceHash = QString::fromLatin1(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256).toHex());
    const QString outputRoot = QFileInfo(parser.value(QStringLiteral("output"))).absoluteFilePath();
    for (double bpm : bpms) {
        const QString directory = QDir(outputRoot).filePath(QStringLiteral("bpm_%1").arg(bpm, 0, 'g', 8));
        if (QFileInfo::exists(QDir(directory).filePath(QStringLiteral("manifest.json"))) && !parser.isSet(QStringLiteral("overwrite"))) {
            QTextStream(stderr) << "Preview already exists; use another output or --overwrite: " << directory << Qt::endl; return 2;
        }
    }
    const QString backend = parser.value(QStringLiteral("backend")).toLower();
    QQuickWindow::setGraphicsApi(backend == QLatin1String("d3d11") ? QSGRendererInterface::Direct3D11
        : backend == QLatin1String("metal") ? QSGRendererInterface::Metal : QSGRendererInterface::OpenGL);
    qmlRegisterType<PetModelNode>("MiaCode.UI", 1, 0, "PetModelNode");
    qmlRegisterType<PetMesh>("MiaCode.UI", 1, 0, "PetMesh");
    qmlRegisterType<PetScene>("MiaCode.UI", 1, 0, "PetScene");
    OffscreenPet renderer;
    QString error;
    if (!renderer.initialize(modelPath, backend, size, parser.isSet(QStringLiteral("mirrored")), &error)) {
        QTextStream(stderr) << error << Qt::endl; return 3;
    }
    for (double bpm : bpms) {
        const QString directory = QDir(outputRoot).filePath(QStringLiteral("bpm_%1").arg(bpm, 0, 'g', 8));
        if (!generate(renderer, bpm, fps, directory, modelPath, sourceHash, size, &error)) {
            QTextStream(stderr) << error << Qt::endl; return 1;
        }
    }
    return 0;
}
