// A process that called over TLS through the shared plain runtime must exit.
//
// On Windows the runtime's static I/O pool was destroyed at DLL detach, after
// Windows had already killed the pool's thread, sometimes in the middle of a
// handler. ~io_context then waited for that handler's work forever: the call
// had answered and printed, and the process never finished exiting.

#include "logos_protocol.h"
#include "self_signed_cert.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

char* copy(const char* text)
{
    auto* result = static_cast<char*>(std::malloc(std::strlen(text) + 1));
    std::strcpy(result, text);
    return result;
}

char* answer(const char*, const char*, void*) { return copy("42"); }
char* methods(void*) { return copy(R"([{"name":"answer","type":"method","returnType":"int"}])"); }
int acceptToken(const char*, const char*, void*) { return LP_OK; }

fs::path childPath()
{
#ifdef _WIN32
    // Installed beside this binary for the Windows runs.
    std::wstring self(MAX_PATH, L'\0');
    self.resize(::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    return fs::path(self).parent_path() / "plain_exit_child.exe";
#else
    return PLAIN_EXIT_CHILD;
#endif
}

// Runs `exe args...`. False when it has not exited within `timeout`; it is
// then killed.
bool exitsWithin(const fs::path& exe, const std::vector<std::string>& args,
                 std::chrono::seconds timeout, int* code)
{
#ifdef _WIN32
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (const auto& arg : args) {
        std::wstring quoted;
        for (wchar_t c : fs::path(arg).wstring()) {
            if (c == L'"') quoted += L'\\';
            quoted += c;
        }
        command += L" \"" + quoted + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return false;
    ::CloseHandle(info.hThread);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    const bool exited = ::WaitForSingleObject(info.hProcess, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
    if (exited) {
        DWORD status = 0;
        ::GetExitCodeProcess(info.hProcess, &status);
        *code = static_cast<int>(status);
    } else {
        ::TerminateProcess(info.hProcess, 1);
        ::WaitForSingleObject(info.hProcess, INFINITE);
    }
    ::CloseHandle(info.hProcess);
    return exited;
#else
    std::vector<std::string> store{exe.string()};
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : store) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawn(&pid, store[0].c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (::waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return true;
#endif
}

} // namespace

TEST(PlainExitTest, AProcessThatCalledOverTlsExits)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation(io, {boost::asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    const fs::path directory = fs::temp_directory_path()
        / ("logos-plain-exit-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    ASSERT_TRUE(writeSelfSignedCert(directory / "cert.pem", directory / "key.pem"));

    nlohmann::json client = {{"protocol", "tcp_ssl"}, {"host", "127.0.0.1"},
                             {"port", port}, {"verify_peer", false}};
    nlohmann::json server = client;
    server["cert_file"] = (directory / "cert.pem").string();
    server["key_file"] = (directory / "key.pem").string();
    lp_provider* provider =
        lp_provider_create("plain_exit", nlohmann::json::array({server}).dump().c_str());
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_save_token(provider, "exit_child", "secret"), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, answer, methods, acceptToken, nullptr), LP_OK);

    // Each run is a fresh process; the hang took about one exit in 40.
    constexpr int kRuns = 150;
    int stuck = 0;
    for (int run = 0; run < kRuns; ++run) {
        int code = -1;
        if (!exitsWithin(childPath(), {"plain_exit", "secret", client.dump()},
                         std::chrono::seconds(10), &code))
            ++stuck;
        else
            EXPECT_EQ(code, 0) << "run " << run;
    }
    EXPECT_EQ(stuck, 0) << stuck << " of " << kRuns
                        << " processes answered but never finished exiting";

    lp_provider_destroy(provider);
    fs::remove_all(directory);
}
