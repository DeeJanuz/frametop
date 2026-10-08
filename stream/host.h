// The host side of a Frametop remote display (docs/remote-displays.md): one Moonlight client
// per display, paired with a Vibepollo host through its Web UI API token, and the requests
// that open and close the display's stream. Shared by ft-stream and the spike's ft-streamtest.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

extern "C" {
#include "client.h"
#include "errors.h"
#include "http.h"
}

namespace ftstream {

constexpr int kRemoteMonitor = 2147483505, kDisconnectMonitor = 2147483502;  // Vibepollo's remote_session.h
// frametop-vibepollo: stream an existing display, named by the launch argument frametopDisplay.
constexpr int kFrametopDisplay = 2147483521;
constexpr const char *kPrimaryApp = "Frametop primary display";

// ~/.local/share/frametop-stream/ID: one client's certificate, key and unique id.
std::string KeyDir(const std::string &id);
// The host's API token: ~/.local/share/frametop-stream/hosts/HOST.token.
std::string TokenFile(const std::string &host);

// The host's Web UI API (port 47990), with a scoped token.
struct WebApi {
    std::string base, token, pin;  // pin: the Web UI's public key, "sha256//BASE64" (curl's form)
    bool Load(const std::string &host, const std::string &file);
    // The HTTP status (or -1), with the response's JSON in *out.
    long Call(const char *method, const std::string &path, const nlohmann::json *body, nlohmann::json *out);
    // Paired clients by host id: name and permissions.
    bool Clients(std::map<std::string, std::pair<std::string, uint32_t>> *clients);
    // Lets a client launch apps and use the mouse and keyboard.
    bool Grant(const std::string &uuid, const std::string &name);
};

// One client (one display) of one host.
struct Client {
    std::string host, id, dir, uniqueId;
    SERVER_DATA server{};
    std::string display;  // a frametopDisplay launch's device id

    // Makes the key directory and a unique id if needed, then asks the host about itself.
    bool Connect(const std::string &host, const std::string &id);
    // Pairs this client: with the token, by sending its own PIN through the API and then
    // granting it launch, mouse and keyboard; without one, by printing the PIN to type in.
    bool Pair(WebApi *api);
    bool GrantByName(WebApi &api);
    bool Unpair();
    // The app id for primary, monitor, display:DEVICE, a number, or an app's name; 0 if none.
    int FindApp(const std::string &want);
    // /launch (or /resume) for appId, keeping the status code: Vibepollo answers 503 while a
    // Remote Monitor is being set up.
    int Launch(STREAM_CONFIGURATION &cfg, int appId, bool resume, std::string *message);
    // libgamestream's connection is dropped after every request (see host.cpp).
    void DropConnection();
};

}  // namespace ftstream
