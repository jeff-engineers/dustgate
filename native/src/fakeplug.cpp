// native/src/fakeplug.cpp — a Shelly Gen2 plug that is only an HTTP server: `fakeplug <port> <powerfile>` answers
// /rpc/Switch.GetStatus with the number in <powerfile> as apower, so a test can run a tool by writing a file.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cstdio>
#include <fstream>
#include <string>
namespace beast = boost::beast; namespace http = beast::http; namespace net = boost::asio; using tcp = net::ip::tcp;

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: fakeplug <port> <powerfile>\n"); return 2; }
    const std::string file = argv[2];
    net::io_context io; tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), (unsigned short)std::atoi(argv[1])));
    for (;;) {
        tcp::socket s(io); acc.accept(s);
        try {
            beast::flat_buffer b; http::request<http::string_body> req; http::read(s, b, req);
            std::string body; const std::string t = std::string(req.target());
            if (t.rfind("/rpc/Switch.GetStatus", 0) == 0) {
                double w = 0; std::ifstream f(file); f >> w;
                body = "{\"id\":0,\"source\":\"test\",\"output\":true,\"apower\":" + std::to_string(w) + ",\"voltage\":120.0}";
            } else if (t.rfind("/rpc/Switch.Set", 0) == 0) {
                std::ofstream(file + ".sw") << t; body = "{\"was_on\":true}";
            } else body = "{}";
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json"); res.body() = body; res.prepare_payload(); res.keep_alive(false);
            http::write(s, res);
        } catch (...) {}
    }
}
