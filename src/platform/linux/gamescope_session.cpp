/**
 * @file src/platform/linux/gamescope_session.cpp
 * @brief Gamescope overlay toggle and Cemu GamePad View touch injection.
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

  std::pair<int, int> touch_to_window_xy(float x, float y, int width, int height) {
    if (width <= 0 || height <= 0) {
      return {0, 0};
    }
    const auto px = std::clamp(static_cast<int>(std::lround(std::clamp(x, 0.0F, 1.0F) * static_cast<float>(width - 1))), 0, width - 1);
    const auto py = std::clamp(static_cast<int>(std::lround(std::clamp(y, 0.0F, 1.0F) * static_cast<float>(height - 1))), 0, height - 1);
    return {px, py};
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

}  // namespace platf::gamescope

#ifdef SUNSHINE_BUILD_X11
namespace {

  std::mutex x11_lock;  ///< Serializes Xlib calls on the cached display.
  Display *cached_display = nullptr;  ///< Reused X11 connection for `:0` gamescope.
  Window cached_pad = None;  ///< Last Cemu GamePad View xid.
  bool pad_pointer_down = false;  ///< True while display-1 contact is held on GamePad View.
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
    cached_display = XOpenDisplay(nullptr);
    if (!cached_display) {
      BOOST_LOG(debug) << "gamescope session: XOpenDisplay failed"sv;
      return nullptr;
    }
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
   * @brief Remember the running shortcut id for overlay hide.
   *
   * @param dpy X11 display.
   * @param tv Cemu TV window, or `None`.
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
   * @brief Close the overlay and restore Cemu TV as the HDMI baselayer.
   *
   * @param dpy X11 display.
   * @param bpm Steam Big Picture window, or `None`.
   * @param tv Cemu TV window, or `None`.
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
   * @brief Return the Cemu GamePad View window, refreshing a stale cache.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window gamepad_view_window(Display *dpy) {
    if (cached_pad != None) {
      XWindowAttributes attr {};
      if (XGetWindowAttributes(dpy, cached_pad, &attr) && attr.width > 0 && attr.height > 0) {
        return cached_pad;
      }
      cached_pad = None;
    }
    cached_pad = find_window(dpy, platf::gamescope::title_is_gamepad_view);
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
   * @brief Send a pointer event to a window without raising or focusing it.
   *
   * Host uinput is hit-tested by gamescope onto the top surface (Cemu TV).
   * XSendEvent goes to the X11 client directly, so GamePad View still receives
   * the click while it stays stacked under the TV.
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
    XEvent event {};
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

    long mask = PointerMotionMask;
    if (type == ButtonPress) {
      mask = ButtonPressMask;
    } else if (type == ButtonRelease) {
      mask = ButtonReleaseMask;
    } else if (state & Button1Mask) {
      mask = ButtonMotionMask;
    }
    XSendEvent(dpy, window, True, mask, &event);
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

    const auto bpm = find_window(dpy, platf::gamescope::title_is_steam_big_picture);
    if (bpm == None) {
      BOOST_LOG(debug) << "gamescope overlay: no Steam Big Picture window"sv;
      return;
    }
    const auto tv = find_window(dpy, platf::gamescope::title_is_cemu_tv);
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

    const auto pad = gamepad_view_window(dpy);
    if (pad == None) {
      return false;
    }

    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, pad, &attr) || attr.width <= 0 || attr.height <= 0) {
      return false;
    }

    const auto [x, y] = platf::gamescope::touch_to_window_xy(touch.x, touch.y, attr.width, attr.height);

    static bool logged_inject = false;
    if (!logged_inject) {
      BOOST_LOG(info) << "display-1 touch → GamePad View xid="sv << pad << ' ' << attr.width << 'x' << attr.height;
      logged_inject = true;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL_ALL:
        if (pad_pointer_down) {
          send_pointer(dpy, pad, ButtonRelease, x, y, Button1Mask, Button1);
          pad_pointer_down = false;
          XFlush(dpy);
        }
        return true;
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        send_pointer(dpy, pad, ButtonRelease, x, y, Button1Mask, Button1);
        pad_pointer_down = false;
        XFlush(dpy);
        return true;
      case LI_TOUCH_EVENT_DOWN:
        send_pointer(dpy, pad, MotionNotify, x, y, 0, 0);
        send_pointer(dpy, pad, ButtonPress, x, y, 0, Button1);
        pad_pointer_down = true;
        XFlush(dpy);
        return true;
      case LI_TOUCH_EVENT_MOVE:
        send_pointer(dpy, pad, MotionNotify, x, y, pad_pointer_down ? Button1Mask : 0, 0);
        XFlush(dpy);
        return true;
      case LI_TOUCH_EVENT_HOVER:
        send_pointer(dpy, pad, MotionNotify, x, y, 0, 0);
        XFlush(dpy);
        return true;
      default:
        return false;
    }
#else
    (void) touch;
    return false;
#endif
  }

}  // namespace platf
