// native/src/NodeHub.h — the native shell's node registry: one pure NodeSession per paired node,
// plus the rules the ESP32 puts in nodeEventHook (is this node paired, is it already linked).
// Single-threaded by design: everything runs on the io_context's one thread, so a session needs
// no mutex (the session's header says the shell serialises access; here that is the thread).
#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "NodeSession.h"
#include "NodeBus.h"
#include "SessionBus.h"

namespace dgbrain {

inline uint32_t nowMs() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct StdoutSink : topo::SessionSink {
    void say(const char* l) override { std::printf("%s\n", l); std::fflush(stdout); }
};

struct Node {
    std::string id;
    topo::NodeSession session;
    unsigned linkId = 0;       // which connection owns the link; 0 = none
    SessionBus bus{session};
    Node(const std::string& nid, const std::string& primaryId, topo::SessionSink* sink)
        : id(nid), session(nowMs, sink) { session.configure(nid.c_str(), primaryId.c_str()); }
};

class NodeHub {
public:
    NodeHub(std::string primaryId, std::vector<std::string> paired) : _primaryId(std::move(primaryId)) {
        for (auto& id : paired) {
            auto n = std::make_unique<Node>(id, _primaryId, &_sink);
            _bus.registerRemote(id, &n->bus);
            _nodes.emplace(id, std::move(n));
        }
    }
    const std::string& primaryId() const { return _primaryId; }
    Node* find(const std::string& id) { auto it = _nodes.find(id); return it == _nodes.end() ? nullptr : it->second.get(); }
    std::map<std::string, std::unique_ptr<Node>>& nodes() { return _nodes; }
    topo::NodeBus& bus() { return _bus; }
    bool anyDown() { for (auto& kv : _nodes) if (!kv.second->session.online()) return true; return false; }
private:
    std::string _primaryId;
    StdoutSink _sink;
    topo::NodeBus _bus;
    std::map<std::string, std::unique_ptr<Node>> _nodes;
};

}  // namespace dgbrain
