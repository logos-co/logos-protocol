// Makes one plain call and returns from main, for PlainExitTest.
// argv: <target> <token> <transport json>
#include "logos_protocol.h"

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    if (lp_token_save(argv[1], argv[2]) != LP_OK) return 3;
    lp_client* client = lp_client_create(argv[1], "exit_child", argv[3], argv[3]);
    if (!client) return 4;
    char* value = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, "answer", "[]", 5000, &value, &error);
    std::printf("%s\n", value ? value : (error ? error : "null"));
    lp_string_free(value);
    lp_string_free(error);
    lp_client_destroy(client);
    return status == LP_OK ? 0 : 5;
}
