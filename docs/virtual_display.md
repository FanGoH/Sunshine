# The virtual display

The second display a client streams is normally not a monitor anyone owns. It is
the size of the client's second panel, it should exist only while a session is
running, and no physical monitor is likely to match it.

## Driver requirement

Sunshine DS controls a virtual display driver; it does not install or implement
one. Windows virtual monitors are provided by signed indirect display drivers
(IddCx), so one of the supported drivers must already be installed:

- [SudoVDA](https://github.com/SudoMaker/SudoVDA) is preferred. Its control
  protocol creates a monitor at the client's exact requested width, height, and
  refresh rate for each session.
- [MikeTheTech Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
  is supported as a compatibility fallback. Its available modes are configured
  before the driver starts, so Sunshine DS can select only a mode the running
  driver already exposes.

`libdisplaydevice`, which Sunshine already uses, configures display topology and
modes. It cannot add a new mode to a running indirect display driver.

## Configuration

Set `dual_display_source` in `sunshine.conf`:

| Value | Meaning |
| --- | --- |
| *(empty)* | The feature is off. This is the default. |
| `virtual` | Acquire a supported virtual display for the client's second panel. |
| `gamescope-virtual` | Linux Game Mode: capture a headless gamescope PipeWire node (not KWin). |
| `pipewire:<serial>` | Linux: capture that PipeWire `object.serial` as video/1. |
| anything else | Capture the real monitor identified by that output name or stable device identifier. |

The server advertises two-stream support only when the configured source can be
resolved. If acquisition later fails, the session continues with its primary
stream rather than failing the whole connection.

### SudoVDA lifecycle

For `dual_display_source = virtual`, Sunshine DS first opens the installed
SudoVDA interface and verifies its protocol version. It then:

1. derives a stable monitor identity from the paired client;
2. creates a monitor at the exact client-requested mode;
3. extends the Windows desktop and resolves the new target to its GDI/DXGI
   output name;
4. keeps the driver's watchdog alive for the session; and
5. removes the monitor when the second stream ends.

The stable identity lets Windows remember the monitor's position between
reconnects without accumulating a new ghost monitor for every session. An
overlapping session cannot claim the same identity.

### MikeTheTech fallback

When SudoVDA is unavailable, Sunshine DS can lease a recognized MikeTheTech
virtual output. It can use an already active output or activate a detached one
by stable device identifier. It does not treat an arbitrary disconnected
physical monitor as virtual.

The driver reads its modes from `vdd_settings.xml` when it initializes. The
default location is `C:\VirtualDisplayDriver\vdd_settings.xml`; a `VDDPATH`
value under `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver` can override that
directory. Add every client-panel mode that must be exact before restarting the
driver. For example, an AYN Thor lower panel entry is:

```xml
<resolution>
    <width>1240</width>
    <height>1080</height>
    <refresh_rate>30</refresh_rate>
</resolution>
```

Editing the XML alone does not update the running driver's mode table. Restart
only the virtual display device, or reboot Windows, after changing it. The
driver's named-pipe reload command in existing releases does not add a new
resolution to an already initialized mode table.

At session start Sunshine DS chooses an exact enumerated mode when available.
If the driver does not expose one, it logs the nearest supported mode and the
encoder scales or letterboxes that capture to the client dimensions. An exact
driver mode avoids that extra conversion and preserves one-to-one desktop and
touch geometry.

For an existing virtual output, Sunshine DS records the original Windows mode.
On teardown it restores that mode only if the output is still using the mode
Sunshine applied; a mode changed by the user or another component is preserved.
An automatically activated output also restores its preceding topology.

### Using a real monitor

Set `dual_display_source` to the output name or stable display identifier of an
attached monitor. Nothing is created or removed. Capture uses that display and
the encoder produces the dimensions requested by the client.

### Linux / KWin virtual output

On Linux, `dual_display_source = virtual` creates a compositor virtual monitor
with `sunshine-ds-virtual-output` (`zkde_screencast_unstable_v1.stream_virtual_output`)
and captures it with `capture = kwin`. The helper is stopped when the second
stream ends. Physical outputs are not disabled. A matching KWin permission
desktop file is required (`X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`).

KMS cannot see a KWin-only virtual monitor. Named existing outputs
(`dual_display_source = Virtual-sunshine-ds`, or a second HDMI) work without
creating anything, still with `capture = kwin`.

On KWin 6.7, `stream_virtual_output` often fails immediately with
`Could not find output` (`workspace()->findOutput` before the LogicalOutput
exists). The helper must keep the Wayland stream open anyway, then enable the
output with `kscreen-doctor`. Exiting the helper removes `Virtual-sunshine-ds`.

### Linux / Game Mode headless gamescope

Gamescope has no `zkde_screencast_unstable_v1`. A second KMS plane does not
exist for a Cemu GamePad either. The playbook holds a *headless* gamescope
(`--backend headless`) that publishes a PipeWire `Video/Source`. Set
`capture = kms` for HDMI on video/0 and `dual_display_source = gamescope-virtual`
so video/1 attaches to that node (sidecar `$XDG_RUNTIME_DIR/sunshine-ds-gamemode-virtual`,
or `pipewire:<object.serial>`). Do not set `virtual` here — that still spawns
the KWin helper. A static surface may emit only one PipeWire buffer; software
encode must copy that CPU frame into `dummy_img()` and re-present it, or
video/1 stays black.

Hold-Select (`back_button_timeout`) pulses HOME on the libvirtualhid x360.
That pad is UHID bluetooth, so Steam Game Mode ignores Guide. Sunshine then
toggles `STEAM_OVERLAY` on Steam Big Picture (or the largest `STEAM_GAME=769`
window when that title is missing) plus `GAMESCOPE_FOCUSED_APP=769`.
Display-index-1 finger taps from Fangoh Moonlight are **absolute mouse**
(`sendMousePositionOnDisplay(..., displayIndex=1)` plus a later button packet
with no display index). Native `LiSendTouchEvent` is compiled out. Sunshine
maps those packets onto Cemu **GamePad View** or Azahar **Secondary Window**
on session gamescope (`:1` first, then `:0`; never headless `:2`).
`packet_to_unit` is linear `x/width` — Thor Stretch fills 1240×1080 and the
old client sends `ref=1239x1079` (protocol stores width−1). Do **not**
unletterbox Fit bars (that sheared Y). Fit already shrinks StreamView to 16:9.
A stream-sized ref (newer Moonlight) is the same linear map. Do not run
`client_to_touchport`. wx/GTK drop `XSendEvent` (`send_event` is always True
on the wire). Overlay-tag + opacity 0 also skip gamescope hit-test, so
briefly clear those tags, `XTestFakeMotionEvent` / `XTestFakeButtonEvent` onto
the GL child, then re-cover. `GAMESCOPE_FOCUS_DISPLAY` writes go to session
`:0` and must `XFlush` that connection (GamePad Xlib talks to `:1`). Middle
must stay **1** while Cemu is on `:1`. Do not also uinput. Overlay hide
restores Cemu TV or Azahar **Primary Window**. Odin **GamePad only** is
display 0 with `x-ml-video[0].source=secondary` (`primary_from_secondary`);
HDMI/TV taps stay display 0 without that flag. GamePad-only must not take
the HDMI path. **THE Game Mode standard is `checkpoint-2026-09-18-gamepad-xtest`**
(user: “WE DID IT”; host `4ca50111`). Live: `GamePad pkt x=165,332
ref=1239x1079 unit=0.133,0.308` then `abs` / `button-down` on the 1920×1080
GL child. Hold-Select overlay (`STEAM_OVERLAY=1`) must warp Steam Big Picture
on `:0` (`HDMI inject: overlay on :0` / `overlay-abs`), not Cemu TV. Do not
raise GamePad over TV.

## Behavior without a supported source

When the feature is disabled or no configured source is available,
`dual_display::supported()` reports false. The server then advertises one video
stream and refuses setup of `streamid=video/1/0`. Primary streaming, audio, and
input retain normal Sunshine behavior.
