#include "LocalVision.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QMutex>
#include <QPainter>
#include <QFileInfo>
#include <QDateTime>
#include <QSet>
#include <thread>
#ifdef COMPOSITOR_ONNX
#include <onnxruntime_cxx_api.h>
#endif
namespace comp {
static QString modelFolder() {
    auto override = qEnvironmentVariable("COMPOSITOR_MODEL_PATH");
    if (!override.isEmpty())
        return override;
    auto base = QCoreApplication::applicationDirPath();
    if (QFile::exists(base + "/models/validation.json"))
        return base + "/models";
    return QDir(base).absoluteFilePath("../../.tools/vision-models");
}
bool LocalVision::available() {
#ifdef COMPOSITOR_ONNX
    QFile file(modelFolder() + "/validation.json");
    return file.open(QIODevice::ReadOnly) &&
           QJsonDocument::fromJson(file.readAll()).object()["validated"].toBool();
#else
    return false;
#endif
}
static void unavailable() {
    throw std::runtime_error("本机视觉模型尚未安装或验证，请先圈画区域；基础修图仍可使用。");
}
#ifdef COMPOSITOR_ONNX
namespace {
struct Tensor {
    std::string name;
    std::vector<int64_t> shape;
    std::vector<float> values;
};
QMutex visionMutex;
Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "CompositorLocalVision");
    static bool disabled = [&] {
        env.DisableTelemetryEvents();
        return true;
    }();
    (void)disabled;
    return env;
}
std::vector<Tensor> infer(const QString& name, const std::vector<Tensor>& input,
                          const CancelCheck& canceled) {
    checkCanceled(canceled);
    while (!visionMutex.tryLock(50))
        checkCanceled(canceled);
    struct Unlock {
        ~Unlock() { visionMutex.unlock(); }
    } unlock;
    checkCanceled(canceled);
    const auto path = modelFolder() + '/' + name;
    QFile report(modelFolder() + "/validation.json");
    if (!report.open(QIODevice::ReadOnly))
        unavailable();
    auto expected = QJsonDocument::fromJson(report.readAll())
                        .object()["models"]
                        .toObject()[name]
                        .toObject()["sha256"]
                        .toString();
    QFile model(path);
    if (expected.isEmpty() || !model.open(QIODevice::ReadOnly))
        unavailable();
    const QFileInfo info(model);
    const auto identity = info.canonicalFilePath() + expected + QString::number(info.size()) +
                          QString::number(info.lastModified().toMSecsSinceEpoch()) +
                          QString::number(info.birthTime().toMSecsSinceEpoch());
    static QSet<QString> verified;
    if (!verified.contains(identity)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&model) || QString::fromLatin1(hash.result().toHex()) != expected)
            unavailable();
        if (verified.size() > 32)
            verified.clear();
        verified.insert(identity);
    }
    static QMap<QString, std::shared_ptr<Ort::Session>> sessions;
    const auto sessionID = path + expected;
    if (!sessions.contains(sessionID)) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        sessions[sessionID] = std::make_shared<Ort::Session>(
            environment(), reinterpret_cast<const wchar_t*>(path.utf16()), options);
    }
    auto& session = *sessions[sessionID];
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> tensors;
    std::vector<const char*> names;
    for (const auto& tensor : input) {
        names.push_back(tensor.name.c_str());
        tensors.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(tensor.values.data()),
                                                          tensor.values.size(), tensor.shape.data(),
                                                          tensor.shape.size()));
    }
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<Ort::AllocatedStringPtr> allocated;
    std::vector<const char*> outputs;
    for (size_t i = 0; i < session.GetOutputCount(); ++i) {
        allocated.push_back(session.GetOutputNameAllocated(i, allocator));
        outputs.push_back(allocated.back().get());
    }
    Ort::RunOptions run;
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (canceled && canceled()) {
                run.SetTerminate();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    auto values =
        session.Run(run, names.data(), tensors.data(), tensors.size(), outputs.data(), outputs.size());
    monitor.request_stop();
    checkCanceled(canceled);
    std::vector<Tensor> result;
    for (size_t i = 0; i < values.size(); ++i) {
        auto info = values[i].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            unavailable();
        auto count = info.GetElementCount();
        if (count > size_t(Limits::maxSurfacePixels) * 4)
            unavailable();
        auto data = values[i].GetTensorData<float>();
        result.push_back({outputs[i], info.GetShape(), std::vector<float>(data, data + count)});
    }
    return result;
}
Tensor normalized(const QImage& source, QSize size, QString name = "image", bool pad = false) {
    auto image =
        source.scaled(size, pad ? Qt::KeepAspectRatio : Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);
    Tensor tensor{name.toStdString(),
                  {1, 3, size.height(), size.width()},
                  std::vector<float>(size_t(size.width()) * size.height() * 3)};
    const float means[] = {123.675f, 116.28f, 103.53f}, deviations[] = {58.395f, 57.12f, 57.375f};
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            for (int c = 0; c < 3; ++c)
                tensor.values[size_t(c) * size.width() * size.height() + size_t(y) * size.width() + x] =
                    (image.constScanLine(y)[x * 3 + c] - means[c]) / deviations[c];
    return tensor;
}
QImage segmentation(const QImage& image, QPointF point, QRectF bounds, const CancelCheck& canceled) {
    if (image.isNull())
        unavailable();
    checkCanceled(canceled);
    auto normalizedImage = normalized(image, {1024, 1024}, "image", true);
    auto bytes = QByteArrayView(reinterpret_cast<const char*>(normalizedImage.values.data()),
                                qsizetype(normalizedImage.values.size() * sizeof(float)));
    QFile report(modelFolder() + "/validation.json");
    if (!report.open(QIODevice::ReadOnly))
        unavailable();
    const QFileInfo encoder(modelFolder() + "/mobilesam-encoder.onnx");
    auto identity = encoder.canonicalFilePath().toUtf8() + report.readAll() +
                    QByteArray::number(encoder.size()) +
                    QByteArray::number(encoder.lastModified().toMSecsSinceEpoch());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    hash.addData(identity);
    auto key = hash.result();
    static QMutex embeddingMutex;
    static QMap<QByteArray, std::vector<Tensor>> embeddings;
    static QList<QByteArray> order;
    std::vector<Tensor> embedding;
    {
        while (!embeddingMutex.tryLock(50))
            checkCanceled(canceled);
        struct Unlock {
            QMutex& mutex;
            ~Unlock() { mutex.unlock(); }
        } unlock{embeddingMutex};
        checkCanceled(canceled);
        if (!embeddings.contains(key)) {
            auto encoded = infer("mobilesam-encoder.onnx", {normalizedImage}, canceled);
            if (order.size() >= 2)
                embeddings.remove(order.takeFirst());
            order.append(key);
            embeddings[key] = std::move(encoded);
        }
        embedding = embeddings[key];
    }
    if (embedding.empty())
        unavailable();
    embedding[0].name = "image_embeddings";
    double scale = 1024. / std::max(image.width(), image.height());
    std::vector<float> points =
        bounds.isEmpty() ? std::vector<float>{float(point.x() * scale), float(point.y() * scale), 0, 0}
                         : std::vector<float>{float(bounds.left() * scale), float(bounds.top() * scale),
                                              float(bounds.right() * scale), float(bounds.bottom() * scale)};
    auto outputs = infer(
        "mobilesam-decoder.onnx",
        {embedding[0],
         {"point_coords", {1, 2, 2}, points},
         {"point_labels", {1, 2}, bounds.isEmpty() ? std::vector<float>{1, -1} : std::vector<float>{2, 3}},
         {"mask_input", {1, 1, 256, 256}, std::vector<float>(256 * 256)},
         {"has_mask_input", {1}, {0}},
         {"orig_im_size", {2}, {float(image.height()), float(image.width())}}},
        canceled);
    if (outputs.empty() || outputs[0].values.size() != size_t(image.width()) * image.height())
        unavailable();
    QImage mask(image.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            mask.scanLine(y)[x] = outputs[0].values[size_t(y) * image.width() + x] > 0 ? 255 : 0;
    return mask;
}
} // namespace
#endif
QImage LocalVision::objectMask(const QImage& image, QPointF point, const CancelCheck& canceled) {
#ifdef COMPOSITOR_ONNX
    return segmentation(image, point, {}, canceled);
#else
    unavailable();
    return {};
#endif
}
QImage LocalVision::boxMask(const QImage& image, QRectF bounds, const CancelCheck& canceled) {
#ifdef COMPOSITOR_ONNX
    return segmentation(image, {}, bounds.intersected(QRectF(QPoint(), image.size())), canceled);
#else
    unavailable();
    return {};
#endif
}
QImage LocalVision::refineMask(const QImage& image, const QImage& original, const CancelCheck& canceled) {
#ifdef COMPOSITOR_ONNX
    if (image.size() != original.size() || original.isNull())
        unavailable();
    auto mask = original.scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_Grayscale8);
    auto input = normalized(image, {512, 512});
    input.shape[1] = 4;
    input.values.resize(512 * 512 * 4);
    QImage trimap(mask.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x) {
            int lo = 255, hi = 0;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    int value = mask.constScanLine(std::clamp(y + dy, 0, 511))[std::clamp(x + dx, 0, 511)];
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
            uchar value = lo > 250 ? 255 : hi < 5 ? 0 : 128;
            trimap.scanLine(y)[x] = value;
            input.values[512 * 512 * 3 + y * 512 + x] = value / 255.f;
        }
    auto outputs = infer("vitmatte-small.onnx", {input}, canceled);
    if (outputs.empty() || outputs[0].values.size() != 512 * 512)
        unavailable();
    QImage alpha(mask.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x) {
            auto known = trimap.constScanLine(y)[x];
            alpha.scanLine(y)[x] =
                known == 128 ? uchar(std::clamp(outputs[0].values[y * 512 + x], 0.f, 1.f) * 255) : known;
        }
    return alpha.scaled(original.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
#else
    unavailable();
    return {};
#endif
}
QVector<PosePoint> LocalVision::pose(const QImage& image, QRectF person, const CancelCheck& canceled) {
#ifdef COMPOSITOR_ONNX
    if (person.isEmpty())
        person = QRectF(QPoint(), image.size());
    auto center = person.center();
    auto size = person.size() * 1.25;
    if (size.width() / size.height() > .75)
        size.setHeight(size.width() / .75);
    else
        size.setWidth(size.height() * .75);
    person = QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
    QImage crop(192, 256, QImage::Format_RGB888);
    crop.fill(Qt::black);
    QPainter painter(&crop);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(0, 0, 192, 256), image, person);
    painter.end();
    auto outputs = infer("rtmpose-m.onnx", {normalized(crop, {192, 256}, "input")}, canceled);
    if (outputs.size() < 2 || outputs[0].shape.size() != 3 || outputs[1].shape.size() != 3 ||
        outputs[0].shape[1] < 17)
        unavailable();
    QVector<PosePoint> points;
    const auto& xx = outputs[0];
    const auto& yy = outputs[1];
    int nx = int(xx.shape[2]), ny = int(yy.shape[2]);
    for (int i = 0; i < 17; ++i) {
        auto x = std::max_element(xx.values.begin() + i * nx, xx.values.begin() + (i + 1) * nx);
        auto y = std::max_element(yy.values.begin() + i * ny, yy.values.begin() + (i + 1) * ny);
        auto px = int(x - (xx.values.begin() + i * nx)), py = int(y - (yy.values.begin() + i * ny));
        points.append(
            {{person.x() + px / double(nx) * person.width(), person.y() + py / double(ny) * person.height()},
             std::min(*x, *y)});
    }
    return points;
#else
    unavailable();
    return {};
#endif
}
} // namespace comp
