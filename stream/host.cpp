// See host.h.
#include "host.h"

#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

extern "C" {
void http_cleanup(void);  // in http.c, not in http.h
}

namespace ftstream {
namespace {

// What a Frametop client may do on a Vibepollo host: list apps, view and launch them, and
// send mouse and keyboard input (crypto::PERM in Vibepollo's crypto.h).
constexpr uint32_t kPermMouse = 1u << 11, kPermKeyboard = 1u << 12;
constexpr uint32_t kPermList = 1u << 24, kPermView = 1u << 25, kPermLaunch = 1u << 26;
constexpr uint32_t kFrametopPerm = kPermList | kPermView | kPermLaunch | kPermMouse | kPermKeyboard;
constexpr const char *kNoId = "0123456789ABCDEF";  // libgamestream's id without uniqueid.dat

// Percent-encodes a query value (device ids are {GUID}, display names \\.\DISPLAYn).
std::string UrlEncode(const std::string &v) {
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

std::string ReadUniqueId(const std::string &dir) {
    char buf[17];
    std::strcpy(buf, kNoId);
    FILE *f = std::fopen((dir + "/uniqueid.dat").c_str(), "rb");
    if (f) {
        if (std::fread(buf, 1, 16, f) != 16) std::strcpy(buf, kNoId);
        std::fclose(f);
    }
    return buf;
}

// libgamestream gives every client the same id unless uniqueid.dat exists; hosts track
// pairing by it, so each client gets a random one first.
void NewUniqueId(const std::string &dir) {
    if (ReadUniqueId(dir) != kNoId) return;
    for (size_t at = dir.find('/', 1);; at = dir.find('/', at + 1)) {
        mkdir(dir.substr(0, at).c_str(), 0700);
        if (at == std::string::npos) break;
    }
    std::random_device rd;
    char id[17];
    std::snprintf(id, sizeof id, "%08X%08X", rd(), rd());
    FILE *f = std::fopen((dir + "/uniqueid.dat").c_str(), "wb");
    if (f) std::fwrite(id, 1, 16, f), std::fclose(f);
}

// The attribute `name` of the response's root element.
std::string Attribute(const char *xml, size_t len, const char *name) {
    const std::string s(xml, len), key = std::string(name) + "=\"";
    const size_t at = s.find(key);
    if (at == std::string::npos) return "";
    const size_t end = s.find('"', at + key.size());
    return s.substr(at + key.size(), end - at - key.size());
}

size_t Append(char *data, size_t size, size_t n, void *out) {
    static_cast<std::string *>(out)->append(data, size * n);
    return size * n;
}

}  // namespace

std::string KeyDir(const std::string &id) {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/frametop-stream/" + id;
}

std::string TokenFile(const std::string &host) { return KeyDir("hosts") + "/" + host + ".token"; }

bool WebApi::Load(const std::string &host, const std::string &file) {
    std::ifstream f(file);
    if (!(f >> token)) return false;
    base = "https://" + host + ":47990";
    // The Web UI's public key as Remote Displays saw it when signing in (HOST.pin next to
    // HOST.token): another one is refused. Without one, any (a host from make-frametop-token).
    const std::string pinFile = file.size() > 6 && file.compare(file.size() - 6, 6, ".token") == 0
                                    ? file.substr(0, file.size() - 6) + ".pin" : "";
    std::ifstream p(pinFile);
    if (!(p >> pin)) pin.clear();
    return true;
}

long WebApi::Call(const char *method, const std::string &path, const nlohmann::json *body, nlohmann::json *out) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    std::string text;
    const std::string payload = body ? body->dump() : "";
    curl_slist *headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, (base + path).c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    // The Web UI's certificate is self-signed: its public key is pinned instead (Load).
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    if (!pin.empty()) curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY, pin.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Append);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &text);
    long code = -1;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    *out = nlohmann::json::parse(text, nullptr, false);
    return code;
}

bool WebApi::Clients(std::map<std::string, std::pair<std::string, uint32_t>> *clients) {
    nlohmann::json r;
    const long code = Call("GET", "/api/clients/list", nullptr, &r);
    if (code != 200 || !r.is_object() || !r.contains("named_certs"))
        return std::fprintf(stderr, "web api: clients/list: %ld\n", code), false;
    for (const auto &c : r["named_certs"]) (*clients)[c.value("uuid", "")] = {c.value("name", ""), c.value("perm", 0u)};
    return true;
}

// Vibepollo's update replaces the whole record, so every field it takes is given.
bool WebApi::Grant(const std::string &uuid, const std::string &name) {
    const nlohmann::json body = {{"uuid", uuid},
                                 {"name", name},
                                 {"perm", kFrametopPerm},
                                 {"display_mode", ""},
                                 {"output_name_override", ""},
                                 {"virtual_display_mode", ""},
                                 {"virtual_display_layout", ""},
                                 {"always_use_virtual_display", false},
                                 {"enable_legacy_ordering", true},
                                 {"allow_client_commands", false},
                                 {"do", nlohmann::json::array()},
                                 {"undo", nlohmann::json::array()}};
    nlohmann::json r;
    const long code = Call("POST", "/api/clients/update", &body, &r);
    if (code != 200 || !r.is_object() || !r.value("status", false))
        return std::fprintf(stderr, "web api: clients/update: %ld\n", code), false;
    return true;
}

// libgamestream keeps one curl handle whose connections stay open (it forbids reuse only
// on FreeBSD). Vibepollo closes an HTTPS connection with a blocking TLS shutdown on its only
// HTTPS thread, so a connection left idle here freezes its API for every client until this
// process exits. So every request is followed by dropping the handle, and with it the
// connection.
void Client::DropConnection() {
    http_cleanup();
    http_init(dir.c_str(), 0);
}

bool Client::Connect(const std::string &h, const std::string &name) {
    host = h, id = name, dir = KeyDir(id);
    const mode_t old = umask(077);  // the client's private key
    NewUniqueId(dir);
    const int r = gs_init(&server, host.data(), 0, dir.c_str(), 0, true);
    umask(old);
    if (r != GS_OK) return std::fprintf(stderr, "can't reach %s: %s\n", host.c_str(), gs_error ? gs_error : "?"), false;
    DropConnection();
    uniqueId = ReadUniqueId(dir);
    return true;
}

bool Client::Pair(WebApi *api) {
    std::random_device rd;
    char pin[5];
    std::snprintf(pin, sizeof pin, "%04u", rd() % 10000);
    if (!api) {
        std::printf("PIN %s: enter it in the host's Web UI (PIN pairing), with the device name %s\n", pin, id.c_str());
        std::fflush(stdout);
        if (gs_pair(&server, pin) != GS_OK) return std::fprintf(stderr, "pairing failed: %s\n", gs_error ? gs_error : "?"), false;
        std::printf("paired. Now allow it to launch apps and use the mouse and keyboard in the Web UI (Clients).\n");
        return true;
    }
    std::map<std::string, std::pair<std::string, uint32_t>> before, after;
    if (!api->Clients(&before)) return false;
    // gs_pair waits for the PIN, so it runs beside the request that gives it.
    std::atomic<int> result{GS_FAILED};
    std::atomic<bool> done{false};
    std::thread pairing([&] { result = gs_pair(&server, pin), done = true; });
    bool given = false;
    for (int i = 0; i < 40 && !given && !done; ++i) {
        usleep(250'000);
        const nlohmann::json body = {{"pin", pin}, {"name", id}};
        nlohmann::json r;
        given = api->Call("POST", "/api/pin", &body, &r) == 200 && r.is_object() && r.value("status", false);
    }
    if (!given) {
        std::fprintf(stderr, "the host took no PIN for this pairing (token scopes?)\n");
        std::fflush(stderr);
        std::_Exit(1);  // gs_pair is still waiting
    }
    pairing.join();
    DropConnection();
    if (result != GS_OK) return std::fprintf(stderr, "pairing failed: %s\n", gs_error ? gs_error : "?"), false;
    if (!api->Clients(&after)) return false;
    std::string uuid;
    for (const auto &[u, c] : after)
        if (!before.count(u) && c.first == id) uuid = u;
    if (uuid.empty()) return std::fprintf(stderr, "paired, but the host lists no new client named %s\n", id.c_str()), false;
    if (!api->Grant(uuid, id)) return false;
    std::printf("paired as %s (host id %s), allowed to launch and to use the mouse and keyboard\n", id.c_str(), uuid.c_str());
    return true;
}

bool Client::GrantByName(WebApi &api) {
    std::map<std::string, std::pair<std::string, uint32_t>> clients;
    if (!api.Clients(&clients)) return false;
    std::vector<std::string> named;
    for (const auto &[u, c] : clients)
        if (c.first == id) named.push_back(u);
    if (named.size() != 1) return std::fprintf(stderr, "the host has %zu clients named %s\n", named.size(), id.c_str()), false;
    if (!api.Grant(named[0], id)) return false;
    std::printf("%s may now launch and use the mouse and keyboard (perm was %u)\n", id.c_str(), clients[named[0]].second);
    return true;
}

bool Client::Unpair() {
    const int r = gs_unpair(&server);
    DropConnection();
    std::printf("unpair: %s\n", r == GS_OK ? "done" : gs_error ? gs_error : "failed");
    return r == GS_OK;
}

int Client::FindApp(const std::string &want) {
    if (want == "monitor") return kRemoteMonitor;
    if (want.rfind("display:", 0) == 0) return display = want.substr(8), kFrametopDisplay;
    if (!want.empty() && std::all_of(want.begin(), want.end(), ::isdigit)) return std::atoi(want.c_str());
    const std::string name = want == "primary" ? kPrimaryApp : want;
    PAPP_LIST list = nullptr;
    const int listed = gs_applist(&server, &list);
    DropConnection();
    if (listed != GS_OK) return std::fprintf(stderr, "can't list the host's apps: %s\n", gs_error), 0;
    for (PAPP_LIST a = list; a; a = a->next)
        if (name == a->name) return a->id;
    std::fprintf(stderr, "the host has no app named \"%s\"\n", name.c_str());
    return 0;
}

// As gs_start_app does it, but without its mode check and with the status code kept.
int Client::Launch(STREAM_CONFIGURATION &cfg, int appId, bool resume, std::string *message) {
    std::random_device rd;
    for (auto &b : cfg.remoteInputAesKey) b = char(rd());
    std::memset(cfg.remoteInputAesIv, 0, sizeof cfg.remoteInputAesIv);
    uint32_t rikeyid = rd();
    std::memcpy(cfg.remoteInputAesIv, &rikeyid, sizeof rikeyid);
    char rikey[33];
    for (int i = 0; i < 16; ++i) std::snprintf(rikey + 2 * i, 3, "%02x", uint8_t(cfg.remoteInputAesKey[i]));
    const std::string extra = appId == kFrametopDisplay ? "&frametopDisplay=" + UrlEncode(display) : "";
    char url[4096];
    std::snprintf(url, sizeof url,
                  "https://%s:%u/%s?uniqueid=%s&uuid=%08x-0000-4000-8000-%012x&appid=%d&mode=%dx%dx%d&additionalStates=1"
                  "&sops=0&rikey=%s&rikeyid=%d&localAudioPlayMode=1&surroundAudioInfo=%d&remoteControllersBitmap=0&gcmap=0%s%s",
                  server.serverInfo.address, server.httpsPort, resume ? "resume" : "launch", uniqueId.c_str(), rd(), rd(),
                  appId, cfg.width, cfg.height, cfg.fps, rikey, int(htonl(rikeyid)),
                  SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(cfg.audioConfiguration), LiGetLaunchUrlQueryParameters(),
                  extra.c_str());
    PHTTP_DATA data = http_create_data();
    if (!data) return -1;
    int code = -1;
    if (http_request(url, data) == GS_OK) {
        const std::string c = Attribute(data->memory, data->size, "status_code");
        code = c.empty() ? -1 : std::atoi(c.c_str());
        *message = Attribute(data->memory, data->size, "status_message");
        char *session = nullptr;
        if (code == 200 && xml_search(data->memory, data->size, const_cast<char *>("sessionUrl0"), &session) == GS_OK)
            server.serverInfo.rtspSessionUrl = session;
    } else {
        *message = gs_error ? gs_error : "request failed";
    }
    http_free_data(data);
    DropConnection();
    return code;
}

}  // namespace ftstream
