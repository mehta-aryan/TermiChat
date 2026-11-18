CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -pthread -MMD -MP
LDFLAGS  := -pthread

COMMON := build/src/socket_utils.o build/src/protocol.o
SERVER := $(COMMON) build/src/server.o build/src/server_main.o
CLIENT := $(COMMON) build/src/client.o build/src/client_main.o
TESTS  := $(COMMON) build/tests/test_framing.o

.PHONY: all test clean

all: termichat_server termichat_client

termichat_server: $(SERVER)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

termichat_client: $(CLIENT)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/test_framing: $(TESTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Unit test first, then the multi-client smoke test (3 consecutive runs).
test: all build/test_framing
	@echo "=== Running test_framing ==="
	build/test_framing
	@echo "=== Running smoke_test.sh (3 runs) ==="
	bash tests/smoke_test.sh 3

clean:
	rm -rf build termichat_server termichat_client

-include $(shell find build -name '*.d' 2>/dev/null)
