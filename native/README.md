# native/ — dustgate-brain for Linux and macOS

The brain core (`firmware/control/*.h`) is plain C++ and runs here unchanged; this directory is the
platform shell (Boost.Beast for HTTP/WebSocket, a UDP beacon). **Status (2026-10-05): nodes link; a layout is adopted
and routes gates on nodes (fake nodes in tests; real ones only linked and fed a CONFIG, never moved); the collector is pressed through a node's transmitter; tool and collector plugs are
polled with the ESP32's own Shelly/Tasmota drivers; the app is served (`--www`). The shared API (`firmware/api/ApiCore.h`),
node firmware staging, the plug screens and board/plug problems are in. Plugs are claimed like the ESP32 does (a Shelly is pointed at this brain's `/shelly-rpc` and read off its socket, a plug someone else owns is polled until a person approves a takeover). Not yet: mDNS advertising,
packaging; see `TODO/TODO.md`.** Verified end to end against fake
nodes and a fake plug (`make -C native test`) and, for linking, against the five bench nodes.

```bash
brew install boost            # or: apt install libboost-dev
make -C native
sudo native/build/dustgate-brain --pair dustgate-planer,dustgate-tablesaw   # port 80 (nodes pull firmware from it); --port 8080 needs no sudo
curl localhost:8080/api/nodes
```

A node dials the brain it is paired to by its id (`--id`, default `dustgate`), so run this only while
the ESP32 brain with the same id is paused (`POST /api/nodes/pause`), or give it another id.
Verified 2026-10-04 on the bench: all five nodes found it by beacon and linked.
