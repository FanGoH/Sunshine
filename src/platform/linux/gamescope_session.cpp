/**
 * @file src/platform/linux/gamescope_session.cpp
 * @brief Gamescope overlay toggle and GamePad / Azahar touch injection.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef SUNSHINE_BUILD_X11
  // platform includes
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
#endif

// local includes
#include "gamescope_session.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::gamescope {

  bool title_is_steam_big_picture(std::string_view title) {
    return title.find("Steam Big Picture Mode") != std::string_view::npos;
  }

  bool title_is_gamepad_view(std::string_view title) {
    return title.find("GamePad View") != std::string_view::npos;
  }

  bool title_is_cemu_tv(std::string_view title) {
    if (title.find("GamePad") != std::string_view::npos) {
      return false;
    }
    return title.find("Cemu ") != std::string_view::npos;
  }

  bool title_is_touch_surface(std::string_view title) {
    if (title_is_gamepad_view(title)) {
      return true;
    }
    return title.find("Secondary Window") != std::string_view::npos;
  }

  bool title_is_hdmi_surface(std::string_view title) {
    if (title_is_touch_surface(title)) {
      return false;
    }
    if (title_is_cemu_tv(title)) {
      return true;
    }
    return title.find("Primary Window") != std::string_view::npos;
  }

  std::pair<int, int> touch_to_window_xy(float x, float y, int width, int height) {
    if (width <= 0 || height <= 0) {
      return {0, 0};
    }
    const auto px = std::clamp(static_cast<int>(std::lround(std::clamp(x, 0.0F, 1.0F) * static_cast<float>(width - 1))), 0, width - 1);
    const auto py = std::clamp(static_cast<int>(std::lround(std::clamp(y, 0.0F, 1.0F) * static_cast<float>(height - 1))), 0, height - 1);
    return {px, py};
  }

  std::pair<float, float> abs_to_unit(float x, float y, int offset_x, int offset_y, int width, int height) {
    const auto nx = width > 0 ? (x - static_cast<float>(offset_x)) / static_cast<float>(width) : 0.0F;
    const auto ny = height > 0 ? (y - static_cast<float>(offset_y)) / static_cast<float>(height) : 0.0F;
    return {nx, ny};
  }

  const char *session_x11_name() {
    return ":0";
  }

  overlay_action_e overlay_toggle_action(bool overlay_is_on) {
    return overlay_is_on ? overlay_action_e::hide : overlay_action_e::show;
  }

  std::uint32_t resolve_overlay_appid(
    std::optional<std::uint32_t> steam_game,
    std::optional<std::uint32_t> focused_app,
    std::optional<std::uint32_t> focused_gfx,
    std::uint32_t steam_client_id
  ) {
    if (steam_game && *steam_game != 0 && *steam_game != steam_client_id) {
      return *steam_game;
    }
    if (focused_gfx && *focused_gfx != 0 && *focused_gfx != steam_client_id) {
      return *focused_gfx;
    }
    if (focused_app && *focused_app != 0 && *focused_app != steam_client_id) {
      return *focused_app;
    }
    return 0;
  }

  bool touch_is_second_display(std::uint32_t pointer_id) {
    return (pointer_id & TOUCH_SECOND_DISPLAY_POINTER) != 0;
  }

  bool abs_targets_gamepad_view(std::size_t display_index, bool primary_from_secondary) {
    if (display_index == 1) {
      return true;
    }
    return display_index == 0 && primary_from_secondary;
  }

}  // namespace platf::gamescope

#ifdef SUNSHINE_BUILD_X11
namespace {

  std::mutex x11_lock;  ///< Serializes Xlib calls on the cached display.
  Display *cached_display = nullptr;  ///< Reused X11 connection for `:0` gamescope.
  Window cached_pad = None;  ///< Last GamePad View / Azahar Secondary xid.
  bool pad_pointer_down = false;  ///< True while display-1 contact is held on GamePad View.
  int last_pad_x = 0;  ///< Last GamePad-local pointer X (for mouse-button packets).
  int last_pad_y = 0;  ///< Last GamePad-local pointer Y (for mouse-button packets).
  bool last_pad_xy_valid = false;  ///< True after at least one display-1 motion.
  std::uint32_t remembered_appid = 0;  ///< Last non-Steam shortcut id for overlay hide.
  XErrorHandler previous_x11_error = nullptr;  ///< Previous Xlib error handler.

  /**
   * @brief Ignore stale window ids from a cached GamePad or Steam xid.
   *
   * @param dpy X11 display.
   * @param error Error event.
   * @return 0 so Xlib continues.
   */
  int ignore_stale_window(Display *dpy, XErrorEvent *error) {
    if (error && (error->error_code == BadWindow || error->error_code == BadDrawable)) {
      return 0;
    }
    if (previous_x11_error) {
      return previous_x11_error(dpy, error);
    }
    return 0;
  }

  /**
   * @brief Return a cached X11 display, opening it on first use.
   *
   * @return Display pointer, or `nullptr` when `$DISPLAY` cannot be opened.
   */
  Display *x11_display() {
    if (cached_display) {
      return cached_display;
    }
    XInitThreads();
    // Do not use `$DISPLAY`. kms unsets it; a leftover `:2` is headless gamescope.
    cached_display = XOpenDisplay(platf::gamescope::session_x11_name());
    if (!cached_display) {
      BOOST_LOG(warning) << "gamescope session: XOpenDisplay "sv << platf::gamescope::session_x11_name() << " failed"sv;
      return nullptr;
    }
    BOOST_LOG(info) << "gamescope session: XOpenDisplay "sv << DisplayString(cached_display);
    previous_x11_error = XSetErrorHandler(ignore_stale_window);
    return cached_display;
  }

  /**
   * @brief Read `_NET_WM_NAME` or `WM_NAME` for a window.
   *
   * @param dpy X11 display.
   * @param window Window to inspect.
   * @return UTF-8 title, or empty when unset.
   */
  std::string window_title(Display *dpy, Window window) {
    const auto net_wm = XInternAtom(dpy, "_NET_WM_NAME", True);
    const auto utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    if (net_wm && utf8) {
      Atom actual = None;
      int format = 0;
      unsigned long nitems = 0;
      unsigned long after = 0;
      unsigned char *prop = nullptr;
      if (XGetWindowProperty(dpy, window, net_wm, 0, 1024, False, utf8, &actual, &format, &nitems, &after, &prop) == Success && prop && nitems > 0) {
        std::string title(reinterpret_cast<char *>(prop), nitems);
        XFree(prop);
        return title;
      }
      if (prop) {
        XFree(prop);
      }
    }

    char *name = nullptr;
    if (XFetchName(dpy, window, &name) && name) {
      std::string title(name);
      XFree(name);
      return title;
    }
    return {};
  }

  /**
   * @brief Walk the window tree from a parent.
   *
   * @param dpy X11 display.
   * @param parent Window whose children are visited.
   * @param out Accumulator for every descendant (not including `parent`).
   */
  void collect_windows(Display *dpy, Window parent, std::vector<Window> &out) {
    Window root_return = 0;
    Window parent_return = 0;
    Window *children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, parent, &root_return, &parent_return, &children, &nchildren) || !children) {
      return;
    }
    for (unsigned int i = 0; i < nchildren; ++i) {
      out.push_back(children[i]);
      collect_windows(dpy, children[i], out);
    }
    XFree(children);
  }

  /**
   * @brief Find the first window whose title matches a predicate.
   *
   * @tparam Predicate Callable `bool(std::string_view)`.
   * @param dpy X11 display.
   * @param predicate Title matcher.
   * @return Window id, or `None`.
   */
  template<typename Predicate>
  Window find_window(Display *dpy, Predicate predicate) {
    std::vector<Window> windows;
    collect_windows(dpy, DefaultRootWindow(dpy), windows);
    for (const auto window : windows) {
      const auto title = window_title(dpy, window);
      if (!title.empty() && predicate(title)) {
        return window;
      }
    }
    return None;
  }

  /**
   * @brief Read a 32-bit CARDINAL property.
   *
   * @param dpy X11 display.
   * @param window Window or root.
   * @param name Atom name.
   * @return Value when present.
   */
  std::optional<std::uint32_t> get_cardinal(Display *dpy, Window window, const char *name) {
    const auto atom = XInternAtom(dpy, name, True);
    if (!atom) {
      return std::nullopt;
    }
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, window, atom, 0, 1, False, XA_CARDINAL, &actual, &format, &nitems, &after, &prop) != Success || !prop || nitems < 1) {
      if (prop) {
        XFree(prop);
      }
      return std::nullopt;
    }
    const auto value = static_cast<std::uint32_t>(*reinterpret_cast<unsigned long *>(prop));
    XFree(prop);
    return value;
  }

  /**
   * @brief Replace a 32-bit CARDINAL property.
   *
   * @param dpy X11 display.
   * @param window Window or root.
   * @param name Atom name (created if missing).
   * @param value Cardinal value.
   */
  void set_cardinal(Display *dpy, Window window, const char *name, std::uint32_t value) {
    const auto atom = XInternAtom(dpy, name, False);
    unsigned long stored = value;
    XChangeProperty(dpy, window, atom, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&stored), 1);
  }

  /**
   * @brief True when Steam Big Picture has `STEAM_OVERLAY=1`.
   *
   * @param dpy X11 display.
   * @param bpm Steam Big Picture window.
   * @return True when the overlay split is active.
   */
  bool overlay_is_on(Display *dpy, Window bpm) {
    const auto value = get_cardinal(dpy, bpm, "STEAM_OVERLAY");
    return value && *value == 1;
  }

  /**
   * @brief Find the Steam window that owns `STEAM_OVERLAY`.
   *
   * Prefers a `Steam Big Picture Mode` title. Game Mode sometimes leaves only
   * a `steamwebhelper` with `STEAM_GAME=769` (no BPM caption) — overlay toggle
   * is a silent no-op without that fallback. Prefer an already-on overlay atom,
   * then a BPM-sized surface, then the largest 769 window.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window find_steam_overlay_window(Display *dpy) {
    const auto titled = find_window(dpy, platf::gamescope::title_is_steam_big_picture);
    if (titled != None) {
      return titled;
    }

    std::vector<Window> windows;
    collect_windows(dpy, DefaultRootWindow(dpy), windows);
    Window overlay_on = None;
    Window sized = None;
    int sized_area = -1;
    Window any_client = None;
    int any_area = -1;
    for (const auto window : windows) {
      if (overlay_is_on(dpy, window)) {
        overlay_on = window;
      }
      const auto steam_game = get_cardinal(dpy, window, "STEAM_GAME");
      if (!steam_game || *steam_game != platf::gamescope::STEAM_CLIENT_APPID) {
        continue;
      }
      XWindowAttributes attr {};
      if (!XGetWindowAttributes(dpy, window, &attr) || attr.width <= 0 || attr.height <= 0) {
        continue;
      }
      const int area = attr.width * attr.height;
      if (area > any_area) {
        any_client = window;
        any_area = area;
      }
      if (attr.width >= 1280 && attr.height >= 720 && area > sized_area) {
        sized = window;
        sized_area = area;
      }
    }
    if (overlay_on != None) {
      return overlay_on;
    }
    if (sized != None) {
      return sized;
    }
    return any_client;
  }

  /**
   * @brief Remember the running shortcut id for overlay hide.
   *
   * @param dpy X11 display.
   * @param tv HDMI game window (Cemu TV or Azahar Primary), or `None`.
   */
  void remember_appid(Display *dpy, Window tv) {
    std::optional<std::uint32_t> steam_game;
    if (tv != None) {
      steam_game = get_cardinal(dpy, tv, "STEAM_GAME");
    }
    const auto root = DefaultRootWindow(dpy);
    const auto resolved = platf::gamescope::resolve_overlay_appid(
      steam_game,
      get_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP"),
      get_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP_GFX")
    );
    if (resolved != 0) {
      remembered_appid = resolved;
    }
  }

  /**
   * @brief Open the Steam Game Mode overlay split.
   *
   * @param dpy X11 display.
   * @param bpm Steam Big Picture window.
   * @param appid Shortcut id kept on `GAMESCOPE_FOCUSED_APP_GFX`.
   */
  void show_overlay(Display *dpy, Window bpm, std::uint32_t appid) {
    const auto root = DefaultRootWindow(dpy);
    set_cardinal(dpy, bpm, "STEAM_OVERLAY", 1);
    set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP", platf::gamescope::STEAM_CLIENT_APPID);
    if (appid != 0) {
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP_GFX", appid);
    }
    XFlush(dpy);
    BOOST_LOG(info) << "gamescope overlay on bpm="sv << bpm << " gfx="sv << appid;
  }

  /**
   * @brief Close the overlay and restore the HDMI game surface as the baselayer.
   *
   * @param dpy X11 display.
   * @param bpm Steam overlay window, or `None`.
   * @param tv HDMI game window (Cemu TV or Azahar Primary), or `None`.
   * @param appid Shortcut id to restore.
   */
  void hide_overlay(Display *dpy, Window bpm, Window tv, std::uint32_t appid) {
    const auto root = DefaultRootWindow(dpy);
    if (bpm != None) {
      set_cardinal(dpy, bpm, "STEAM_OVERLAY", 0);
    }
    if (tv != None && appid != 0) {
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_WINDOW", static_cast<std::uint32_t>(tv));
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP", appid);
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP_GFX", appid);
      set_cardinal(dpy, root, "GAMESCOPECTRL_BASELAYER_WINDOW", static_cast<std::uint32_t>(tv));
    } else if (appid != 0) {
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP", appid);
      set_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP_GFX", appid);
    }
    XFlush(dpy);
    BOOST_LOG(info) << "gamescope overlay off tv="sv << tv << " app="sv << appid;
  }

  /**
   * @brief Return the GamePad View or Azahar Secondary Window, refreshing a stale cache.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window gamepad_view_window(Display *dpy) {
    if (cached_pad != None) {
      XWindowAttributes attr {};
      if (XGetWindowAttributes(dpy, cached_pad, &attr) && attr.width > 0 && attr.height > 0) {
        const auto title = window_title(dpy, cached_pad);
        if (platf::gamescope::title_is_touch_surface(title)) {
          return cached_pad;
        }
      }
      cached_pad = None;
    }
    cached_pad = find_window(dpy, platf::gamescope::title_is_touch_surface);
    return cached_pad;
  }

  /**
   * @brief Translate window-local coordinates to root coordinates.
   *
   * @param dpy X11 display.
   * @param window Source window.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @return Root coordinates.
   */
  std::pair<int, int> root_xy(Display *dpy, Window window, int x, int y) {
    int rx = x;
    int ry = y;
    Window child = None;
    XTranslateCoordinates(dpy, window, DefaultRootWindow(dpy), x, y, &rx, &ry, &child);
    return {rx, ry};
  }

  /**
   * @brief Prefer the mapped GL child of GamePad View over the wx frame.
   *
   * Cemu paints on a full-size child (`1920x1080+0+0`). `XSendEvent` with
   * `propagate=True` walks *ancestors*, not children, so a frame that has not
   * selected `ButtonPressMask` never delivers the click to the canvas.
   *
   * @param dpy X11 display.
   * @param pad GamePad View frame.
   * @return Input target window.
   */
  Window gamepad_input_window(Display *dpy, Window pad) {
    XWindowAttributes pad_attr {};
    if (!XGetWindowAttributes(dpy, pad, &pad_attr) || pad_attr.width <= 0 || pad_attr.height <= 0) {
      return pad;
    }

    Window root_return = 0;
    Window parent_return = 0;
    Window *children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, pad, &root_return, &parent_return, &children, &nchildren) || !children) {
      return pad;
    }

    Window best = pad;
    int best_area = 0;
    const int min_w = pad_attr.width * 9 / 10;
    const int min_h = pad_attr.height * 9 / 10;
    for (unsigned int i = 0; i < nchildren; ++i) {
      XWindowAttributes attr {};
      if (!XGetWindowAttributes(dpy, children[i], &attr) || attr.map_state != IsViewable) {
        continue;
      }
      if (attr.width < min_w || attr.height < min_h) {
        continue;
      }
      const int area = attr.width * attr.height;
      if (area > best_area) {
        best = children[i];
        best_area = area;
      }
    }
    XFree(children);
    return best;
  }

  /**
   * @brief Log the first few GamePad pointer events with mapped pixels.
   *
   * @param kind Short label (`touch`, `abs`, `button`).
   * @param nx Unit X before clamp.
   * @param ny Unit Y before clamp.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @param window Target xid.
   * @param width Target width.
   * @param height Target height.
   */
  void log_gamepad_pointer(std::string_view kind, float nx, float ny, int x, int y, Window window, int width, int height) {
    static int remaining = 12;
    if (remaining <= 0) {
      return;
    }
    --remaining;
    BOOST_LOG(info) << "GamePad "sv << kind << " unit="sv << nx << ',' << ny
                    << " px="sv << x << ',' << y
                    << " xid="sv << window << ' ' << width << 'x' << height;
  }

  /**
   * @brief Send a pointer event to a window without raising or focusing it.
   *
   * Host uinput is hit-tested by gamescope onto the top surface (Cemu TV).
   * `XSendEvent` with mask `0` is delivered to the client that created the
   * destination, so GamePad View still receives the click while stacked
   * under the TV. `XWarpPointer` keeps `XQueryPointer` in sync — wx/GTK
   * often ignore `send_event=True` and read the real cursor, which sits at
   * screen center unless we warp.
   *
   * @param dpy X11 display.
   * @param window Target window.
   * @param type `ButtonPress`, `ButtonRelease`, or `MotionNotify`.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @param state Modifier / button mask.
   * @param button Button number (`1` for left), or `0` for motion.
   */
  void send_pointer(Display *dpy, Window window, int type, int x, int y, unsigned int state, unsigned int button) {
    const auto [rx, ry] = root_xy(dpy, window, x, y);
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, rx, ry);

    XEvent event {};
    if (type == MotionNotify) {
      event.xmotion.type = MotionNotify;
      event.xmotion.serial = 0;
      event.xmotion.send_event = True;
      event.xmotion.display = dpy;
      event.xmotion.window = window;
      event.xmotion.root = DefaultRootWindow(dpy);
      event.xmotion.subwindow = None;
      event.xmotion.time = CurrentTime;
      event.xmotion.x = x;
      event.xmotion.y = y;
      event.xmotion.x_root = rx;
      event.xmotion.y_root = ry;
      event.xmotion.state = state;
      event.xmotion.is_hint = NotifyNormal;
      event.xmotion.same_screen = True;
    } else {
      event.xbutton.type = type;
      event.xbutton.serial = 0;
      event.xbutton.send_event = True;
      event.xbutton.display = dpy;
      event.xbutton.window = window;
      event.xbutton.root = DefaultRootWindow(dpy);
      event.xbutton.subwindow = None;
      event.xbutton.time = CurrentTime;
      event.xbutton.x = x;
      event.xbutton.y = y;
      event.xbutton.x_root = rx;
      event.xbutton.y_root = ry;
      event.xbutton.state = state;
      event.xbutton.button = button;
      event.xbutton.same_screen = True;
    }

    // Mask 0 + propagate False: deliver to the creating client of `window`.
    // A non-zero mask only reaches clients that selected it; the wx frame
    // often has not, and propagate walks parents rather than the GL child.
    XSendEvent(dpy, window, False, 0, &event);
  }

  /**
   * @brief Resolve GamePad View, map unit coords, and send a pointer event.
   *
   * @param dpy X11 display.
   * @param nx Unit X in `[0, 1]`.
   * @param ny Unit Y in `[0, 1]`.
   * @param kind Log label.
   * @param type `ButtonPress`, `ButtonRelease`, or `MotionNotify`.
   * @param state Modifier / button mask.
   * @param button Button number, or `0` for motion.
   * @return True when GamePad View exists and the event was flushed.
   */
  bool inject_unit(Display *dpy, float nx, float ny, std::string_view kind, int type, unsigned int state, unsigned int button) {
    const auto pad = gamepad_view_window(dpy);
    if (pad == None) {
      static bool logged_missing = false;
      if (!logged_missing) {
        BOOST_LOG(warning) << "GamePad inject: no GamePad View / Azahar Secondary Window on "sv << platf::gamescope::session_x11_name();
        logged_missing = true;
      }
      return false;
    }
    const auto target = gamepad_input_window(dpy, pad);

    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, target, &attr) || attr.width <= 0 || attr.height <= 0) {
      return false;
    }

    const auto [x, y] = platf::gamescope::touch_to_window_xy(nx, ny, attr.width, attr.height);
    last_pad_x = x;
    last_pad_y = y;
    last_pad_xy_valid = true;
    log_gamepad_pointer(kind, nx, ny, x, y, target, attr.width, attr.height);
    send_pointer(dpy, target, type, x, y, state, button);
    XFlush(dpy);
    return true;
  }

  /**
   * @brief Moonlight mouse button to X11 button number.
   *
   * @param button Moonlight `BUTTON_*` (`1` left, `2` middle, `3` right).
   * @return X11 button, or `0` when unsupported.
   */
  unsigned int x11_button(int button) {
    if (button >= 1 && button <= 5) {
      return static_cast<unsigned int>(button);
    }
    return 0;
  }

}  // namespace
#endif

namespace platf {

  void gamescope_on_guide_press() {
#ifdef SUNSHINE_BUILD_X11
    std::scoped_lock lock {x11_lock};
    auto *dpy = x11_display();
    if (!dpy) {
      return;
    }

    const auto bpm = find_steam_overlay_window(dpy);
    if (bpm == None) {
      BOOST_LOG(warning) << "gamescope overlay: no Steam Big Picture / STEAM_GAME=769 window"sv;
      return;
    }
    const auto tv = find_window(dpy, platf::gamescope::title_is_hdmi_surface);
    remember_appid(dpy, tv);
    const auto appid = remembered_appid;
    if (platf::gamescope::overlay_toggle_action(overlay_is_on(dpy, bpm)) == platf::gamescope::overlay_action_e::hide) {
      hide_overlay(dpy, bpm, tv, appid);
    } else {
      show_overlay(dpy, bpm, appid);
    }
#endif
  }

  bool inject_gamepad_view_touch(const touch_port_t &touch_port, const touch_input_t &touch) {
    (void) touch_port;
#ifdef SUNSHINE_BUILD_X11
    if (!platf::gamescope::touch_is_second_display(touch.pointerId)) {
      return false;
    }

    std::scoped_lock lock {x11_lock};
    auto *dpy = x11_display();
    if (!dpy) {
      return false;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL_ALL:
        if (pad_pointer_down) {
          const auto ok = inject_unit(dpy, touch.x, touch.y, "touch-cancel"sv, ButtonRelease, Button1Mask, Button1);
          pad_pointer_down = false;
          return ok;
        }
        return true;
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        pad_pointer_down = false;
        return inject_unit(dpy, touch.x, touch.y, "touch-up"sv, ButtonRelease, Button1Mask, Button1);
      case LI_TOUCH_EVENT_DOWN:
        if (!inject_unit(dpy, touch.x, touch.y, "touch-move"sv, MotionNotify, 0, 0)) {
          return false;
        }
        pad_pointer_down = true;
        return inject_unit(dpy, touch.x, touch.y, "touch-down"sv, ButtonPress, 0, Button1);
      case LI_TOUCH_EVENT_MOVE:
        return inject_unit(dpy, touch.x, touch.y, "touch-drag"sv, MotionNotify, pad_pointer_down ? Button1Mask : 0, 0);
      case LI_TOUCH_EVENT_HOVER:
        return inject_unit(dpy, touch.x, touch.y, "touch-hover"sv, MotionNotify, 0, 0);
      default:
        return false;
    }
#else
    (void) touch;
    return false;
#endif
  }

  bool inject_gamepad_view_abs_mouse(const touch_port_t &touch_port, float x, float y) {
#ifdef SUNSHINE_BUILD_X11
    std::scoped_lock lock {x11_lock};
    auto *dpy = x11_display();
    if (!dpy) {
      return false;
    }
    const auto [nx, ny] = platf::gamescope::abs_to_unit(
      x,
      y,
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port.width,
      touch_port.height
    );
    return inject_unit(dpy, nx, ny, "abs"sv, MotionNotify, pad_pointer_down ? Button1Mask : 0, 0);
#else
    (void) touch_port;
    (void) x;
    (void) y;
    return false;
#endif
  }

  bool inject_gamepad_view_button(int button, bool release) {
#ifdef SUNSHINE_BUILD_X11
    const auto x_button = x11_button(button);
    if (x_button == 0) {
      return false;
    }

    std::scoped_lock lock {x11_lock};
    auto *dpy = x11_display();
    if (!dpy || !last_pad_xy_valid) {
      return false;
    }

    const auto pad = gamepad_view_window(dpy);
    if (pad == None) {
      return false;
    }
    const auto target = gamepad_input_window(dpy, pad);
    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, target, &attr) || attr.width <= 0 || attr.height <= 0) {
      return false;
    }

    const auto nx = attr.width > 1 ? static_cast<float>(last_pad_x) / static_cast<float>(attr.width - 1) : 0.0F;
    const auto ny = attr.height > 1 ? static_cast<float>(last_pad_y) / static_cast<float>(attr.height - 1) : 0.0F;
    log_gamepad_pointer(release ? "button-up"sv : "button-down"sv, nx, ny, last_pad_x, last_pad_y, target, attr.width, attr.height);
    const auto type = release ? ButtonRelease : ButtonPress;
    const unsigned int state = release ? Button1Mask : 0;
    send_pointer(dpy, target, type, last_pad_x, last_pad_y, state, x_button);
    pad_pointer_down = !release && x_button == Button1;
    XFlush(dpy);
    return true;
#else
    (void) button;
    (void) release;
    return false;
#endif
  }

}  // namespace platf
