---
name: sunshine-ds-gamestream
description: Diagnose SteamOS GameStream on sunshine-ds vs Decky Sunshine (black screen, Moonlight 503, Starting Desktop hang, reconnect after drop, Thor/Moonlight DS, KWin screencast). Use when the user mentions Sunshine, sunshine-ds, GameStream, Moonlight, 503, black capture, Starting Desktop, encoder probe, KWin PipeWire, or dual-stream HDMI/virtual.
---

# sunshine-ds GameStream debug

Read this before tracing capture or `/launch` from scratch. Host facts change; symptoms and recovery do not.

## Two servers (do not mix)

| | Decky Flatpak | Dev sunshine-ds |
|---|---|---|
| HTTP | `:47989` | `:48100` |
| HTTPS | `:47984` | `:48095` |
| Web UI | — | `:48101` |
| RTSP | default | `:48121` |
| uniqueid | `25450B84-9F6E-E308-6E9D-F07A25BD6CD2` | `E2C181AA-F002-A006-1D11-800D02FDBE7C` |
| Capture | KMS (black on Plasma desktop) | `capture = kwin` |

Thor must pin **`192.168.68.65:48100`** (or Tailscale `100.64.0.8:48100`). Hitting `:47989` is Decky KMS black.

```bash
curl -s --max-time 3 http://127.0.0.1:48100/serverinfo | grep -E 'uniqueid|currentgame|state'
curl -s --max-time 3 http://127.0.0.1:47989/serverinfo | grep -E 'uniqueid|currentgame|state'
```

Conf: `~/.config/sunshine-ds-dev/sunshine/sunshine.conf`  
Binary: `/home/deck/.local/bin/sunshine-ds` (install from `~/code/sunshine-ds/build/sunshine`)  
Do **not** replace Decky Sunshine. Do **not** `POST /api/restart`. Do **not** `sudo systemctl --user`.

## Symptom → cause → fix

### Moonlight 503 on reconnect

`/launch` failed: no display/encoder. Log:

- `KWin screencasting unavailable after init` then `drop_elevated_privileges`
- then `zkde_screencast_unstable_v1 not found in registry`
- then `Fatal: Unable to find display or encoder`

Privilege drop is **process-wide**. That pid cannot capture again. `/serverinfo` can still be `FREE` on `:48100`.

**Fix:** restart sunshine-ds (new pid). Do not keep talking to the poisoned process.

Old binaries also re-probed `ALWAYS_REPROBE` software on every `/launch` (vulkan/vaapi/software), which is what triggered the drop. Current code logs `Skipping encoder re-probe; using [software]` and must **not** drop caps after KWin was already bound this process.

### “Starting Desktop” for ~75s

Encoder probe created a KWin screencast, destructor did not flush Close before PipeWire stop, KWin held the node ~75s. HTTP `:48100` was down until probe finished, so clients fell through to Decky `:47989`.

**Fix already in tree:** Close+flush before PW stop; skip software re-probe when `encoder = software`; async `pw_stream_destroy`. If it returns, confirm the **running** pid started after that install (`ps -o lstart,cmd -p $(pgrep -x sunshine-ds)` vs `ls -l ~/.local/bin/sunshine-ds`).

### Black Moonlight / ~1KB I-frames

| Check | Meaning |
|---|---|
| Spectacle screenshot all black | KWin screenshot/screencast FBO wedged. `qdbus org.kde.KWin /Compositor org.kde.kwin.Compositing.reinitialize`. Playbook: `scripts/ensure-kwin-screencast.sh`. **Never** `kwin_wayland --replace`. |
| Probe I-frame ~1200 bytes / 0% coded | `dummy_img()`, not live capture health |
| `cpu frame type=2` + high `pixel_diffs` | SHM/MemFd path is actually capturing |
| DMA-BUF DCC modifier + mmap EPERM | Do not offer DMA-BUF for software encode |

`encoder = software`, `hevc_mode = 1`, `av1_mode = 1`. Dual stream: HDMI-A-1 twice until `Virtual-sunshine-ds` exists (`createVirtualOutput` / helper `/home/deck/.local/bin/sunshine-ds-virtual-output`).

### Ghost BUSY / wrong app

Desktop app id **881448767** is a placebo; stays BUSY until `POST /api/apps/close`. Do not tap Low Res Desktop `303580669`. HTTPS `/cancel` needs client cert. Use Decky `lastAuthHeader` (user `decky_sunshine`); CSRF skipped if no Origin/Referer. Playbook helper: `sunshine_close_app_via_api`.

## Restart sunshine-ds

Kill by **exact** name only:

```bash
pgrep -x sunshine-ds   # never pgrep -f / pkill -f
kill <pid>
```

Start inside Distrobox `steamos-tools` (Fedora 42):

```bash
podman exec --user 1000 -d steamos-tools bash -lc 'export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus PIPEWIRE_RUNTIME_DIR=/run/user/1000 CONFIGURATION_DIRECTORY=/home/deck/.config/sunshine-ds-dev HOME=/home/deck KWIN_WAYLAND_NO_PERMISSION_CHECKS=1; unset DISPLAY; exec /home/deck/.local/bin/sunshine-ds /home/deck/.config/sunshine-ds-dev/sunshine/sunshine.conf >> /home/deck/steamos-playbook/logs/sunshine-ds.log 2>&1'
```

Wait until `:48100` `/serverinfo` is `SUNSHINE_SERVER_FREE` and uniqueid is the **dev** one. After `/launch`, log must contain `Skipping encoder re-probe` (not a full vulkan/vaapi walk).

Build/install:

```bash
podman exec --user 1000 steamos-tools ninja -C /home/deck/code/sunshine-ds/build -j2 sunshine
install -m 0755 /home/deck/code/sunshine-ds/build/sunshine /home/deck/.local/bin/sunshine-ds
```

Over SSH: `export XDG_RUNTIME_DIR=/run/user/$(id -u)`.

## Client / protocol

- Moonlight package `com.fangoh.moonlight.debug`. Thor wireless ADB `192.168.68.60:34151`. Phone `192.168.68.50`.
- `/serverinfo` must advertise `<MaxVideoStreams>2</MaxVideoStreams>`.
- SETUP `streamid=video/1/0` before ANNOUNCE.
- Do not `adb kill-server`.
- Distrobox Pulse often `Access denied` — DS audio may be missing; video can still work.

## Capture smoke pages

Served from `/tmp/sunshine-ds-smoke` on `http://127.0.0.1:18080` (also `tools/sunshine-ds-smoke/` in this repo).

- `primary.html` — HDMI 1920×1080, red, bouncing box + Gamepad API HUD
- `gamepad.html` — second stream, blue, same HUD; left stick moves the box
- Chrome: `gamepadconnected` is empty until a button press. Flatpak Chrome already has `devices=all`.

```bash
python3 -m http.server 18080 --bind 127.0.0.1 --directory /tmp/sunshine-ds-smoke
# profiles: /tmp/sunshine-ds-smoke/chrome-primary2 and chrome-gamepad2
# cache-bust: --app=http://127.0.0.1:18080/primary.html?m=$EPOCH
```

## Do not

- Treat probe I-frame size as capture health
- Enable Flatpak Sunshine systemd user unit
- Poll `/api/restart` or restart Decky to “fix” DS
- Hardcode Headscale URLs or print `.auth` / certs / passwords
- Install Bazzite Eden reorder hooks
