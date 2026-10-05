# native/ — dustgate-brain for Linux and macOS

The brain core (`firmware/control/*.h`) is plain C++ and runs here unchanged; this directory is the
platform shell (Boost.Beast for HTTP/WebSocket, a UDP beacon). **Status (2026-10-05): nodes link; a layout is adopted
and routes gates on nodes (fake nodes in tests; real ones only linked and fed a CONFIG, never moved); the collector is pressed through a node's transmitter; tool and collector plugs are
polled with the ESP32's own Shelly/Tasmota drivers; the app is served (`--www`). Not yet: the rest of the API (outlet
discovery/pairing, calibration, OTA staging, settings), mDNS advertising, packaging.** Verified end to end against fake
nodes and a fake plug (`make -C native test`) and, for linking, against the five bench nodes.

```bash
brew install boost            # or: apt install libboost-dev
make -C native
native/build/dustgate-brain --pair dustgate-planer,dustgate-tablesaw --broadcast 192.168.86.255
curl localhost:8080/api/nodes
```

A node dials the brain it is paired to by its id (`--id`, default `dustgate`), so run this only while
the ESP32 brain with the same id is paused (`POST /api/nodes/pause`), or give it another id.
Verified 2026-10-04 on the bench: all five nodes found it by beacon and linked.
