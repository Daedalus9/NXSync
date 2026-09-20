#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nxsync {

enum class LoginState { Starting, Waiting, ResolvingUser, Authorized, Error, Expired, Cancelled };

struct LoginCredentials {
    std::string server;
    std::string loginName;
    std::string appPassword;
    std::string davUserId;
};

// Accept an instance URL or an existing personal WebDAV URL. Credentials and
// query/fragment parameters are never accepted in a server address.
std::string normalizeLoginServer(const std::string& address);

// Nonblocking HTTPS Login Flow v2. Call update from the UI loop; no browser or
// callback listener is needed on the console. All session secrets stay in RAM.
class NextcloudLogin {
public:
    NextcloudLogin();
    ~NextcloudLogin();
    NextcloudLogin(const NextcloudLogin&) = delete;
    NextcloudLogin& operator=(const NextcloudLogin&) = delete;

    bool begin(const std::string& server, std::uint64_t nowMs,
               const std::string& trustedCaFile = {});
    void update(std::uint64_t nowMs);
    void cancel();
    LoginState state() const;
    const std::string& server() const;
    const std::string& loginUrl() const;
    const std::string& message() const;
    const LoginCredentials& credentials() const;
    unsigned secondsRemaining(std::uint64_t nowMs) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nxsync
