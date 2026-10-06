# Running the brain on a Raspberry Pi

For now the goal is simply **run the shop without the Mac**: a Pi on the shop's WiFi doing exactly what the Mac brain does.
Packaging for customers (first-boot WiFi, read-only root) comes later.

## One-time: flash the Pi

In Raspberry Pi Imager choose **Raspberry Pi OS Lite (64-bit)** and, under the OS customisation (the gear / "Edit settings"):

| Setting | Value | Why |
|---|---|---|
| Hostname | `dustgate` | the app is then at `http://dustgate.local`. (Nothing REQUIRES mDNS: it is only how you find the Pi; use its IP if your network blocks it.) |
| Username / password | yours | the deploy script SSHes in as this user and uses `sudo` |
| SSH | enable, **allow public-key authentication** and paste your Mac's public key (`cat ~/.ssh/id_ed25519.pub`) | `deploy.sh` runs non-interactively |
| WiFi | the shop's network, correct country | |
| Locale / time zone | yours | logs carry times |

Then, **in your router, give the Pi a DHCP reservation (a fixed address)**. The brain tells plugs and nodes its address; a lease that
changes under them breaks the links until they find it again. (Nodes find a moved brain by its beacon; plugs do not.)

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

The first build on a Pi Zero 2 W takes several minutes and needs the swap `setup.sh` adds; later builds recompile only what changed.
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

Nothing here has been run on a Pi yet: the code is written to build with GCC on Debian (Boost 1.74, no `.git` needed) and the
Mac build is green, but the first Pi build may need a fix or two. Paste the compiler output and it will be quick.
Not built: a git deploy key on the Pi (the `--git` path), mDNS advertising of the brain, a read-only root, first-boot WiFi setup.
