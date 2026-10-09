# Native dustgate-brain. The decision core is the SAME headers the ESP32 builds (firmware/control);
# only this directory is the Linux/macOS shell. Needs Boost (brew install boost | apt install libboost-dev)
# and the ArduinoJson headers (found under the PlatformIO libdeps, or set ARDUINOJSON=<dir containing ArduinoJson.h>).
BOOST      ?= $(shell brew --prefix boost 2>/dev/null || echo /usr)
# ArduinoJson: an explicit ARDUINOJSON=<dir>, else a vendored copy (pi/setup.sh downloads one), else PlatformIO's.
ARDUINOJSON?= $(shell for d in vendor /opt/dustgate/vendor; do [ -f $$d/ArduinoJson.h ] && echo $$d && exit; done; dirname "$$(find ../.pio.nosync/libdeps -path '*/ArduinoJson/src/ArduinoJson.h' 2>/dev/null | head -1)")
# Which commit this is, for /api/info and the log: COMMIT=<sha> (the deploy script writes ../COMMIT when it ships source without .git).
COMMIT     ?= $(shell cat ../COMMIT 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
CXX        ?= c++
CXXFLAGS   ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
INC        := -I$(BOOST)/include -I$(ARDUINOJSON) -I../firmware/control -I../firmware/outlets -I../firmware/sensing -I../firmware/api -Isrc
BIN        := build/dustgate-brain

FAKE := build/fakenode
FAKEPLUG := build/fakeplug
all: $(BIN) $(FAKE) $(FAKEPLUG)
# Just the brain: what a Pi builds (the fakes are for the tests).
brain: $(BIN)
$(FAKEPLUG): src/fakeplug.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/fakeplug.cpp -o $@ -L$(BOOST)/lib -pthread
$(FAKE): src/fakenode.cpp $(wildcard ../firmware/control/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/fakenode.cpp -o $@ -L$(BOOST)/lib -pthread
test: all
	bash test/e2e.sh
	bash test/e2e_plugs.sh
	bash test/e2e_api.sh
	bash test/e2e_outlets.sh
	bash test/e2e_push.sh
	bash test/e2e_state.sh
OUTLETS := ../firmware/outlets/ShellyGen2Outlet.cpp ../firmware/outlets/TasmotaOutlet.cpp
# Everything the brain includes, by directory: a hand-kept list missed utils/JsonAlloc.h and sensing/PowerSensor.h, so a change
# to either did not rebuild it. COMMIT_STAMP makes a new commit rebuild too, or /api/info would report the old one.
SRCS := src/main.cpp src/PlugHttpNative.cpp $(OUTLETS)
DEPS := $(SRCS) $(wildcard src/*.h) $(wildcard ../firmware/control/*.h) $(wildcard ../firmware/outlets/*.h) \
        $(wildcard ../firmware/sensing/*.h) $(wildcard ../firmware/utils/*.h) $(wildcard ../firmware/api/*.h)
COMMIT_STAMP := build/commit/$(COMMIT)
$(COMMIT_STAMP):
	@rm -rf build/commit && mkdir -p build/commit && touch $@
$(BIN): $(DEPS) $(COMMIT_STAMP)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DDUSTGATE_NATIVE -DDG_COMMIT=\"$(COMMIT)\" $(INC) $(SRCS) -o $@ -L$(BOOST)/lib -pthread
clean:
	rm -rf build
# The Raspberry Pi binary, cross-built on a Mac with zig (brew install zig): a Pi Zero's 512 MB cannot compile main.cpp in any
# reasonable time, and a customer's Pi should not carry a compiler. Dynamically linked against glibc 2.36 (Raspberry Pi OS Bookworm
# and later); stripped, since the debug info alone is ~85 MB.
ARM64 := build/arm64/dustgate-brain
arm64: $(ARM64)
$(ARM64): $(DEPS) $(COMMIT_STAMP)
	@mkdir -p build/arm64
	zig c++ -target aarch64-linux-gnu.2.36 -std=c++17 -O2 -s -DDUSTGATE_NATIVE -DDG_COMMIT=\"$(COMMIT)\" $(INC) $(SRCS) -o $@ -pthread
.PHONY: all brain clean test arm64
