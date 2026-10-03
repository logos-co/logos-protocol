// A process that called over a tls_tcp session through the shared plain runtime
// must exit.
//
// On Windows the runtime's static I/O pool was destroyed at DLL detach, after
// Windows had already killed the pool's thread, sometimes in the middle of a
// handler. ~io_context then waited for that handler's work forever: the call
// had answered and printed, and the process never finished exiting.

#include "logos_protocol.h"
#include "session_certs.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
char* admit(const char*, void*)
{
    return copy(R"({"caller":{"kind":"remote","peer":"peer-1","name":"exit_child"},)"
                R"("lifetime_ms":60000,"session":{"peer":"peer-1","route":"r1","generation":1}})");
}

void write(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

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

TEST(PlainExitTest, AProcessThatCalledOverASessionExits)
{
    session_test::Identity server{"serverAuth"};
    session_test::Identity client{"clientAuth"};
    lp_provider* provider = lp_provider_create(
        "plain_exit", R"([{"protocol":"tls_tcp","host":"127.0.0.1","port":0}])");
    ASSERT_NE(provider, nullptr);
    ASSERT_EQ(lp_provider_set_tls_credential(provider, server.chainPem().c_str(),
                                             server.keyPem().c_str()), LP_OK);
    ASSERT_EQ(lp_provider_set_trust_anchors(provider, client.rootPem().c_str()), LP_OK);
    ASSERT_EQ(lp_provider_set_session_authenticator(provider, &admit, nullptr), LP_OK);
    ASSERT_EQ(lp_provider_register(provider, answer, methods, nullptr, nullptr), LP_OK);
    char* endpoints = lp_provider_endpoints_json(provider);
    const auto listeners = nlohmann::json::parse(endpoints ? endpoints : "[]");
    lp_string_free(endpoints);
    ASSERT_EQ(listeners.size(), 1u);
    const std::string port = std::to_string(listeners[0].value("port", 0));

    // What the child dials with: its credential, and the server's root and pin.
    const fs::path directory = fs::temp_directory_path()
        / ("logos-plain-exit-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    write(directory / "chain.pem", client.chainPem());
    write(directory / "key.pem", client.keyPem());
    write(directory / "anchors.pem", server.rootPem());
    write(directory / "pin", server.leafPin());

    // Each run is a fresh process; the hang took about one exit in 40.
    constexpr int kRuns = 150;
    int stuck = 0;
    for (int run = 0; run < kRuns; ++run) {
        int code = -1;
        if (!exitsWithin(childPath(), {directory.string(), port}, std::chrono::seconds(10), &code))
            ++stuck;
        else
            EXPECT_EQ(code, 0) << "run " << run;
    }
    EXPECT_EQ(stuck, 0) << stuck << " of " << kRuns
                        << " processes answered but never finished exiting";

    lp_provider_destroy(provider);
    fs::remove_all(directory);
}
