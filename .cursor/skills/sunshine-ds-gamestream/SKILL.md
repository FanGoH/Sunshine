---
name: sunshine-ds-gamestream
description: Diagnose SteamOS GameStream on sunshine-ds vs Decky Sunshine (black screen, Moonlight 503, Starting Desktop hang, reconnect after drop, Thor/Moonlight DS, KWin screencast). Use when the user mentions Sunshine, sunshine-ds, GameStream, Moonlight, 503, black capture, Starting Desktop, encoder probe, KWin PipeWire, or dual-stream HDMI/virtual.
---

# sunshine-ds GameStream debug

Read this **before** tracing capture or `/launch` from scratch. Host uniqueids and LAN IPs live in gitignored `.env` / `rules_of_the_land.md` (this box: `~/.cursor/skills/sunshine-ds-gamestream/host.md`).

## Start here (do not rediscover)

1. Confirm which HTTP the client hits. Decky Flatpak and dev sunshine-ds are different processes.

```bash
curl -s --max-time 3 http://127.0.0.1:48100/serverinfo | grep -E 'uniqueid|currentgame|state'
curl -s --max-time 3 http://127.0.0.1:47989/serverinfo | grep -E 'uniqueid|currentgame|state'
```

Thor must use the **dev** uniqueid / `:48100`. `:47989` is Decky KMS and is black on Plasma desktop.

2. Confirm the **running** pid is the installed binary (`pgrep -x sunshine-ds` only — never `pgrep -f` / `pkill -f`):

```bash
ps -o pid,lstart,cmd -p "$(pgrep -x sunshine-ds)"
ls -l /home/deck/.local/bin/sunshine-ds
```

If `lstart` is older than the binary mtime, the process does not have the latest fixes.

3. Match the log to the table below. Apply that fix. Do not start a new capture theory.

## Known-good checkpoint

This is the working GameStream baseline. Do not “improve” it unless the user asks.

- Conf `~/.config/sunshine-ds-dev/sunshine/sunshine.conf`: `capture = kwin`, `encoder = software`, `hevc_mode = 1`, `av1_mode = 1`, `port = 48100`, `output_name = HDMI-A-1`. Dual stream is HDMI twice until `Virtual-sunshine-ds` exists.
- Start env: Distrobox `steamos-tools`, `CONFIGURATION_DIRECTORY=/home/deck/.config/sunshine-ds-dev`, `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`, `WAYLAND_DISPLAY=wayland-0`, `unset DISPLAY`.
- After `/launch` the log must contain `Skipping encoder re-probe; using [software]` (not a vulkan/vaapi walk).
- Capture health: `cpu frame type=2` + high `pixel_diffs`. Probe I-frame ~1KB / 0% coded is `dummy_img()`, ignore it.
- Reconnect must keep the same pid. If log shows `drop_elevated_privileges` then `zkde_screencast_unstable_v1 not found`, that pid is dead for capture — restart DS.
- Do **not** replace Decky Sunshine. Do **not** `POST /api/restart`. Do **not** `sudo systemctl --user`. Do **not** `kwin_wayland --replace`.

Code that must stay in the running binary: skip software `ALWAYS_REPROBE` on `/launch`; flush KWin Close before PipeWire stop; async `pw_stream_destroy`; never `drop_elevated_privileges` after KWin was bound this process.

## Symptom → cause → fix

### Moonlight 503 on reconnect

`/launch` failed: no display/encoder. Log:

- `KWin screencasting unavailable after init` then `drop_elevated_privileges`
- then `zkde_screencast_unstable_v1 not found in registry`
- then `Fatal: Unable to find display or encoder`

Privilege drop is **process-wide**. `/serverinfo` can still be `FREE`. Restart sunshine-ds (new pid).

### “Starting Desktop” for ~75s

Encoder probe opened a KWin screencast; destructor did not Close before PipeWire stop; KWin held the node ~75s. DS HTTP was down, so clients fell through to Decky.

If it returns, the running pid is older than the skip-reprobe / async-teardown install.

### Black Moonlight / ~1KB I-frames

| Check | Meaning |
|---|---|
| Spectacle screenshot all black | KWin FBO wedged. `qdbus org.kde.KWin /Compositor org.kde.kwin.Compositing.reinitialize`. Playbook: `scripts/ensure-kwin-screencast.sh`. |
| Probe I-frame ~1200 bytes / 0% coded | `dummy_img()`, not live capture |
| `cpu frame type=2` + high `pixel_diffs` | SHM/MemFd is capturing |
| DMA-BUF DCC modifier + mmap EPERM | Do not offer DMA-BUF for software encode |

### Ghost BUSY / wrong app

Desktop placebo app stays BUSY until `POST /api/apps/close`. HTTPS `/cancel` needs client cert. Use Decky `lastAuthHeader`; CSRF skipped if no Origin/Referer. Playbook helper: `sunshine_close_app_via_api`. Do not tap a Low Res Desktop app unless asked.

## Restart sunshine-ds

```bash
pgrep -x sunshine-ds   # never pgrep -f / pkill -f
kill <pid>
podman exec --user 1000 -d steamos-tools bash -lc 'export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus PIPEWIRE_RUNTIME_DIR=/run/user/1000 CONFIGURATION_DIRECTORY=/home/deck/.config/sunshine-ds-dev HOME=/home/deck KWIN_WAYLAND_NO_PERMISSION_CHECKS=1; unset DISPLAY; exec /home/deck/.local/bin/sunshine-ds /home/deck/.config/sunshine-ds-dev/sunshine/sunshine.conf >> /home/deck/steamos-playbook/logs/sunshine-ds.log 2>&1'
```

Wait until `:48100` `/serverinfo` is `SUNSHINE_SERVER_FREE` with the **dev** uniqueid.

```bash
podman exec --user 1000 steamos-tools ninja -C /home/deck/code/sunshine-ds/build -j2 sunshine
install -m 0755 /home/deck/code/sunshine-ds/build/sunshine /home/deck/.local/bin/sunshine-ds
```

Over SSH: `export XDG_RUNTIME_DIR=/run/user/$(id -u)`.

## Client / protocol

- Moonlight DS package and Thor ADB: `rules_of_the_land.md` / `host.md`. Do not `adb kill-server`.
- `/serverinfo` must advertise `<MaxVideoStreams>2</MaxVideoStreams>`.
- SETUP `streamid=video/1/0` before ANNOUNCE.
- Distrobox Pulse often `Access denied` — video can work without DS audio.

## Capture smoke pages

`/tmp/sunshine-ds-smoke` on `http://127.0.0.1:18080` (repo: `tools/sunshine-ds-smoke/`).

- `primary.html` — HDMI, red, bouncing box + Gamepad API HUD
- `gamepad.html` — second stream, blue; left stick moves the box
- Chrome Gamepad API is empty until a button press

## Do not

- Treat probe I-frame size as capture health
- Enable Flatpak Sunshine systemd user unit
- Poll `/api/restart` or restart Decky to “fix” DS
- Hardcode Headscale URLs or print `.auth` / certs / passwords
- Install Bazzite Eden reorder hooks
