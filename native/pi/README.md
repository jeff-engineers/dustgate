# Running the brain on a Raspberry Pi

For now the goal is simply **run the shop without the Mac**: a Pi on the shop's WiFi doing exactly what the Mac brain does.
Packaging for customers (first-boot WiFi, read-only root) comes later.

## One-time: flash the Pi

In Raspberry Pi Imager choose **Raspberry Pi OS Lite (64-bit)** and, under the OS customisation (the gear / "Edit settings"):

| Setting | Value | Why |
|---|---|---|
| Hostname | `dustgate` | the app is then at `http://dustgate.local`. (Nothing REQUIRES mDNS: it is only how you find the Pi; use its IP if your network blocks it.) **Two boards answering to `dustgate.local` collide**, so while an ESP32 brain with that name is still powered, name the Pi something else (`dustgate-pi`). The brain's pairing id (`--id`) is separate from the hostname. |
| Username / password | yours | the deploy script SSHes in as this user and uses `sudo` |
| SSH | enable, **allow public-key authentication** and paste your Mac's public key (`cat ~/.ssh/id_ed25519.pub`) | `deploy.sh` runs non-interactively |
| WiFi | the shop's network, correct country | |
| Locale / time zone | yours | logs carry times |

**The Pi does NOT need a fixed address, and you should not give it one.** The brain follows its own address: it checks every 5 seconds,
and when the router gives it a new one it beacons the new address (nodes find a moved brain by the beacon and redial) and points every
plug it owns at the new address (a plug keeps pushing to the old one until told; a plug whose push goes quiet is polled meanwhile). It
logs `[NET] address changed <old> -> <new>`. Plugs have to be re-told, so expect the shop's plugs to read by polling for a few seconds
after a change. The ESP32 brain has always done the same. What you do need is a way to FIND the Pi: Raspberry Pi OS advertises its
hostname over mDNS, so `http://dustgate.local` works on a network that allows multicast. mDNS is a fast path, not a requirement: if your
network blocks it, read the Pi's address from the router's client list (nodes never need it, they use the beacon).

Boot it, wait a minute, and from your Mac: `ssh <user>@dustgate.local`.

## One-time: set it up

From your Mac, copy this directory over and run the setup (installs the compiler and libraries, creates the `dustgate` service user,
turns WiFi power-saving off, enables the hardware watchdog and a persistent journal, and installs the systemd unit):

```bash
scp -r native/pi <user>@dustgate.local:/tmp/dustgate-pi
ssh -t <user>@dustgate.local 'sudo bash /tmp/dustgate-pi/setup.sh'
```

## Deploy (every time)

```bash
native/pi/deploy.sh <user>@dustgate.local                       # build the app here, ship the source, build there, restart
native/pi/deploy.sh <user>@dustgate.local --state /tmp/dgbench  # the FIRST time: also bring over the layout, pairings and API key
```

`--state` copies a state directory (`topology.json`, `nodes.json`, `apikey`, staged node firmware, `plugs.json`) so the Pi comes up as
the same brain. **Stop the Mac brain first** (`pkill -f dustgate-brain`): two brains with the id `dustgate` fight over the nodes.

`deploy.sh` cross-builds the brain on the Mac with zig (`brew install zig`; `make -C native arm64`, about a minute) and installs only the 4 MB binary. A Pi Zero 2 W cannot compile `main.cpp`: the compiler is killed at 415 MB, and a swapfile on the SD card thrashes until the watchdog reboots the Pi. `--on-pi` builds there anyway (`update.sh` adds a swapfile for it); use a Pi 3 or later for that. `sudo` asks for the Pi user's password once per deploy (cached ~15 minutes).
The update **rolls back** to the previous binary if the new one does not answer on port 80 within 30 s.

## Day to day

```bash
ssh <user>@dustgate.local
sudo systemctl status dustgate-brain
journalctl -u dustgate-brain -f          # the brain's log (also in the app: Boards -> Log)
sudo systemctl restart dustgate-brain
sudo /opt/dustgate/update.sh --git       # later: pull from git on the Pi instead of rsync from the Mac
```

## What this has NOT been through

Run on a Pi Zero 2 W (Raspberry Pi OS Trixie) on 2026-10-06: the cross-built brain started, took the shop's layout and state, re-pointed three plugs
at itself, and all six nodes dialled in by themselves. Not yet run: an address change under load, a power pull, a full shop day.
Not built: a git deploy key on the Pi (the `--git` path), mDNS advertising of the brain, a read-only root, first-boot WiFi setup.
