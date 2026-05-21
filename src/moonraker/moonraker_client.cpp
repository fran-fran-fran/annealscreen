// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>
#include <hv/htime.h>

#include <chrono>
#include <thread>

namespace anneal {

MoonrakerClient::MoonrakerClient() = default;

MoonrakerClient::~MoonrakerClient() {
    should_reconnect_ = false;
    disconnect();
}

void MoonrakerClient::connect(const std::string& host, int port) {
    host_ = host;
    port_ = port;
    should_reconnect_ = true;
    reconnect_delay_ms_ = 1000;

    std::string url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
    spdlog::info("[Moonraker] Connecting to {}", url);

    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = 1000;
    reconn.max_delay = 30000;
    reconn.delay_policy = 2;  // exponential
    ws_.setReconnect(&reconn);

    ws_.onopen = [this]() { on_open(); };
    ws_.onclose = [this]() { on_close(); };
    ws_.onmessage = [this](const std::string& msg) { on_message(msg); };

    ws_.open(url.c_str());
}

void MoonrakerClient::disconnect() {
    should_reconnect_ = false;
    connected_ = false;
    ws_.close();
}

void MoonrakerClient::on_open() {
    spdlog::info("[Moonraker] Connected");
    connected_ = true;
    reconnect_delay_ms_ = 1000;

    if (on_connected) {
        anneal::ui::queue_update([this]() {
            if (on_connected) on_connected();
        });
    }
}

void MoonrakerClient::on_close() {
    bool was_connected = connected_.exchange(false);
    if (was_connected) {
        spdlog::warn("[Moonraker] Disconnected");
        if (on_disconnected) {
            anneal::ui::queue_update([this]() {
                if (on_disconnected) on_disconnected();
            });
        }
    }
}

void MoonrakerClient::on_message(const std::string& msg) {
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(msg);
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("[Moonraker] JSON parse error: {}", e.what());
        return;
    }

    // Handle JSON-RPC responses (have "id" field)
    if (data.contains("id") && !data["id"].is_null()) {
        int id = data["id"].get<int>();
        std::function<void(const nlohmann::json&)> callback;
        {
            std::lock_guard<std::mutex> lock(id_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                callback = std::move(it->second);
                pending_requests_.erase(it);
            }
        }
        if (callback) {
            if (data.contains("result")) {
                callback(data["result"]);
            } else if (data.contains("error")) {
                spdlog::warn("[Moonraker] RPC error for id {}: {}",
                             id, data["error"].dump());
            }
        }
        return;
    }

    // Handle notifications (have "method" field)
    if (data.contains("method")) {
        std::string method = data["method"].get<std::string>();

        if (method == "notify_status_update" && on_status_update) {
            // params is [status_dict, eventtime]
            if (data.contains("params") && data["params"].is_array() &&
                !data["params"].empty()) {
                const auto& status = data["params"][0];
                on_status_update(status);
            }
        }
    }
}

void MoonrakerClient::send_jsonrpc(const std::string& method,
                                    const nlohmann::json& params,
                                    int id) {
    if (!connected_) {
        spdlog::warn("[Moonraker] Not connected, dropping: {}", method);
        return;
    }

    nlohmann::json msg = {
        {"jsonrpc", "2.0"},
        {"method", method},
    };
    if (!params.is_null() && !params.empty()) {
        msg["params"] = params;
    }
    if (id > 0) {
        msg["id"] = id;
    }

    ws_.send(msg.dump());
}

void MoonrakerClient::subscribe(const nlohmann::json& objects) {
    int id;
    {
        std::lock_guard<std::mutex> lock(id_mutex_);
        id = next_id_++;
    }

    nlohmann::json params = {{"objects", objects}};

    // Store a no-op callback just to consume the response
    {
        std::lock_guard<std::mutex> lock(id_mutex_);
        pending_requests_[id] = [](const nlohmann::json& result) {
            spdlog::debug("[Moonraker] Subscription confirmed");
        };
    }

    send_jsonrpc("printer.objects.subscribe", params, id);
    spdlog::info("[Moonraker] Subscribed to {} objects", objects.size());
}

void MoonrakerClient::execute_gcode(const std::string& gcode) {
    int id;
    {
        std::lock_guard<std::mutex> lock(id_mutex_);
        id = next_id_++;
    }

    nlohmann::json params = {{"script", gcode}};
    send_jsonrpc("printer.gcode.script", params, id);
    spdlog::info("[Moonraker] GCode: {}", gcode);
}

void MoonrakerClient::query_config(
        std::function<void(const nlohmann::json&)> callback) {
    int id;
    {
        std::lock_guard<std::mutex> lock(id_mutex_);
        id = next_id_++;
        pending_requests_[id] = [callback](const nlohmann::json& result) {
            // Moonraker response path: result.status.configfile.config
            // (our on_message already extracts "result", so we get status.configfile.config)
            if (result.contains("status") &&
                result["status"].contains("configfile") &&
                result["status"]["configfile"].contains("config")) {
                callback(result["status"]["configfile"]["config"]);
            } else {
                spdlog::warn("[Moonraker] Config response missing expected path "
                             "status.configfile.config");
                if (!result.empty()) {
                    spdlog::debug("[Moonraker] Config response keys: {}",
                                  result.dump().substr(0, 200));
                }
            }
        };
    }

    // Query configfile requesting "config" key (raw string values)
    send_jsonrpc("printer.objects.query",
                 {{"objects", {{"configfile", {"config"}}}}}, id);
}

void MoonrakerClient::schedule_reconnect() {
    if (!should_reconnect_) return;
    spdlog::info("[Moonraker] Reconnecting in {}ms", reconnect_delay_ms_);

    // libhv handles reconnection internally via reconn_setting_t
    // This method exists for manual reconnection if needed
    reconnect_delay_ms_ = std::min(
        reconnect_delay_ms_ * 2, MAX_RECONNECT_DELAY_MS);
}

} // namespace anneal