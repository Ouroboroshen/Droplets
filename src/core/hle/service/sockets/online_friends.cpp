// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "core/hle/service/sockets/online_friends.h"
#include "core/hle/service/sockets/online_redirect.h"

namespace Service::Sockets::OnlineRedirect {

namespace {

// The certificate as PEM, which the TLS library reads (players get ca.der or ca.pem)
std::string CaPem(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream file(std::filesystem::path{Common::FS::ToU8String(path)}, std::ios::binary);
    std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (data.size() > 64 * 1024) {
        return {};
    }
    if (data.empty() || data.find("-----BEGIN CERTIFICATE-----") != std::string::npos) {
        return data;
    }
    static constexpr char Digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (size_t i = 0, column = 0; i < data.size(); i += 3) {
        const u32 chunk = (static_cast<u8>(data[i]) << 16) |
                          (i + 1 < data.size() ? static_cast<u8>(data[i + 1]) << 8 : 0) |
                          (i + 2 < data.size() ? static_cast<u8>(data[i + 2]) : 0);
        pem += Digits[(chunk >> 18) & 63];
        pem += Digits[(chunk >> 12) & 63];
        pem += i + 1 < data.size() ? Digits[(chunk >> 6) & 63] : '=';
        pem += i + 2 < data.size() ? Digits[chunk & 63] : '=';
        if ((column += 4) == 64 || i + 3 >= data.size()) {
            pem += '\n';
            column = 0;
        }
    }
    return pem + "-----END CERTIFICATE-----\n";
}

std::vector<FriendEntry> Entries(const nlohmann::json& view, const char* key) {
    std::vector<FriendEntry> out;
    if (!view.contains(key) || !view[key].is_array()) {
        return out;
    }
    for (const auto& item : view[key]) {
        if (!item.is_object()) {
            continue;
        }
        FriendEntry entry;
        entry.code = item.value("code", std::string{});
        entry.name = item.value("name", std::string{});
        entry.online = item.value("online", false);
        if (entry.code.size() == 12) {
            out.push_back(std::move(entry));
        }
    }
    return out;
}

// "name[:port]" or "a.b.c.d[:port]", as online play reads it: the friends API is on port + 1
bool ApiAddress(const std::string& server, std::string& host, int& port) {
    host = server;
    unsigned online_port = 443;
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        online_port = static_cast<unsigned>(std::strtoul(host.c_str() + colon + 1, nullptr, 10));
        host.resize(colon);
    }
    port = static_cast<int>(online_port + 1);
    return !host.empty() && online_port != 0 && online_port < 65535;
}

FriendLists Call(const Profile& profile, const std::string& ca, const std::string& path,
                 const std::string* body) {
    FriendLists out;
    std::string host;
    int port = 0;
    if (!ApiAddress(profile.server, host, port)) {
        out.error = "the server address isn't name:port";
        return out;
    }
    const std::string pem = CaPem(ca.empty() ? ServerCa() : ca);
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
    // The certificate names Nintendo's hosts (the ones games connect to), not this address: the
    // server's own CA, the only one trusted here, is the check
    client.enable_server_hostname_verification(false);
    const httplib::Headers headers{{"X-Droplet-Code", profile.code},
                                   {"X-Droplet-Secret", profile.secret},
                                   {"X-Droplet-Version", Common::g_build_version}};
    const auto result = body ? client.Post(path, headers, *body, "application/json")
                             : client.Get(path, headers);
    if (!result) {
        out.error = "couldn't reach the server's friend list (" + httplib::to_string(result.error()) + ")";
        return out;
    }
    const auto view = nlohmann::json::parse(result->body, nullptr, false);
    if (view.is_discarded() || !view.is_object()) {
        out.error = result->status == 404 ? "this server has no friend requests (an older server)"
                                           : "the server's answer wasn't readable";
        return out;
    }
    out.reached = true;
    // A field of another type throws, which would end the program from the settings page's thread
    try {
        if (result->status != 200) {
            out.error = view.value("error", std::string{"the server refused"});
            return out;
        }
        out.friends = Entries(view, "friends");
        out.received = Entries(view, "received");
        out.sent = Entries(view, "sent");
    } catch (const nlohmann::json::exception&) {
        out = {};
        out.error = "the server's answer wasn't readable";
    }
    return out;
}

// GET `path` from the server's friends API port, trusting only its certificate: the body, or
// empty and why not.
std::string Get(const std::string& server, const std::string& path, const httplib::Headers& headers,
                std::string& error) {
    std::string host;
    int port = 0;
    if (!ApiAddress(server, host, port)) {
        error = "the server address isn't name:port";
        return {};
    }
    const std::string pem = CaPem(ServerCa());
    if (pem.empty()) {
        error = "the server's certificate isn't set";
        return {};
    }
    httplib::SSLClient client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    client.load_ca_cert_store(pem.data(), pem.size());
    client.enable_server_certificate_verification(true);
    client.enable_server_hostname_verification(false); // as in Call: the server's CA is the check
    const auto result = client.Get(path, headers);
    if (!result) {
        error = "couldn't reach the server (" + httplib::to_string(result.error()) + ")";
        return {};
    }
    if (result->status != 200) {
        error = result->status == 404 ? "the server has no " + path + " (an older server)"
                                      : "the server answered " + std::to_string(result->status);
        return {};
    }
    return result->body;
}

} // namespace

bool FriendsApiAddress(const std::string& server, std::string& host, int& port) {
    return ApiAddress(server, host, port);
}

std::string ServerCaPem() {
    return CaPem(ServerCa());
}

std::string FetchServerCa(const std::string& server) {
    std::string host;
    int port = 0;
    if (!ApiAddress(server, host, port)) {
        return "the server address isn't name:port";
    }
    httplib::SSLClient client(host, port);
    client.set_connection_timeout(3);
    client.set_read_timeout(5);
    client.enable_server_certificate_verification(false);
    const auto result = client.Get("/v1/ca.der");
    if (!result) {
        return "the server didn't answer on port " + std::to_string(port) + " (" +
               httplib::to_string(result.error()) + ")";
    }
    const std::string& der = result->body;
    // A DER certificate: an X.509 SEQUENCE of a few hundred bytes to a few kilobytes
    if (result->status != 200 || der.size() < 64 || der.size() > 16 * 1024 ||
        static_cast<u8>(der[0]) != 0x30) {
        return result->status == 404 ? "this server doesn't hand out its certificate (an older server)"
                                     : "the server's answer wasn't a certificate";
    }
    const auto path = SavedServerCa(server);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto part = std::filesystem::path{path}.concat(".part");
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out.write(der.data(), static_cast<std::streamsize>(der.size()))) {
            return "couldn't save the certificate in " + Common::FS::PathToUTF8String(path.parent_path());
        }
    }
    std::filesystem::rename(part, path, ec);
    if (ec) {
        return "couldn't save the certificate in " + Common::FS::PathToUTF8String(path.parent_path());
    }
    return {};
}

std::string FetchPublic(const std::string& server, const std::string& path, std::string& error) {
    return Get(server, path, {{"X-Droplet-Version", Common::g_build_version}}, error);
}

namespace {

// A version as a server reports it, if it looks like one: shown to the player, so nothing else.
std::string CheckedVersion(const nlohmann::json& answer, const char* key) {
    const auto it = answer.find(key);
    if (it == answer.end() || !it->is_string()) {
        return {};
    }
    const std::string text = it->get<std::string>();
    const bool ok = !text.empty() && text.size() <= 32 &&
                    std::all_of(text.begin(), text.end(), [](char c) {
                        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '+' ||
                               c == '-';
                    });
    return ok ? text : std::string{};
}

void AskVersion(const std::string& server) {
    httplib::Headers headers{{"X-Droplet-Version", Common::g_build_version}};
    if (const auto profile = LoadProfile(server, false)) {
        headers.emplace("X-Droplet-Code", profile->code);
        headers.emplace("X-Droplet-Secret", profile->secret);
    }
    std::string error;
    const std::string body = Get(server, "/v1/hello", headers, error);
    const auto answer = nlohmann::json::parse(body, nullptr, false);
    if (body.empty() || !answer.is_object()) {
        LOG_INFO(Network, "No version check from {}: {}", server,
                 error.empty() ? "unreadable answer" : error);
        return;
    }
    // Only the status and the server's version count; the sentence the player reads is Droplet's
    // own (the windowed app's ShowVersionNotice), never the server's text.
    const auto status = answer.find("status");
    const std::string state = status != answer.end() && status->is_string() ? status->get<std::string>() : "";
    const std::string server_version = CheckedVersion(answer, "server_version");
    const bool droplet_behind = state == "emulator_outdated";
    if ((!droplet_behind && state != "server_outdated") || server_version.empty()) {
        // Known words only: a server's text could forge log lines
        const char* const shown = state == "match"     ? "match"
                                  : state == "unknown" ? "unknown"
                                  : state.empty()      ? "no status"
                                                       : "unexpected status";
        LOG_INFO(Network, "Version check with {}: {} (server {}, Droplet {})", server, shown,
                 server_version.empty() ? "?" : server_version, Common::g_build_version);
        return;
    }
    LOG_WARNING(Network, "{}: this Droplet ({}) {} the online server ({})", server,
                Common::g_build_version, droplet_behind ? "is older than" : "is newer than",
                server_version);
    {
        std::scoped_lock lock{version_notice_mutex};
        version_notice = {droplet_behind, server_version};
    }
    version_notices.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void CheckVersionOnce() {
    static std::mutex mutex;
    static std::set<std::string> asked;
    const std::string server = ServerAddress();
    if (server.empty()) {
        return;
    }
    {
        std::scoped_lock lock{mutex};
        if (asked.contains(server)) {
            return;
        }
        // Before the server's certificate is here (a first run, until EnsureOnlineCa fetches it)
        // the answer couldn't be trusted: a later call asks.
        if (CaPem(ServerCa()).empty()) {
            return;
        }
        asked.insert(server);
    }
    std::thread([server] {
        // Nothing may escape this thread: an uncaught exception would end the game.
        try {
            AskVersion(server);
        } catch (const std::exception& e) {
            LOG_WARNING(Network, "Version check with {} failed: {}", server, e.what());
        } catch (...) {
            LOG_WARNING(Network, "Version check with {} failed", server);
        }
    }).detach();
}

FriendLists FetchFriends(const Profile& profile, const std::string& ca) {
    return Call(profile, ca, "/v1/friends", nullptr);
}

FriendLists FriendAction(const Profile& profile, const std::string& action, const std::string& code,
                         const std::string& ca) {
    const std::string body = nlohmann::json{{"code", code}}.dump();
    return Call(profile, ca, "/v1/friends/" + action, &body);
}

} // namespace Service::Sockets::OnlineRedirect
