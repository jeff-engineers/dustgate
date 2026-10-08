// native/src/SessionBus.h — an ActuatorBus over one NodeSession. The ESP32 has the same thing as
// RemoteActuatorBus, wrapped in a mutex because its transport runs on another task; here every call
// is on the io_context's one thread, so it is a straight delegation.
#pragma once
// Spelled out, not left to whatever Boost happens to pull in: GCC (the Pi) is stricter than the Mac's clang.
#include <cstdint>
#include "NodeSession.h"

namespace dgbrain {

class SessionBus : public topo::ActuatorBus {
public:
    explicit SessionBus(topo::NodeSession& s) : _s(s) {}
    bool online() const override { return _s.online(); }
    bool busy() const override { return _s.busy(); }
    bool setState(const char* id, JsonObjectConst sel, const char* st) override { return _s.setState(id, sel, st); }
    bool jog(int ch, int angle, bool detach) override { return _s.jog(ch, angle, detach); }
    void setServoPulseRange(int minUs, int maxUs) override { _s.setServoPulseRange(minUs, maxUs); }
    void configureSensors(JsonArrayConst a) override { _s.configureSensors(a); }
    bool senseOf(const char* id, bool& on, uint32_t& at) const override { return _s.senseOf(id, on, at); }
    bool pollsPlugs() const override { return _s.pollsPlugs(); }
    bool plugReading(const char* id, float& w, bool& f, uint32_t& at) const override { return _s.plugReading(id, w, f, at); }
    bool canPressRf() const override { return _s.canPressRf(); }
    bool watchesBin() const override { return _s.watchesBin(); }
    bool pressRf(uint8_t a, uint8_t d, uint32_t t, uint32_t r) override { return _s.pressRf(a, d, t, r); }
private:
    topo::NodeSession& _s;
};

}  // namespace dgbrain
