// client_main.cpp – argument parsing and program entry point for the client.

#include "client.hpp"

#include <cstdlib>    // strtol
#include <cstdio>     // fprintf
#include <iostream>
#include <string>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [host] [port]\n"
        "  host  Server hostname or IP (default: localhost)\n"
        "  port  TCP port (1-65535, default: 12345)\n"
        "Options:\n"
        "  -h, --help   Show this message and exit\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t    port = 12345;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            usage(argv[0]);
            return 0;
        }
        host = arg1;
    }

    if (argc > 2) {
        char* end = nullptr;
        long p = std::strtol(argv[2], &end, 10);
        if (*end != '\0' || p < 1 || p > 65535) {
            std::fprintf(stderr, "Error: invalid port '%s'\n", argv[2]);
            usage(argv[0]);
            return 2;
        }
        port = static_cast<uint16_t>(p);
    }

    if (argc > 3) {
        usage(argv[0]);
        return 2;
    }

    try {
        Client client(host, port);
        return client.run();
    } catch (const NetError& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
