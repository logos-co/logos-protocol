// Makes one tls_tcp call and returns from main, for PlainExitTest.
// argv: <directory holding chain.pem, key.pem, anchors.pem, pin> <port>
#include "logos_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct Session {
    std::string anchors;
    std::string pin;
    std::string port;
};

std::string read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

char* copy(const std::string& text)
{
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

char* dial(const char*, void* userData)
{
    const auto& s = *static_cast<Session*>(userData);
    std::string anchors;
    for (char c : s.anchors) anchors += c == '\n' ? std::string("\\n") : std::string(1, c);
    return copy(R"({"addresses":["127.0.0.1"],"port":)" + s.port + R"(,"server_pin":")" + s.pin
                + R"(","anchors":")" + anchors + R"("})");
}

char* hello(const char*, void*) { return copy(R"({"ticket":"exit","module":"plain_exit"})"); }

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string dir = argv[1];
    Session session{read(dir + "/anchors.pem"), read(dir + "/pin"), argv[2]};
    lp_client* client = lp_client_create("plain_exit", "exit_child", R"({"protocol":"tls_tcp"})",
                                         nullptr);
    if (!client) return 4;
    if (lp_client_set_tls_credential(client, read(dir + "/chain.pem").c_str(),
                                     read(dir + "/key.pem").c_str()) != LP_OK
        || lp_client_set_session_hook(client, &dial, &hello, &session) != LP_OK)
        return 3;
    char* value = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, "answer", "[]", 5000, &value, &error);
    std::printf("%s\n", value ? value : (error ? error : "null"));
    lp_string_free(value);
    lp_string_free(error);
    lp_client_destroy(client);
    return status == LP_OK ? 0 : 5;
}
