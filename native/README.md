# native/ — dustgate-brain for Linux and macOS

The brain core (`firmware/control/*.h`) is plain C++ and runs here unchanged; this directory is the
platform shell (Boost.Beast for HTTP/WebSocket, a UDP beacon). **Step 1 of the native build
(2026-10-04): nodes link and are listed; no layout, routing or UI yet.**

```bash
brew install boost            # or: apt install libboost-dev
make -C native
native/build/dustgate-brain --pair dustgate-planer,dustgate-tablesaw --broadcast 192.168.86.255
curl localhost:8080/api/nodes
```

A node dials the brain it is paired to by its id (`--id`, default `dustgate`), so run this only while
the ESP32 brain with the same id is paused (`POST /api/nodes/pause`), or give it another id.
Verified 2026-10-04 on the bench: all five nodes found it by beacon and linked.
