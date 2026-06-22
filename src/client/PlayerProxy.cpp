#include "PlayerProxy.h"

#include <QDebug>

namespace vsr {

PlayerProxy::PlayerProxy(QObject* parent) : QObject(parent) {
    player_ = CreatePlayer();
    player_->set_event_callback([this](const PlayerEvent& e) {
        // Forward to Qt main thread via queued connection
        QMetaObject::invokeMethod(this, [this, e] { on_player_event(e); },
                                  Qt::QueuedConnection);
    });
}

PlayerProxy::~PlayerProxy() {
    player_->shutdown();
}

bool PlayerProxy::initialize(void* vulkan_surface) {
    return player_->initialize(vulkan_surface);
}

void PlayerProxy::send_command(PlayerCommand cmd) {
    player_->send_command(std::move(cmd));
}

void PlayerProxy::on_player_event(const PlayerEvent& event) {
    switch (event.type) {
    case PlayerEvent::STATE_CHANGED:
        emit state_changed(event.state);
        break;
    case PlayerEvent::POSITION_CHANGED:
        emit position_changed(event.time_ms, event.duration_ms);
        break;
    case PlayerEvent::FRAME_INFO:
        emit frame_info(event.in_width, event.in_height,
                        event.out_width, event.out_height, event.fps);
        break;
    case PlayerEvent::ERROR:
        emit error_occurred(QString::fromStdString(event.error_msg));
        break;
    case PlayerEvent::END_OF_FILE:
        emit end_of_file();
        break;
    }
}

}  // namespace vsr
