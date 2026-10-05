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
    std::string name;
    bool removed = false;      // unpaired: its socket closes itself on the next tick
    SessionBus bus{session};
    Node(const std::string& nid, const std::string& primaryId, topo::SessionSink* sink)
        : id(nid), session(nowMs, sink) { session.configure(nid.c_str(), primaryId.c_str()); }
};

class NodeHub {
public:
    NodeHub(std::string primaryId) : _primaryId(std::move(primaryId)) {}
    const std::string& primaryId() const { return _primaryId; }
    // Pair (or rename) a node by id/host. Pairing is its own fact, independent of any layout.
    std::shared_ptr<Node> add(const std::string& id, const std::string& name) {
        auto it = _nodes.find(id);
        if (it != _nodes.end()) { if (!name.empty()) it->second->name = name; return it->second; }
        auto n = std::make_shared<Node>(id, _primaryId, &_sink);
        n->name = name.empty() ? id : name;
        _nodes[id] = n; rebind();
        return n;
    }
    bool remove(const std::string& id) {
        auto it = _nodes.find(id);
        if (it == _nodes.end()) return false;
        it->second->removed = true;
        _nodes.erase(it); rebind();
        return true;
    }
    std::shared_ptr<Node> find(const std::string& id) { auto it = _nodes.find(id); return it == _nodes.end() ? nullptr : it->second; }
    std::map<std::string, std::shared_ptr<Node>>& nodes() { return _nodes; }
    topo::NodeBus& bus() { return _bus; }
    bool anyDown() { for (auto& kv : _nodes) if (!kv.second->session.online()) return true; return false; }
    bool paused = false;    // POST /api/nodes/pause: every link closes and new JOINs are refused
private:
    void rebind() { _bus.clearRemotes(); for (auto& kv : _nodes) _bus.registerRemote(kv.first, &kv.second->bus); }
    std::string _primaryId;
    StdoutSink _sink;
    topo::NodeBus _bus;
    std::map<std::string, std::shared_ptr<Node>> _nodes;
};

}  // namespace dgbrain
