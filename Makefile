CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -pthread -MMD -MP
LDFLAGS  := -pthread

COMMON := build/src/socket_utils.o build/src/protocol.o build/src/text_utils.o
# Terminal UI: raw mode and keys, the line editor, and the pure renderer.
UI     := build/src/terminal.o build/src/line_editor.o build/src/render.o
SERVER := $(COMMON) build/src/server.o build/src/server_main.o
CLIENT := $(COMMON) $(UI) build/src/client.o build/src/client_main.o
UNIT   := build/test_framing build/test_protocol build/test_text build/test_ui

.PHONY: all test clean
# Keep the intermediate .o files (the unit-test pattern rule below would
# otherwise make make delete the test objects after linking).
.SECONDARY:

all: termichat_server termichat_client

termichat_server: $(SERVER)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

termichat_client: $(CLIENT)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Each unit test links its own object plus the shared sources.
build/test_%: build/tests/test_%.o $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# The UI test also needs the UI modules.
build/test_ui: build/tests/test_ui.o $(COMMON) $(UI)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Unit tests first, then the multi-client smoke test (3 consecutive runs).
test: all $(UNIT)
	@for t in $(UNIT); do echo "=== Running $$t ==="; $$t || exit 1; done
	@echo "=== Running smoke_test.sh (3 runs) ==="
	bash tests/smoke_test.sh 3

clean:
	rm -rf build termichat_server termichat_client

-include $(shell find build -name '*.d' 2>/dev/null)
