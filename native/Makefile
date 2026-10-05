# Native dustgate-brain. The decision core is the SAME headers the ESP32 builds (firmware/control);
# only this directory is the Linux/macOS shell. Needs Boost (brew install boost | apt install libboost-dev)
# and the ArduinoJson headers (found under the PlatformIO libdeps, or set ARDUINOJSON=<dir containing ArduinoJson.h>).
BOOST      ?= $(shell brew --prefix boost 2>/dev/null || echo /usr)
ARDUINOJSON?= $(shell dirname "$$(find ../.pio.nosync/libdeps -path '*/ArduinoJson/src/ArduinoJson.h' 2>/dev/null | head -1)")
CXX        ?= c++
CXXFLAGS   ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
INC        := -I$(BOOST)/include -I$(ARDUINOJSON) -I../firmware/control -I../firmware/outlets -I../firmware/sensing -Isrc
BIN        := build/dustgate-brain

FAKE := build/fakenode
FAKEPLUG := build/fakeplug
all: $(BIN) $(FAKE) $(FAKEPLUG)
$(FAKEPLUG): src/fakeplug.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/fakeplug.cpp -o $@ -L$(BOOST)/lib -pthread
$(FAKE): src/fakenode.cpp $(wildcard ../firmware/control/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/fakenode.cpp -o $@ -L$(BOOST)/lib -pthread
test: all
	bash test/e2e.sh
	bash test/e2e_plugs.sh
OUTLETS := ../firmware/outlets/ShellyGen2Outlet.cpp ../firmware/outlets/TasmotaOutlet.cpp
$(BIN): src/main.cpp src/PlugHttpNative.cpp src/PlugPoller.h src/NodeHub.h src/SessionBus.h $(OUTLETS) $(wildcard ../firmware/control/*.h) $(wildcard ../firmware/outlets/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DDUSTGATE_NATIVE $(INC) src/main.cpp src/PlugHttpNative.cpp $(OUTLETS) -o $@ -L$(BOOST)/lib -pthread
clean:
	rm -rf build
.PHONY: all clean test
