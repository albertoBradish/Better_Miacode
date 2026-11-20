#pragma once

class QQuickItem;

namespace miacode::video_export {

struct VideoWatermarkSettings;
void addVideoWatermarkOverlay(QQuickItem* parent, const VideoWatermarkSettings& settings);

}  // namespace miacode::video_export
