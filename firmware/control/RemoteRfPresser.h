// =============================================================================
// control/RemoteRfPresser.h — press the collector's remote through a NODE's transmitter.
//
// The second CollectorPresser (control/CollectorPress.h). RfCollectorPresser keys this
// board's own pad; this one asks a paired board to key ITS pad, with a PRESS frame. It is
// what lets the board at the collector be an ordinary node instead of the brain.
//
// press() answers the question the interface asks — "could the edge be SENT" — and no
// more: true means the node is linked, said it has a transmitter, and the frame was
// handed over. Whether the blower changed state is still only the plug's to say, and
// when and whether to press again is still the policy in CollectorPress.h, on THIS side
// of the wire, because only this side can read the plug.
//
// PURE — no Arduino.h, so the host tests drive it.
// =============================================================================
#pragma once
#include <string>
#include "CollectorPress.h"
#include "NodeBus.h"
#include "RfDefaults.h"

namespace topo {

class RemoteRfPresser : public CollectorPresser {
public:
    RemoteRfPresser(NodeBus* bus, const std::string& controllerId,
                    uint8_t address = rf::kRocklerAddress, uint8_t data = rf::kRocklerData,
                    uint32_t tickUs = rf::kDefaultTickUs, uint32_t repeats = rf::kDefaultRepeats)
        : _bus(bus), _cid(controllerId), _address(address), _data(data),
          _tickUs(tickUs), _repeats(repeats) {}

    bool press() override {
        return _bus && _bus->pressRf(_cid.c_str(), _address, _data, _tickUs, _repeats);
    }
    const char* kind() const override { return "rf"; }
    const std::string& controllerId() const { return _cid; }

private:
    NodeBus*    _bus;
    std::string _cid;
    uint8_t     _address;
    uint8_t     _data;
    uint32_t    _tickUs;
    uint32_t    _repeats;
};

}  // namespace topo
