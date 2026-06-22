#pragma once

#include <QObject>

#include "api/Player.h"

namespace vsr {

/// Bridge between Qt client and libvsrplayer core.
///
/// Translates Qt signals/slots ↔ Player commands/events.
/// Owns the Player instance and manages its lifecycle.
class PlayerProxy : public QObject {
    Q_OBJECT

public:
    explicit PlayerProxy(QObject* parent = nullptr);
    ~PlayerProxy() override;

    /// Initialize the player core with the Vulkan surface.
    bool initialize(void* vulkan_surface);

    /// Send a command to the player core.
    void send_command(PlayerCommand cmd);

    /// Get the underlying Player.
    Player* player() const { return player_.get(); }

signals:
    void state_changed(PlaybackState state);
    void position_changed(int64_t time_ms, int64_t duration_ms);
    void frame_info(int in_w, int in_h, int out_w, int out_h, double fps);
    void error_occurred(const QString& message);
    void end_of_file();

private:
    void on_player_event(const PlayerEvent& event);

    std::unique_ptr<Player> player_;
};

}  // namespace vsr
