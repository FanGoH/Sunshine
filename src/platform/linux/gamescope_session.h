/**
 * @file src/platform/linux/gamescope_session.h
 * @brief Gamescope overlay toggle and Cemu GamePad View touch injection.
 */
#pragma once

// standard includes
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
   * @brief Choose show vs hide from the current overlay atom.
   *
   * @param overlay_is_on True when `STEAM_OVERLAY=1` on Steam Big Picture.
   * @return Action for this Guide press.
   */
  [[nodiscard]] overlay_action_e overlay_toggle_action(bool overlay_is_on);

  /**
   * @brief Pick the Steam shortcut id to restore after overlay close.
   *
   * Prefers `STEAM_GAME` on the Cemu TV window. Falls back to
   * `GAMESCOPE_FOCUSED_APP_GFX` or `GAMESCOPE_FOCUSED_APP` when those are not
   * the Steam client id.
   *
   * @param steam_game `STEAM_GAME` on the game window, if present.
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

}  // namespace platf::gamescope

namespace platf {

  /**
   * @brief Toggle the Steam gamescope overlay on a Guide/HOME rising edge.
   *
   * libvirtualhid x360 is UHID bluetooth (`045e:028e` bus `0005`). Steam Game
   * Mode does not honor Guide on that path. `back_button_timeout` already
   * pulses HOME; this writes `STEAM_OVERLAY` on Steam Big Picture plus
   * `GAMESCOPE_FOCUSED_APP=769`. No-op when Big Picture is missing.
   */
  void gamescope_on_guide_press();

  /**
   * @brief Send a display-1 touch to Cemu GamePad View instead of host uinput.
   *
   * Game Mode keeps TV and GamePad stacked at `:0` `0,0`. Host uinput hits the
   * raised TV. XSendEvent targets the GamePad X11 window without raising it.
   *
   * @param touch_port Viewport used to size the event (unused for 0–1 coords).
   * @param touch Touch event in monitor-local `[0, 1]` coordinates.
   * @return True when the event was delivered to GamePad View.
   */
  bool inject_gamepad_view_touch(const touch_port_t &touch_port, const touch_input_t &touch);

}  // namespace platf
