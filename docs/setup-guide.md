# Norns on Move — Setup Guide (via Schwung)

Complete walkthrough for deploying Monome Norns on Ableton Move via Schwung.

## Prerequisites

- Ableton Move with [Schwung](https://github.com/charlesvestal/schwung) installed
- PipeWire module working (via [Schwung Installer](https://github.com/charlesvestal/schwung-installer))
- Debian sid arm64 chroot at `/data/UserData/pw-chroot` with PipeWire + JACK
- SSH access to Move (`ssh root@move.local`)
- Docker installed on your build machine (for cross-compilation)

## 1. Build the Module

On your host machine (not the Move):

```bash
git clone <this-repo>
cd schwung-norns
./scripts/build.sh
```

This cross-compiles three aarch64 binaries:
- `dsp.so` — DSP plugin (audio FIFO bridge + screen FIFO pump)
- `pw-helper` — setuid root helper for chroot management
- `norns-input-bridge` — MIDI to encoder/key event translator

Output: `dist/norns/` directory and `dist/norns-module.tar.gz`

## 2. Deploy to Move

```bash
DEVICE_HOST=move.local ./scripts/install.sh
```

This deploys:
- Module files to `/data/UserData/schwung/modules/tools/norns/`
- `pw-helper-norns` (setuid root) to `/data/UserData/schwung/bin/`
- `norns-input-bridge` to chroot at `/usr/local/bin/`
- PipeWire no-RT config (prevents SCHED_FIFO conflicts with Move's audio engine)

## 3. Install Norns in Chroot

SSH into Move and run the setup script:

```bash
scp scripts/setup-norns.sh root@move.local:/data/
ssh root@move.local
sh /data/setup-norns.sh
```

This installs (~10-30 minutes first time):
- SuperCollider server + sc3-plugins
- Norns dependencies (Lua 5.3, Cairo, liblo, etc.)
- Clones and builds matron + crone from monome/norns
- Clones and builds Maiden web IDE
- Installs starter scripts (awake, molly_the_poly, passersby)
- Configures scsynth for 44100 Hz (matching Move's sample rate)

### Applying Matron Patches

After setup-norns.sh completes, you need to apply the screen and input patches to matron. See `patches/` directory for patch specifications:

- **screen-fifo.patch**: Modifies matron to write framebuffer to a FIFO instead of SPI
- **input-virtual.patch**: Modifies matron to read encoder/key events from a FIFO instead of GPIO

Apply these to the norns source in the chroot and rebuild matron:

```bash
ssh root@move.local
chroot /data/UserData/pw-chroot bash -l
cd /home/we/norns/matron
# Apply patches (modify screen.c and input.c per patch descriptions)
./waf build
```

## 4. Load in Schwung

1. On the Move, navigate to Schwung
2. Load "Norns" as a tool in any shadow chain slot
3. The module starts PipeWire, SuperCollider, crone, matron, and Maiden automatically

## 5. Access Maiden Web IDE

Open in a browser:

```
http://move.local:5000
```

From Maiden you can:
- Browse and load scripts
- Edit Lua code
- Install community scripts from the catalog
- Use the REPL (`;restart` to restart matron)

## Controls

| Move Control | Norns Function |
|-------------|----------------|
| Knob 1 | E1 (menu/level) |
| Knob 2 | E2 (navigate/param) |
| Knob 3 | E3 (navigate/param) |
| Pad 1 | K1 (menu/alt) |
| Pad 2 | K2 (action/select) |
| Pad 3 | K3 (action/select) |
| Pad 4 | Restart Norns |

Knobs send CC 71-73, translated to encoder deltas by `norns-input-bridge`.
Pads send notes 36-38 (K1-K3) and 39 (restart).

## Installing Scripts

### Via Maiden
1. Open `http://move.local:5000`
2. Click the package manager icon (box icon)
3. Browse community scripts or enter a Git URL
4. Click install

### Via SSH
```bash
ssh root@move.local
chroot /data/UserData/pw-chroot bash -l
su - move
cd /home/we/dust/code
git clone https://github.com/<user>/<script>.git
```

## Troubleshooting

### No audio
- Check PipeWire is running: `chroot /data/UserData/pw-chroot pgrep pipewire`
- Check crone is running: `chroot /data/UserData/pw-chroot pgrep crone`
- Verify the FIFO exists: `ls -la /tmp/pw-to-move-*`
- Check Move isn't muted (mute toggle in Schwung can silently mute a slot)
- Check logs: `cat /tmp/norns-dsp-debug.log`

### Screen is blank (shows "Starting...")
- Matron may not be running: `chroot /data/UserData/pw-chroot pgrep matron`
- Screen FIFO patch may not be applied — see "Applying Matron Patches" above
- Check screen FIFO: `ls -la /tmp/norns-screen-*`

### SuperCollider not starting
- scsynth is optional — matron + crone still work for softcut-only scripts
- Check if installed: `chroot /data/UserData/pw-chroot which scsynth`
- Must be launched with `-r 44100` (setup-norns.sh configures this)

### Maiden not accessible
- Check if running: `chroot /data/UserData/pw-chroot pgrep maiden`
- Default port 5000 — ensure no firewall blocking
- Try `http://<move-ip>:5000` if mDNS isn't working

### Processes getting killed
- Likely SCHED_FIFO inheritance issue — ensure `start-norns.sh` uses `chrt -o 0` before all chroot calls
- Check PipeWire no-RT config: `cat /data/UserData/pw-chroot/etc/pipewire/pipewire.conf.d/no-rt.conf`
- Should contain `context.properties = { module.rt = false }`

### Restarting Norns
- Press Pad 4 on Move
- Or via Maiden REPL: type `;restart`
- Or via SSH: kill matron, it will be restarted on next module load

## Architecture

```
Schwung Host                            Debian sid arm64 Chroot
┌─────────────────────┐                ┌──────────────────────────────┐
│ norns DSP plugin    │                │  matron (Lua VM + Cairo)     │
│  (dsp.so)           │                │    ↕ OSC                     │
│                     │  audio FIFO    │  crone (JACK client+softcut) │
│  render_block() ◄───┼────────────────┤    ↕ JACK                   │
│                     │                │  scsynth (SuperCollider)     │
│  on_midi() ────────►┼── midi FIFO ──►│  norns-input-bridge         │
│  pump_midi_out() ◄──┼── midi FIFO ──┤    → matron virtual input    │
│                     │                │                              │
│  ui.js ◄────────────┼── screen FIFO─┤  matron screen.c (patched)   │
│  (128x64 OLED)      │               │                              │
│                     │                │  Maiden (web IDE, port 5000) │
└─────────────────────┘                └──────────────────────────────┘
```
