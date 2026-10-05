// native/src/PlugHttpNative.cpp — plughttp (outlets/PlugHttp.h) on Linux and macOS: a blocking Boost.Beast client
// with real connect and read deadlines, and the system resolver for mDNS names. Called from the plug poller's own
// thread, never from the network thread — a dead plug costs its timeout and must not stall a node link.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <netdb.h>
#include <arpa/inet.h>
#include <cstdio>
#include "PlugHttp.h"
#include "Log.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace plughttp {

// A plug answers on port 80. A test points this at a fake one (--plug-port).
static std::string g_port = "80";
void setPort(const std::string& p) { g_port = p; }

static Reply request(http::verb verb, const std::string& url, const std::string& body, const char* contentType,
                     uint32_t readMs, uint32_t connectMs) {
    Reply r;
    // http://host[:port]/target
    if (url.rfind("http://", 0) != 0) return r;
    const size_t hs = 7, slash = url.find('/', hs);
    std::string hostport = url.substr(hs, slash == std::string::npos ? std::string::npos : slash - hs);
    const std::string target = slash == std::string::npos ? "/" : url.substr(slash);
    std::string host = hostport, port = g_port;
    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
    try {
        net::io_context io;
        tcp::resolver rs(io);
        beast::tcp_stream s(io);
        s.expires_after(std::chrono::milliseconds(connectMs ? connectMs : 5000));
        s.connect(rs.resolve(host, port));
        s.expires_after(std::chrono::milliseconds(readMs ? readMs : 5000));
        http::request<http::string_body> req{verb, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "dustgate");
        if (verb == http::verb::post) { if (contentType && *contentType) req.set(http::field::content_type, contentType); req.body() = body; req.prepare_payload(); }
        http::write(s, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(s, buf, res);   // decodes chunked bodies, which Tasmota sends
        r.code = (int)res.result_int();
        // The contract: a GET's body only on a 200, a POST's on any answer.
        if (verb == http::verb::post || r.code == 200) r.body = res.body();
        beast::error_code ec; s.socket().shutdown(tcp::socket::shutdown_both, ec);
    } catch (const std::exception&) {
        r.code = -1;
    }
    return r;
}

Reply get(const std::string& url, uint32_t readMs, uint32_t connectMs) { return request(http::verb::get, url, "", nullptr, readMs, connectMs); }
Reply post(const std::string& url, const std::string& body, const char* ct, uint32_t ms) { return request(http::verb::post, url, body, ct, ms, ms); }

bool resolveHost(const char* host, std::string& ipOut, uint32_t) {
    if (!host || !*host) return false;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo((std::string(host) + ".local").c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    ipOut = ip;
    return true;
}

void log(const std::string& line) { dglog::line(line); }

}  // namespace plughttp
