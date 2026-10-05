// native/src/fakenode.cpp — a node that is only a socket: dials a brain, answers HELLO with a WELCOME, ACKs
// and "arrives" every SET, and says what it received. For testing the brain without a bench:
//   fakenode <host> <port> <nodeId> [seconds]
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdio>
#include <ArduinoJson.h>
#include "NodeLink.h"

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 4) { std::printf("usage: fakenode <host> <port> <nodeId> [seconds]\n"); return 2; }
    const std::string host = argv[1], port = argv[2], id = argv[3];
    const int seconds = argc > 4 ? std::atoi(argv[4]) : 30;
    net::io_context io; tcp::resolver rs(io); ws::stream<tcp::socket> s(io);
    net::connect(s.next_layer(), rs.resolve(host, port));
    s.handshake(host + ":" + port, "/nodelink");
    auto send = [&](const JsonDocument& d) { std::string o; serializeJson(d, o); s.text(true); s.write(net::buffer(o)); };
    { StaticJsonDocument<96> d; topo::nodelink::buildJoin(d.to<JsonObject>(), id.c_str()); send(d); }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    // The socket is blocking; a read with no traffic would hold us past `seconds`, so poll the clock between frames.
    s.next_layer().non_blocking(false);
    s.read_message_max(1 << 16);
    while (std::chrono::steady_clock::now() < end) {
        beast::flat_buffer b; beast::error_code ec; s.read(b, ec);
        if (ec) { std::printf("closed: %s\n", ec.message().c_str()); return 0; }
        DynamicJsonDocument f(1024); deserializeJson(f, beast::buffers_to_string(b.data()));
        const std::string t = f["t"] | "";
        std::printf("<- %s\n", beast::buffers_to_string(b.data()).c_str());
        if (t == "HELLO") {
            StaticJsonDocument<384> d;
            topo::nodelink::buildWelcome(d.to<JsonObject>(), id.c_str(), "fake", "fake 0", 2, 0, nullptr, true, 0, false, true);
            send(d);
        } else if (t == "SET") {
            { StaticJsonDocument<96> d; topo::nodelink::buildAck(d.to<JsonObject>(), f["seq"] | 0, true); send(d); }
            { StaticJsonDocument<128> d; topo::nodelink::buildState(d.to<JsonObject>(), f["selectorId"] | "", f["stateId"] | "", false); send(d); }
        }
    }
    return 0;
}
