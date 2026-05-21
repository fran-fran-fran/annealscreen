// SPDX-License-Identifier: GPL-3.0-or-later
// Thin Moonraker WebSocket client for AnnealScreen.
//
// Handles: connect, reconnect with exponential backoff, JSON-RPC 2.0
// request/response, status subscription, GCode execution, config query.
// No printer discovery, no multi-printer, no complex state machine.

#pragma once

#include <hv/WebSocketClient.h>
#include <hv/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace anneal {

class MoonrakerClient {
  public:
    MoonrakerClient();
    ~MoonrakerClient();

    // Connect to Moonraker WebSocket
    void connect(const std::string& host, int port = 7125);
    void disconnect();
    bool is_connected() const { return connected_.load(); }

    // Subscribe to printer object status updates.
    // objects: JSON object like {"annealr": null, "heater_generic annealer": ["temperature","target"]}
    void subscribe(const nlohmann::json& objects);

    // Send a GCode command (fire-and-forget)
    void execute_gcode(const std::string& gcode);

    // Query configfile.config and deliver result via callback (main thread)
    void query_config(std::function<void(const nlohmann::json&)> callback);

    // Callbacks — set before connect()
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;

    // Called with the full notify_status_update data dict
    std::function<void(const nlohmann::json&)> on_status_update;

  private:
    void on_open();
    void on_close();
    void on_message(const std::string& msg);

    void send_jsonrpc(const std::string& method,
                      const nlohmann::json& params = {},
                      int id = 0);

    void schedule_reconnect();

    hv::WebSocketClient ws_;
    std::string host_;
    int port_ = 7125;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_reconnect_{true};

    std::mutex id_mutex_;
    int next_id_ = 1;
    std::unordered_map<int, std::function<void(const nlohmann::json&)>> pending_requests_;

    // Reconnect state
    int reconnect_delay_ms_ = 1000;
    static constexpr int MAX_RECONNECT_DELAY_MS = 30000;
};

} // namespace anneal
