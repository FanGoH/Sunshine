/**
 * @file src/platform/linux/gamescope_session.h
 * @brief Gamescope overlay toggle and GamePad / Azahar touch injection.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// local includes
#include "src/platform/common.h"

namespace platf::gamescope {

  constexpr std::uint32_t STEAM_CLIENT_APPID = 769;  ///< Steam client / overlay focused-app id in Game Mode.
  constexpr std::uint32_t TOUCH_SECOND_DISPLAY_POINTER = 0x80000000U;  ///< High bit set on display-index-1 pointer ids.

  /**
   * @brief Overlay show vs hide for a Guide press.
   */
  enum class overlay_action_e {
    show,  ///< Open the Steam Game Mode overlay.
    hide  ///< Close the overlay and restore the game surface.
  };

  /**
   * @brief True when an X11 title is Steam Big Picture Mode.
   *
   * @param title Window title, including UTF-8 `_NET_WM_NAME`.
   * @return True for the Steam Big Picture window used as the overlay split.
   */
  [[nodiscard]] bool title_is_steam_big_picture(std::string_view title);

  /**
   * @brief True when an X11 title is Cemu GamePad View.
   *
   * @param title Window title.
   * @return True for the Wii U GamePad window.
   */
  [[nodiscard]] bool title_is_gamepad_view(std::string_view title);

  /**
   * @brief True when an X11 title is the Cemu TV window.
   *
   * @param title Window title.
   * @return True for `Cemu 2.x` (not GamePad View, not the 10×10 helper).
   */
  [[nodiscard]] bool title_is_cemu_tv(std::string_view title);

  /**
   * @brief True when an X11 title is the Moonlight GamePad / 3DS-bottom surface.
   *
   * Cemu uses `GamePad View`. Azahar Separate Windows uses `Secondary Window`
   * for the touch screen. The library caption without Primary/Secondary is not
   * a touch target.
   *
   * @param title Window title, including UTF-8 `_NET_WM_NAME`.
   * @return True for Cemu GamePad View or Azahar Secondary Window.
   */
  [[nodiscard]] bool title_is_touch_surface(std::string_view title);

  /**
   * @brief True when an X11 title is the HDMI / TV game surface.
   *
   * Overlay hide restores this window as `GAMESCOPECTRL_BASELAYER_WINDOW`.
   * Azahar top screen is `Primary Window`. Do not match the library window or
   * the touch surface.
   *
   * @param title Window title.
   * @return True for Cemu TV or Azahar Primary Window.
   */
  [[nodiscard]] bool title_is_hdmi_surface(std::string_view title);

  /**
   * @brief Map a normalized touch point onto a window in pixels.
   *
   * @param x Horizontal coordinate in `[0, 1]`.
   * @param y Vertical coordinate in `[0, 1]`.
   * @param width Window width in pixels.
   * @param height Window height in pixels.
   * @return Clamped pixel coordinates.
   */
  [[nodiscard]] std::pair<int, int> touch_to_window_xy(float x, float y, int width, int height);

  /**
   * @brief Convert absolute mouse pixels into a unit square.
   *
   * Moonlight GamePad taps are `LiSendMousePositionEventOnDisplay` (native
   * LI_TOUCH is compiled out). Those packets are desktop pixels, not `[0, 1]`.
   * Passing them through `touch_to_window_xy` clamps every value `> 1` to the
   * far edge. Subtract the touch-port origin first.
   *
   * @param x Desktop-space X from `client_to_touchport`.
   * @param y Desktop-space Y from `client_to_touchport`.
   * @param offset_x Touch-port origin X.
   * @param offset_y Touch-port origin Y.
   * @param width Touch-port width in pixels.
   * @param height Touch-port height in pixels.
   * @return Coordinates in the same space as `touch_to_window_xy` (may be outside `[0, 1]`).
   */
  [[nodiscard]] std::pair<float, float> abs_to_unit(float x, float y, int offset_x, int offset_y, int width, int height);

  /**
   * @brief X11 display name for Steam Big Picture / overlay atoms.
   *
   * Session gamescope `--xwayland-count 2` keeps Steam on `:0`. Cemu uses
   * `FOCUS_DISPLAY=1` and lives on `:1`. kms unsets `$DISPLAY`;
   * `XOpenDisplay(nullptr)` then fails. Headless video/1 is `:2` — never
   * open that for inject or overlay.
   *
   * @return `":0"`.
   */
  [[nodiscard]] const char *session_x11_name();

  /**
   * @brief How many session Xwaylands may host GamePad View / Cemu TV.
   *
   * @return 2 (`:1` then `:0`). Does not include headless `:2`.
   */
  [[nodiscard]] std::size_t session_x11_touch_count();

  /**
   * @brief Session Xwayland that may host GamePad View, preferred first.
   *
   * Index 0 is `:1` (`FOCUS_DISPLAY=1`). Index 1 is `:0` (older Cemu-on-Steam
   * Xwayland). Out of range returns `nullptr`.
   *
   * @param index Zero-based candidate.
   * @return Display name, or `nullptr`.
   */
  [[nodiscard]] const char *session_x11_touch_name(std::size_t index);

  /**
   * @brief Choose show vs hide from the current overlay atom.
   *
   * @param overlay_is_on True when `STEAM_OVERLAY=1` on Steam Big Picture.
   * @return Action for this Guide press.
   */
  [[nodiscard]] overlay_action_e overlay_toggle_action(bool overlay_is_on);

  /**
   * @brief Pick the Steam shortcut id to restore after overlay close.
   *
   * Prefers `STEAM_GAME` on the HDMI game window (Cemu TV or Azahar Primary).
   * Falls back to `GAMESCOPE_FOCUSED_APP_GFX` or `GAMESCOPE_FOCUSED_APP` when
   * those are not the Steam client id.
   *
   * @param steam_game `STEAM_GAME` on the HDMI game window, if present.
   * @param focused_app Root `GAMESCOPE_FOCUSED_APP`.
   * @param focused_gfx Root `GAMESCOPE_FOCUSED_APP_GFX`.
   * @param steam_client_id Steam client id to ignore (`769`).
   * @return Shortcut id, or `0` when unknown.
   */
  [[nodiscard]] std::uint32_t resolve_overlay_appid(
    std::optional<std::uint32_t> steam_game,
    std::optional<std::uint32_t> focused_app,
    std::optional<std::uint32_t> focused_gfx,
    std::uint32_t steam_client_id = STEAM_CLIENT_APPID
  );

  /**
   * @brief True when this touch belongs to Moonlight display index 1.
   *
   * @param pointer_id Pointer id from `platf::touch_input_t`.
   * @return True when the high bit is set.
   */
  [[nodiscard]] bool touch_is_second_display(std::uint32_t pointer_id);

  /**
   * @brief True when Game Mode should warp this absolute mouse onto GamePad View.
   *
   * Dual-stream / stacked Moonlight tags GamePad fingers as display 1. Odin
   * **GamePad only** is a single stream (`x-ml-video[0].source=secondary`) so
   * those taps are display 0. HDMI/TV taps stay display 0 without
   * `primary_from_secondary` and must not take this path.
   *
   * @param display_index Zero-based Moonlight display index.
   * @param primary_from_secondary True when video/0 captures the GamePad display.
   * @return True when inject should own this packet.
   */
  [[nodiscard]] bool abs_targets_gamepad_view(std::size_t display_index, bool primary_from_secondary);

}  // namespace platf::gamescope

namespace platf {

  /**
   * @brief Toggle the Steam gamescope overlay on a Guide/HOME rising edge.
   *
   * libvirtualhid x360 is UHID bluetooth (`045e:028e` bus `0005`). Steam Game
   * Mode does not honor Guide on that path. `back_button_timeout` already
   * pulses HOME; this writes `STEAM_OVERLAY` on a **mapped** Steam Big Picture
   * window plus `GAMESCOPE_FOCUSED_APP=769`. If Big Picture is unmapped (common
   * for standalone Flatpak shortcuts), it sends `steam://overlay/toggle` instead.
   */
  void gamescope_on_guide_press();

  /**
   * @brief Send a display-1 touch to the GamePad / Azahar bottom window.
   *
   * Game Mode keeps HDMI and the touch surface stacked at session `:1` `0,0`
   * (Steam stays on `:0`). Host
   * uinput hits the raised TV / Primary Window. XSendEvent targets GamePad
   * View or Azahar Secondary Window without raising it.
   *
   * @param touch_port Viewport used to size the event (unused for 0–1 coords).
   * @param touch Touch event in monitor-local `[0, 1]` coordinates.
   * @return True when the event was delivered to the touch surface.
   */
  bool inject_gamepad_view_touch(const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Move the GamePad / Azahar-bottom pointer from a display-1 abs mouse packet.
   *
   * Fangoh Moonlight maps finger taps with `sendMousePositionOnDisplay(..., 1)`
   * plus a later mouse-button packet that has no display index. Host uinput
   * then clicks the raised TV at the last gamescope cursor (often center),
   * which is why Wind Waker's item pad jumps to the middle of the GamePad.
   *
   * @param touch_port Display-1 touch port (`offset` + env size in pixels).
   * @param x Desktop-space X from `client_to_touchport`.
   * @param y Desktop-space Y from `client_to_touchport`.
   * @return True when the touch surface received the motion.
   */
  bool inject_gamepad_view_abs_mouse(const touch_port_t &touch_port, float x, float y);

  /**
   * @brief Click the GamePad / Azahar-bottom window after a display-1 abs mouse move.
   *
   * @param button Moonlight mouse button (`BUTTON_LEFT` is `1`, same as X11).
   * @param release True for button-up.
   * @return True when the touch surface received the button event.
   */
  bool inject_gamepad_view_button(int button, bool release);

}  // namespace platf
