// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/scm_rev.h"
#include "core/hle/service/sockets/online_friends.h"
#include "core/hle/service/sockets/online_notes.h"

namespace Service::Sockets::OnlineRedirect {

namespace {

// What the window shows comes from the server, so every field is checked and bounded before use.
constexpr size_t MaxText = 64;
constexpr size_t MaxRows = 64;

std::string Text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    std::string text = it->get<std::string>();
    if (text.size() > MaxText) {
        text.resize(MaxText);
    }
    return text;
}

double Number(const nlohmann::json& object, const char* key, double fallback) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return fallback;
    }
    const double value = it->get<double>();
    return std::isfinite(value) ? value : fallback;
}

bool Flag(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

bool Three(const nlohmann::json& object, const char* key, double (&out)[3]) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() != 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number()) {
            return false;
        }
        out[i] = (*it)[i].get<double>();
        if (!std::isfinite(out[i])) {
            return false;
        }
    }
    return true;
}

void Read(const nlohmann::json& view, NotesView& out) {
    out.game = Text(view, "game");
    out.open = Flag(view, "open");
    out.live = Text(view, "live");
    if (const auto map = view.find("map"); map != view.end() && map->is_object()) {
        out.map_started = Flag(*map, "started");
        if (const auto player = map->find("player"); player != map->end() && player->is_object()) {
            NotesPlayer p;
            if (Three(*player, "pos", p.pos)) {
                Three(*player, "front", p.front);
                p.stage = Text(*player, "stage");
                p.zone = Text(*player, "zone");
                p.child = Flag(*player, "child");
                p.stopped = Flag(*player, "stopped");
                p.indoor = Flag(*player, "indoor");
                p.age = Number(*map, "age", 0);
                out.player = p;
            }
        }
    }
    if (const auto collected = view.find("collected"); collected != view.end() && collected->is_object()) {
        for (const auto& [name, counts] : collected->items()) {
            if (out.collected.size() >= MaxRows || name.size() > MaxText || !counts.is_object()) {
                continue;
            }
            const int bits = static_cast<int>(std::clamp(Number(counts, "bits", 0), 0.0, 4096.0));
            const int found = static_cast<int>(std::clamp(Number(counts, "found", 0), 0.0, double(bits)));
            out.collected.push_back({name, found, bits});
        }
    }
    if (const auto nearby = view.find("nearby"); nearby != view.end() && nearby->is_array()) {
        for (const auto& entry : *nearby) {
            if (out.nearby.size() >= MaxRows || !entry.is_object()) {
                break;
            }
            NotesNearby n;
            n.type = static_cast<int>(std::clamp(Number(entry, "type", -1), -1.0, 1e9));
            n.distance = Number(entry, "distance", -1);
            n.life = static_cast<int>(std::clamp(Number(entry, "life", -1), -1.0, 1e6));
            n.max_life = static_cast<int>(std::clamp(Number(entry, "max_life", -1), -1.0, 1e6));
            n.battle = Flag(entry, "battle");
            out.nearby.push_back(n);
        }
    }
}

NotesView Call(const Profile& profile, const std::string& path, bool post) {
    NotesView out;
    std::string host;
    int port = 0;
    if (!FriendsApiAddress(profile.server, host, port)) {
        out.error = "the server address isn't name:port";
        return out;
    }
    const std::string pem = ServerCaPem();
    if (pem.empty()) {
        out.error = "the server's certificate isn't set";
        return out;
    }
    httplib::SSLClient client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    client.set_write_timeout(10);
    client.load_ca_cert_store(pem.data(), pem.size());
    client.enable_server_certificate_verification(true);
    // As for friends and GameChat: the certificate names Nintendo's hosts, and the server's own
    // CA, the only one trusted here, is the check
    client.enable_server_hostname_verification(false);
    const httplib::Headers headers{{"X-Droplet-Code", profile.code},
                                   {"X-Droplet-Secret", profile.secret},
                                   {"X-Droplet-Version", Common::g_build_version}};
    const auto result = post ? client.Post(path, headers, "{}", "application/json") : client.Get(path, headers);
    if (!result) {
        out.error = "couldn't reach the server (" + httplib::to_string(result.error()) + ")";
        return out;
    }
    if (result->body.size() > 64 * 1024) {
        out.error = "the server's answer was too long";
        return out;
    }
    const auto view = nlohmann::json::parse(result->body, nullptr, false);
    if (view.is_discarded() || !view.is_object()) {
        out.error = result->status == 404 ? "this server has no ZELDA NOTES (an older server)"
                                           : "the server's answer wasn't readable";
        return out;
    }
    out.reached = true;
    // A field of another type throws, which would end the program from the window's thread
    try {
        if (result->status != 200) {
            out.error = Text(view, "error");
            if (out.error.empty()) {
                out.error = "the server refused";
            }
            out.game = Text(view, "game");
            return out;
        }
        Read(view, out);
    } catch (const nlohmann::json::exception&) {
        out = {};
        out.error = "the server's answer wasn't readable";
    }
    return out;
}

} // namespace

NotesView FetchNotes(const Profile& profile) {
    return Call(profile, "/v1/notes", false);
}

NotesView NotesAction(const Profile& profile, const std::string& action) {
    if (action != "start" && action != "stop") {
        NotesView out;
        out.error = "unknown action";
        return out;
    }
    return Call(profile, "/v1/notes/" + action, true);
}

} // namespace Service::Sockets::OnlineRedirect
