#include "nxsync/nextcloud_login.hpp"
#include "nxsync/nextcloud_paths.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    using namespace nxsync;
    assert(argc == 4);
    assert(normalizeLoginServer("http://example.com").empty());
    assert(normalizeLoginServer("https://user:secret@example.com").empty());
    assert(normalizeLoginServer("https://example.com?token=secret").empty());
    assert(normalizeLoginServer("https://example.com/#fragment").empty());
    assert(normalizeLoginServer("https://cloud.example.com/nextcloud/remote.php/dav/files/user/")
        == "https://cloud.example.com/nextcloud");
    assert(normalizeLoginServer("https://example.com/index.php/") == "https://example.com");
    NextcloudLogin login;
    const std::string scenario = argv[3];
    assert(login.begin(argv[1], 1000, scenario == "untrusted" ? "" : argv[2]));
    std::uint64_t now = 1000;
    bool waiting = false;
    for (int i = 0; i < 12000; ++i) {
        login.update(now);
        if (login.state() == LoginState::Waiting) {
            waiting = true;
            assert(login.loginUrl().find("/login/flow/test") != std::string::npos);
            assert(login.credentials().appPassword.empty());
            if (scenario == "cancel") { login.cancel(); break; }
            if (scenario == "expire") { login.update(1'201'000); break; }
        }
        if (login.state() == LoginState::Authorized || login.state() == LoginState::Error) break;
        now += 20; // Advance the poll clock without making the test wait two seconds each time.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (scenario == "ok" || scenario == "retry" || scenario == "user_retry") {
        if (login.state() != LoginState::Authorized) std::cerr << login.message() << '\n';
        assert(waiting && login.state() == LoginState::Authorized);
        assert(login.credentials().loginName == "person@example.test");
        assert(login.credentials().davUserId == "opaque-user-id");
        assert(login.credentials().appPassword == "synthetic-app-password");
        assert(makeNextcloudDavBaseUrl(login.credentials().server, login.credentials().davUserId)
            == std::string(argv[1]) + "/remote.php/dav/files/opaque-user-id");
        assert(login.loginUrl().empty());
        login.cancel();
    } else if (scenario == "cancel") assert(login.state() == LoginState::Cancelled);
    else if (scenario == "expire") assert(login.state() == LoginState::Expired);
    else {
        assert(login.state() == LoginState::Error);
        if (scenario == "cross_poll" || scenario == "cross_login" || scenario == "http_login" || scenario == "missing")
            assert(login.message().find("unsafe or incomplete") != std::string::npos);
        if (scenario == "cross_server" || scenario == "empty_password")
            assert(login.message().find("invalid account credentials") != std::string::npos);
        if (scenario == "redirect" || scenario == "poll_redirect" || scenario == "user_redirect")
            assert(login.message().find("redirected") != std::string::npos);
    }
    assert(login.credentials().appPassword.empty());
    assert(login.credentials().loginName.empty());
    assert(login.loginUrl().empty());
    assert(login.message().find("synthetic-app-password") == std::string::npos);
    assert(login.message().find("poll-secret") == std::string::npos);
}
