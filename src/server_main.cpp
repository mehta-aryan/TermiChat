// server_main.cpp – argument parsing and program entry point for the server.

#include "server.hpp"

#include <cstdlib>    // strtol
#include <cstdio>     // fprintf, stderr
#include <iostream>
#include <string>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [port]\n"
        "  port  TCP port to listen on (1-65535, default 12345)\n"
        "Options:\n"
        "  -h, --help   Show this message and exit\n",
        prog);
}

int main(int argc, char* argv[]) {
    uint16_t port = 12345;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (argc > 2) {
            usage(argv[0]);
            return 2;
        }
        // Validate port with strtol for a strict, explicit range check.
        char* end = nullptr;
        long p = std::strtol(arg1.c_str(), &end, 10);
        if (*end != '\0' || p < 1 || p > 65535) {
            std::fprintf(stderr, "Error: invalid port '%s'\n", arg1.c_str());
            usage(argv[0]);
            return 2;
        }
        port = static_cast<uint16_t>(p);
    }

    try {
        Server server(port);
        server.run();
    } catch (const NetError& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
