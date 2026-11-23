// client_main.cpp – argument parsing and program entry point for the client.

#include "client.hpp"
#include "text_utils.hpp"

#include <unistd.h>   // isatty, read

#include <cerrno>
#include <cstdlib>    // strtol
#include <cstdio>     // fprintf
#include <iostream>
#include <string>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [host] [port] [username]\n"
        "  host      Server hostname or IP (default: localhost)\n"
        "  port      TCP port (1-65535, default: 12345)\n"
        "  username  1-16 characters from A-Z a-z 0-9 _ -\n"
        "            (asked for interactively if omitted and stdin is a terminal)\n"
        "Options:\n"
        "  -h, --help   Show this message and exit\n",
        prog);
}

// Reads one line from stdin with plain read(), a byte at a time.  Unlike
// std::cin it never buffers ahead, so nothing typed later is swallowed before
// the client's own poll()+read() loop takes over.  Returns false on EOF.
static bool read_stdin_line(std::string& line) {
    line.clear();
    char c;
    while (true) {
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return !line.empty();      // EOF: hand back a last partial line
        if (c == '\n') break;
        line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// Asks for a username, up to three tries.  Only used when stdin is a terminal.
static bool prompt_username(std::string& name) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        std::fputs("Username: ", stdout);
        std::fflush(stdout);
        if (!read_stdin_line(name)) return false;
        const NameCheck check = check_username(name);
        if (check == NameCheck::Ok) return true;
        std::fprintf(stderr, "%s\n", name_problem(check));
    }
    return false;
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t    port = 12345;
    std::string username;

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

    if (argc > 4) {
        usage(argv[0]);
        return 2;
    }

    if (argc > 3) {
        username = argv[3];
        const NameCheck check = check_username(username);
        if (check != NameCheck::Ok) {
            std::fprintf(stderr, "Error: invalid username: %s\n", name_problem(check));
            usage(argv[0]);
            return 2;
        }
    } else if (::isatty(STDIN_FILENO)) {
        if (!prompt_username(username)) {
            std::fprintf(stderr, "Error: no valid username given\n");
            return 2;
        }
    } else {
        std::fprintf(stderr, "Error: username required (stdin is not a terminal)\n");
        usage(argv[0]);
        return 2;
    }

    try {
        Client client(host, port, username);
        return client.run();
    } catch (const NetError& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
