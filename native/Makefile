# Native dustgate-brain. The decision core is the SAME headers the ESP32 builds (firmware/control);
# only this directory is the Linux/macOS shell. Needs Boost (brew install boost | apt install libboost-dev)
# and the ArduinoJson headers (found under the PlatformIO libdeps, or set ARDUINOJSON=<dir containing ArduinoJson.h>).
BOOST      ?= $(shell brew --prefix boost 2>/dev/null || echo /usr)
ARDUINOJSON?= $(shell dirname "$$(find ../.pio.nosync/libdeps -path '*/ArduinoJson/src/ArduinoJson.h' 2>/dev/null | head -1)")
CXX        ?= c++
CXXFLAGS   ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
INC        := -I$(BOOST)/include -I$(ARDUINOJSON) -I../firmware/control -Isrc
BIN        := build/dustgate-brain

FAKE := build/fakenode
all: $(BIN) $(FAKE)
$(FAKE): src/fakenode.cpp ../firmware/control/NodeLink.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/fakenode.cpp -o $@ -L$(BOOST)/lib -pthread
test: all
	bash test/e2e.sh
$(BIN): src/main.cpp src/NodeHub.h src/SessionBus.h ../firmware/control/NodeSession.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) src/main.cpp -o $@ -L$(BOOST)/lib -pthread
clean:
	rm -rf build
.PHONY: all clean test
