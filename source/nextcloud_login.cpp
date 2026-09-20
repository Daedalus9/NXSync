#include "nxsync/nextcloud_login.hpp"

#include <curl/curl.h>
#include "cJSON.h"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace nxsync {
namespace {
constexpr std::uint64_t LoginLifetimeMs = 20 * 60 * 1000;
constexpr std::uint64_t PollIntervalMs = 2000;
constexpr std::size_t MaximumBody = 16 * 1024;

void wipe(std::string& text) {
    volatile char* bytes = text.empty() ? nullptr : &text[0];
    for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = 0;
    text.clear();
}

bool safeText(const std::string& text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum
        && std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
}

std::string part(CURLU* url, CURLUPart field, unsigned flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url, field, &value, flags) != CURLUE_OK) return {};
    std::string result(value);
    curl_free(value);
    return result;
}

std::string origin(const std::string& address, bool serverAddress = false) {
    if (!safeText(address, 2048) || address.find(' ') != std::string::npos) return {};
    CURLU* url = curl_url();
    if (!url) return {};
    std::string result;
    if (curl_url_set(url, CURLUPART_URL, address.c_str(), 0) == CURLUE_OK
        && part(url, CURLUPART_SCHEME) == "https" && !part(url, CURLUPART_HOST).empty()
        && part(url, CURLUPART_USER).empty() && part(url, CURLUPART_PASSWORD).empty()
        && part(url, CURLUPART_FRAGMENT).empty()
        && (!serverAddress || part(url, CURLUPART_QUERY).empty())) {
        result = part(url, CURLUPART_HOST) + ":" + part(url, CURLUPART_PORT, CURLU_DEFAULT_PORT);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    }
    curl_url_cleanup(url);
    return result;
}

bool sameOrigin(const std::string& address, const std::string& server) {
    const auto value = origin(address);
    return !value.empty() && value == origin(server, true);
}

void wipeJson(cJSON* node) {
    if (!node) return;
    if (node->valuestring) {
        const auto size = std::strlen(node->valuestring);
        volatile char* bytes = node->valuestring;
        for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
    }
    for (auto* child = node->child; child; child = child->next) wipeJson(child);
}
struct JsonDeleter { void operator()(cJSON* node) const { wipeJson(node); cJSON_Delete(node); } };
using Json = std::unique_ptr<cJSON, JsonDeleter>;

bool uniqueKeys(const cJSON* node) {
    std::set<std::string> keys;
    for (const auto* child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node) && (!child->string || !keys.insert(child->string).second)) return false;
        if (!uniqueKeys(child)) return false;
    }
    return true;
}

Json parseJson(const std::string& text) {
    // Bound nesting before calling the upstream recursive parser.
    int depth = 0;
    bool quoted = false, escaped = false;
    for (unsigned char ch : text) {
        if (ch == 0) return {};
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 16) return {}; }
        else if (ch == '}' || ch == ']') { if (--depth < 0) return {}; }
    }
    if (text.size() > MaximumBody || text.find("\\u0000") != std::string::npos) return {};
    Json parsed(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true));
    if (!parsed || !cJSON_IsObject(parsed.get()) || !uniqueKeys(parsed.get())) return {};
    return parsed;
}

std::string field(const cJSON* object, const char* name) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
} // namespace

std::string normalizeLoginServer(const std::string& address) {
    if (origin(address, true).empty()) return {};
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    if (curl_url_set(parsed, CURLUPART_URL, address.c_str(), 0) != CURLUE_OK) {
        curl_url_cleanup(parsed); return {};
    }
    std::string result = part(parsed, CURLUPART_URL);
    curl_url_cleanup(parsed);
    const auto dav = result.find("/remote.php/dav/files/");
    if (dav != std::string::npos) result.erase(dav);
    while (!result.empty() && result.back() == '/') result.pop_back();
    const std::string index = "/index.php";
    if (result.size() >= index.size() && result.compare(result.size() - index.size(), index.size(), index) == 0)
        result.erase(result.size() - index.size());
    return result;
}

struct NextcloudLogin::Impl {
    enum class Request { Start, Poll, User };
    LoginState state{LoginState::Cancelled};
    Request request{Request::Start};
    std::string server, login, token, pollEndpoint, status, body, post, caFile;
    LoginCredentials credentials;
    std::uint64_t started{0}, nextRequest{0};
    CURLM* multi{nullptr};
    CURL* easy{nullptr};
    curl_slist* headers{nullptr};
    bool curlInitialized{false}, socketsInitialized{false}, nifmInitialized{false};

    void closeRequest() {
        if (easy) { if (multi) curl_multi_remove_handle(multi, easy); curl_easy_cleanup(easy); easy = nullptr; }
        curl_slist_free_all(headers); headers = nullptr;
        wipe(post);
    }
    void clearSecrets() {
        wipe(token); wipe(login); wipe(pollEndpoint); wipe(body); wipe(post);
        wipe(credentials.appPassword); wipe(credentials.loginName); wipe(credentials.davUserId);
        credentials.server.clear();
    }
    void fail(const std::string& message) {
        closeRequest(); clearSecrets(); state = LoginState::Error; status = message;
    }
    ~Impl() {
        closeRequest(); clearSecrets();
        if (multi) curl_multi_cleanup(multi);
        if (curlInitialized) curl_global_cleanup();
#ifdef __SWITCH__
        if (nifmInitialized) nifmExit();
        if (socketsInitialized) socketExit();
#endif
    }
    static std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (size != 0 && count > MaximumBody / size) return 0;
        const auto length = size * count;
        if (length > MaximumBody - self.body.size()) return 0;
        self.body.append(data, length);
        return length;
    }
    bool send(Request kind) {
        closeRequest(); wipe(body); request = kind;
        easy = curl_easy_init();
        if (!easy) { fail("Unable to create the login request"); return false; }
        const auto url = kind == Request::Start ? server + "/index.php/login/v2"
            : kind == Request::Poll ? pollEndpoint : credentials.server + "/ocs/v2.php/cloud/user?format=json";
        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "NXSync/0.30.12-rc2 (Nintendo Switch)");
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!caFile.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, caFile.c_str());
        // Never redirect a polling token or an application password elsewhere.
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
        headers = curl_slist_append(headers, "Accept: application/json");
        if (kind == Request::User) {
            headers = curl_slist_append(headers, "OCS-APIRequest: true");
            curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(easy, CURLOPT_USERNAME, credentials.loginName.c_str());
            curl_easy_setopt(easy, CURLOPT_PASSWORD, credentials.appPassword.c_str());
        } else {
            if (kind == Request::Poll) {
                char* encoded = curl_easy_escape(easy, token.c_str(), static_cast<int>(token.size()));
                if (!encoded) { fail("Unable to prepare login polling"); return false; }
                post = "token=" + std::string(encoded);
                volatile char* bytes = encoded;
                for (std::size_t i = 0, n = std::strlen(encoded); i < n; ++i) bytes[i] = 0;
                curl_free(encoded);
            }
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post.c_str());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(post.size()));
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            fail("Unable to start the login request"); return false;
        }
        return true;
    }
    void completed(CURLcode result, long http, std::uint64_t now) {
        const auto kind = request;
        closeRequest();
        if (result != CURLE_OK) {
            const bool retryable = result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT
                || result == CURLE_OPERATION_TIMEDOUT || result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR;
            if (kind != Request::Start && retryable) {
                wipe(body); status = "Network interrupted; retrying..."; nextRequest = now + PollIntervalMs; return;
            }
            fail("HTTPS failed. Check network, certificate and console clock."); return;
        }
        if ((kind == Request::Poll && http == 404)
            || (kind != Request::Start && (http == 429 || http >= 500))) {
            wipe(body); status = http == 404 ? "Waiting for authorization on your phone"
                : "Server temporarily unavailable; retrying...";
            nextRequest = now + PollIntervalMs; return;
        }
        if (http != 200) {
            fail(http >= 300 && http < 400 ? "Server redirected the request. Enter its final HTTPS address."
                : "Nextcloud login was not accepted (HTTP " + std::to_string(http) + ")"); return;
        }
        auto json = parseJson(body);
        wipe(body);
        if (!json) { fail("Nextcloud returned an invalid login response"); return; }
        if (kind == Request::Start) {
            const auto* poll = cJSON_GetObjectItemCaseSensitive(json.get(), "poll");
            login = field(json.get(), "login"); token = field(poll, "token"); pollEndpoint = field(poll, "endpoint");
            if (!safeText(token, 4096) || !safeText(login, 1024)
                || !sameOrigin(login, server) || !sameOrigin(pollEndpoint, server)) {
                fail("Nextcloud returned an unsafe or incomplete login session"); return;
            }
            state = LoginState::Waiting; status = "Waiting for authorization on your phone";
            nextRequest = now;
        } else if (kind == Request::Poll) {
            credentials.server = normalizeLoginServer(field(json.get(), "server"));
            credentials.loginName = field(json.get(), "loginName");
            credentials.appPassword = field(json.get(), "appPassword");
            if (!sameOrigin(credentials.server, server) || !safeText(credentials.loginName, 128)
                || !safeText(credentials.appPassword, 256)) {
                fail("Nextcloud returned invalid account credentials"); return;
            }
            wipe(token); wipe(pollEndpoint); wipe(login);
            state = LoginState::ResolvingUser; status = "Authorized. Checking your WebDAV account...";
            nextRequest = now;
        } else {
            const auto* ocs = cJSON_GetObjectItemCaseSensitive(json.get(), "ocs");
            const auto* meta = cJSON_GetObjectItemCaseSensitive(ocs, "meta");
            const auto* data = cJSON_GetObjectItemCaseSensitive(ocs, "data");
            credentials.davUserId = field(data, "id");
            if (field(meta, "status") != "ok" || !safeText(credentials.davUserId, 128)) {
                fail("Unable to identify the Nextcloud WebDAV user. Try again or use manual setup."); return;
            }
            state = LoginState::Authorized; status = "Account authorized";
        }
    }
};

NextcloudLogin::NextcloudLogin() : impl_(new Impl()) {}
NextcloudLogin::~NextcloudLogin() = default;

bool NextcloudLogin::begin(const std::string& address, std::uint64_t now, const std::string& trustedCaFile) {
    impl_.reset(new Impl());
    auto& self = *impl_;
    self.started = now; self.caFile = trustedCaFile;
    self.server = normalizeLoginServer(address);
    if (self.server.empty()) { self.fail("Invalid server address. Use HTTPS without credentials or query parameters."); return false; }
#ifdef __SWITCH__
    u64 clock = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &clock)) || clock < 1704067200ULL || clock > 2366841599ULL) {
        self.fail("Correct the console date and time before connecting"); return false;
    }
    if (R_FAILED(socketInitializeDefault())) { self.fail("Unable to initialize sockets"); return false; }
    self.socketsInitialized = true;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) { self.fail("Unable to initialize the network"); return false; }
    self.nifmInitialized = true;
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { self.fail("Unable to initialize HTTPS"); return false; }
    self.curlInitialized = true; self.multi = curl_multi_init();
    if (!self.multi) { self.fail("Unable to initialize the login session"); return false; }
    self.state = LoginState::Starting; self.status = "Contacting Nextcloud...";
    return self.send(Impl::Request::Start);
}

void NextcloudLogin::update(std::uint64_t now) {
    auto& self = *impl_;
    if (self.state != LoginState::Starting && self.state != LoginState::Waiting && self.state != LoginState::ResolvingUser) return;
    if (now < self.started || now - self.started >= LoginLifetimeMs) {
        cancel(); self.state = LoginState::Expired; self.status = "Login expired. Start again to generate a new QR code."; return;
    }
    if (!self.easy && now >= self.nextRequest) {
        if (!self.send(self.state == LoginState::Waiting ? Impl::Request::Poll : Impl::Request::User)) return;
    }
    if (!self.easy) return;
    int running = 0;
    if (curl_multi_perform(self.multi, &running) != CURLM_OK) { self.fail("HTTPS login could not continue"); return; }
    int remaining = 0;
    while (auto* message = curl_multi_info_read(self.multi, &remaining)) {
        if (message->msg != CURLMSG_DONE) continue;
        const auto result = message->data.result;
        long http = 0;
        curl_easy_getinfo(self.easy, CURLINFO_RESPONSE_CODE, &http);
        self.completed(result, http, now);
        break;
    }
}

void NextcloudLogin::cancel() {
    impl_->closeRequest(); impl_->clearSecrets(); impl_->state = LoginState::Cancelled; impl_->status = "Login cancelled";
}
LoginState NextcloudLogin::state() const { return impl_->state; }
const std::string& NextcloudLogin::server() const { return impl_->server; }
const std::string& NextcloudLogin::loginUrl() const { return impl_->login; }
const std::string& NextcloudLogin::message() const { return impl_->status; }
const LoginCredentials& NextcloudLogin::credentials() const { return impl_->credentials; }
unsigned NextcloudLogin::secondsRemaining(std::uint64_t now) const {
    return now >= impl_->started && now - impl_->started < LoginLifetimeMs
        ? static_cast<unsigned>((LoginLifetimeMs - (now - impl_->started) + 999) / 1000) : 0;
}
} // namespace nxsync
