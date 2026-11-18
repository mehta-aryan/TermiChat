# Makefile for TermiChat – Phase 1
# Targets: all, test, clean
#
# -MMD -MP: generate .d dependency files so that editing any .hpp triggers
#           a rebuild of all .cpp files that include it.

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -pthread \
            -MMD -MP

SRCDIR  := src
TESTDIR := tests
BUILDDIR:= build

# ── Per-binary sources ────────────────────────────────────────────────────────
# socket_utils and protocol are shared; server.cpp / client.cpp are
# specific to their binary so we do not link server code into the client
# or client code into the server.

SHARED_SRCS := $(SRCDIR)/socket_utils.cpp \
               $(SRCDIR)/protocol.cpp

SERVER_SRCS := $(SHARED_SRCS) $(SRCDIR)/server.cpp $(SRCDIR)/server_main.cpp
CLIENT_SRCS := $(SHARED_SRCS) $(SRCDIR)/client.cpp $(SRCDIR)/client_main.cpp

TEST_FRAMING_SRCS := $(SHARED_SRCS) $(TESTDIR)/test_framing.cpp

# ── Object files (mirroring source tree under build/) ────────────────────────
to_obj = $(patsubst %.cpp,$(BUILDDIR)/%.o,$(1))

SERVER_OBJS       := $(call to_obj,$(SERVER_SRCS))
CLIENT_OBJS       := $(call to_obj,$(CLIENT_SRCS))
TEST_FRAMING_OBJS := $(call to_obj,$(TEST_FRAMING_SRCS))

ALL_OBJS := $(sort $(SERVER_OBJS) $(CLIENT_OBJS) $(TEST_FRAMING_OBJS))
-include $(ALL_OBJS:.o=.d)

# ── Binaries ──────────────────────────────────────────────────────────────────
SERVER_BIN       := termichat_server
CLIENT_BIN       := termichat_client
TEST_FRAMING_BIN := $(BUILDDIR)/test_framing

.PHONY: all test clean

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(TEST_FRAMING_BIN): $(TEST_FRAMING_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Pattern rule: compile any .cpp under src/ or tests/ into build/<subdir>/*.o
$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ── Test ──────────────────────────────────────────────────────────────────────
test: all $(TEST_FRAMING_BIN)
	@echo "=== Running test_framing ==="
	$(TEST_FRAMING_BIN)
	@echo ""
	@echo "=== Running smoke_test.sh (3 runs) ==="
	bash $(TESTDIR)/smoke_test.sh 3

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR) $(SERVER_BIN) $(CLIENT_BIN)
