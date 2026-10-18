# native/ — dustgate-brain for Linux and macOS

The brain core (`firmware/control/*.h`) is plain C++ and runs here unchanged; this directory is the
platform shell (Boost.Beast for HTTP/WebSocket, a UDP beacon). It links nodes, routes gates on them, drives the
collector (through a node's transmitter or a smart plug), claims and reads Shelly and Tasmota plugs, stages node
firmware for over-the-air updates, and serves the app (`--www`). **It runs the shop on a Raspberry Pi** (since
2026-10-11; [`pi/README.md`](pi/README.md)), and on a Mac for the bench. `make -C native test` checks it end to end
against fake nodes and a fake plug. Not built: mDNS advertising of its own (avahi does it on a Pi) and packaging for
anyone else; see `TODO/TODO.md`.

```bash
brew install boost            # or: apt install libboost-dev
make -C native
sudo native/build/dustgate-brain --pair dustgate-planer,dustgate-tablesaw   # port 80 (nodes pull firmware from it); --port 8080 needs no sudo
curl localhost:8080/api/nodes
```

A node dials the brain it is paired to by its id (`--id`, default `dustgate`), so two brains with the same id fight
over the nodes: stop the Pi's (`sudo systemctl stop dustgate-brain`) before running one on the Mac, or give the Mac's
another id.
