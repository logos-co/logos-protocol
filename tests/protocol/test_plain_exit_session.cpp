// exit() with a module image's tls_tcp session open: the image's static closes it
// through the host's runtime after exit has destroyed everything made later.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace {

// The child's exit status, or 128 + the signal that ended it; -1 if it hung.
int runChild(const std::vector<std::string>& args)
{
    std::vector<std::string> store{PLAIN_EXIT_SESSION_CHILD, PLAIN_EXIT_SESSION_IMAGE};
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : store) argv.push_back(arg.data());
    argv.push_back(nullptr);
    // Freed memory is overwritten, so a use after free faults every time.
    std::vector<std::string> env{"MallocScribble=1", "MALLOC_PERTURB_=85"};
    for (char** e = environ; *e; ++e) env.emplace_back(*e);
    std::vector<char*> envp;
    for (auto& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    if (::posix_spawn(&pid, store[0].c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0)
        return -2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    while (::waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void expectExitsWithOne(const std::vector<std::string>& args)
{
    constexpr int kRuns = 10;
    for (int run = 0; run < kRuns; ++run)
        ASSERT_EQ(runChild(args), 1) << "run " << run << ": 128+n is signal n, -1 a hang";
}

} // namespace

TEST(PlainExitSessionTest, AnImageMayCloseItsSessionAtExit)
{
    expectExitsWithOne({});
}

TEST(PlainExitSessionTest, AnImageMayCloseItsSessionAtExitAfterOpenSslCleanedUp)
{
    expectExitsWithOne({"late-openssl"});
}
