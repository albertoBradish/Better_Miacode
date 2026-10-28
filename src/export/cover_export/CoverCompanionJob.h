#pragma once

#include <QImage>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <utility>

namespace miacode::cover_export {

// Frozen pixels belong to one video request. Nothing is written until a video
// success consumes them; cancellation and stale asynchronous cleanup drop them.
class CoverCompanionJob final {
public:
    struct Prepared {
        QImage image;
        QString error;
    };
    quint64 begin()
    {
        cancel();
        active_ = true;
        return generation_;
    }
    quint64 generation() const { return generation_; }
    bool prepare(quint64 generation, QImage image, QString error)
    {
        if (!active_ || generation != generation_) return false;
        prepared_ = Prepared{std::move(image), std::move(error)};
        return true;
    }
    // Source changes only cancel the preparation stage. Once pixels are ready,
    // the launched video owns them independently of later editor navigation.
    bool invalidatePreparation()
    {
        if (!active_ || prepared_) return false;
        cancel();
        return true;
    }
    std::optional<Prepared> takeForSuccessfulVideo()
    {
        active_ = false;
        return std::exchange(prepared_, std::nullopt);
    }
    void cancel()
    {
        ++generation_;
        active_ = false;
        prepared_.reset();
    }
    void discard(quint64 generation)
    {
        if (generation == generation_) cancel();
    }
private:
    quint64 generation_ = 0;
    bool active_ = false;
    std::optional<Prepared> prepared_;
};

} // namespace miacode::cover_export
