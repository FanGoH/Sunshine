/**
 * @file src/platform/linux/gamescope_session.cpp
 * @brief Gamescope overlay toggle and GamePad / Azahar touch injection.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef SUNSHINE_BUILD_X11
  // platform includes
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/extensions/XTest.h>
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

  bool title_is_azahar_window(std::string_view title) {
    return title.find("Azahar ") != std::string_view::npos;
  }

  bool title_is_azahar_stacked(std::string_view title) {
    if (!title_is_azahar_window(title)) {
      return false;
    }
    if (title.find("Primary Window") != std::string_view::npos) {
      return false;
    }
    if (title.find("Secondary Window") != std::string_view::npos) {
      return false;
    }
    return true;
  }

  bool title_is_eden(std::string_view title) {
    // Live Game Mode titles: "Eden | v0.2.1 | Clang …" and the same with a
    // game suffix. Do not match the 1×1 "eden" helper or Qt selection owner.
    return title.size() >= 6 && title.substr(0, 6) == "Eden |";
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
    if (title_is_eden(title)) {
      return true;
    }
    if (title.find("Primary Window") != std::string_view::npos) {
      return true;
    }
    // RetroDECK / HDMI-only Azahar is one stacked window (top+bottom). Taps
    // on display 0 must hit that surface; Separate Windows still uses Primary.
    return title_is_azahar_stacked(title);
  }

  bool hdmi_surface_uses_absolute_clicks(std::string_view title) {
    // Eden is still the HDMI BASELAYER restore target, but it is a small Qt
    // window on a 4K nested :1. Mapping the full Thor top stream onto
    // 1024×576 is wonky — those taps become a relative trackpad instead.
    if (title_is_eden(title)) {
      return false;
    }
    return title_is_hdmi_surface(title);
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

  std::pair<float, float> packet_to_unit(float x, float y, float width, float height) {
    if (width <= 0.0F || height <= 0.0F) {
      return {0.0F, 0.0F};
    }
    // Thor Stretch fills the 1240×1080 panel with the 1920×1080 stream
    // (non-uniform). The old client sends that view as ref=1239x1079.
    // Undo-letterbox assumed Fit bars and sheared Y (packet y=190 is 18%
    // down the stretched image, not the 16:9 content top). Linear is
    // correct for Stretch, for Fit (the StreamView is already 16:9), and
    // for a stream-sized ref. Do not run client_to_touchport.
    return {x / width, y / height};
  }

  focus_display_t focus_display_with_middle(focus_display_t current, std::uint32_t middle) {
    current.nested = middle;
    return current;
  }

  const char *session_x11_name() {
    return ":0";
  }

  std::size_t session_x11_touch_count() {
    return 2;
  }

  const char *session_x11_touch_name(std::size_t index) {
    // Prefer :1 — cemu-gamescope-focus.sh sets FOCUS_DISPLAY=1. :0 is Steam.
    // Never :2 (headless video/1). Never $DISPLAY (kms unsets it).
    switch (index) {
      case 0:
        return ":1";
      case 1:
        return ":0";
      default:
        return nullptr;
    }
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

  std::optional<second_screen_touch_t> parse_second_screen_touch(std::string_view text) {
    second_screen_touch_t out;
    bool have_xid = false;
    std::istringstream in {std::string {text}};
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const auto key = line.substr(0, eq);
      auto val = line.substr(eq + 1);
      while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) {
        val.pop_back();
      }
      if (key == "display") {
        out.display = val;
      } else if (key == "xid" && !val.empty()) {
        char *end = nullptr;
        const int base = (val.size() > 1 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) ? 16 : 0;
        out.xid = std::strtoul(val.c_str(), &end, base);
        have_xid = end != val.c_str() && out.xid != 0;
      }
    }
    if (!have_xid) {
      return std::nullopt;
    }
    if (out.display.empty()) {
      out.display = ":0";
    }
    if (out.display == ":2" || out.display.rfind(":2.", 0) == 0) {
      return std::nullopt;
    }
    return out;
  }

  bool abs_targets_gamepad_view(std::size_t display_index, bool primary_from_secondary) {
    if (display_index == 1) {
      return true;
    }
    return display_index == 0 && primary_from_secondary;
  }

  bool abs_targets_hdmi_surface(std::size_t display_index, bool primary_from_secondary) {
    return display_index == 0 && !primary_from_secondary;
  }

  bool abs_targets_steam_overlay(std::size_t display_index, bool primary_from_secondary, bool overlay_is_on) {
    return overlay_is_on && abs_targets_hdmi_surface(display_index, primary_from_secondary);
  }

}  // namespace platf::gamescope

#ifdef SUNSHINE_BUILD_X11
namespace {

  std::mutex x11_lock;  ///< Serializes Xlib calls on the cached displays.
  Display *cached_overlay = nullptr;  ///< Reused X11 connection for Steam `:0`.
  Display *cached_touch[2] = {nullptr, nullptr};  ///< Reused connections for `:1` then `:0`.
  Display *cached_pad_dpy = nullptr;  ///< Display that owns `cached_pad`.
  Window cached_pad = None;  ///< Last GamePad View / Azahar Secondary xid.
  Display *cached_hdmi_dpy = nullptr;  ///< Display that owns `cached_hdmi`.
  Window cached_hdmi = None;  ///< Last Cemu TV / Azahar / Eden HDMI-surface xid.
  int last_hdmi_x = 0;  ///< Last TV-local pointer X (for mouse-button packets).
  int last_hdmi_y = 0;  ///< Last TV-local pointer Y (for mouse-button packets).
  bool last_hdmi_xy_valid = false;  ///< True after at least one display-0 motion.
  bool pad_pointer_down = false;  ///< True while display-1 contact is held on GamePad View.
  bool hdmi_pointer_down = false;  ///< True while display-0 contact is held on Cemu TV.
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
   * @brief Open one session Xwayland and cache it.
   *
   * @param name `:0` or `:1`. Never `$DISPLAY` or `:2`.
   * @return Display pointer, or `nullptr` when the server is down.
   */
  Display *x11_open(const char *name) {
    if (!name) {
      return nullptr;
    }
    if (std::strcmp(name, platf::gamescope::session_x11_name()) == 0 && cached_overlay) {
      return cached_overlay;
    }
    const auto count = platf::gamescope::session_x11_touch_count();
    for (std::size_t i = 0; i < count; ++i) {
      if (platf::gamescope::session_x11_touch_name(i) && std::strcmp(name, platf::gamescope::session_x11_touch_name(i)) == 0 && cached_touch[i]) {
        return cached_touch[i];
      }
    }

    XInitThreads();
    auto *dpy = XOpenDisplay(name);
    if (!dpy) {
      BOOST_LOG(warning) << "gamescope session: XOpenDisplay "sv << name << " failed"sv;
      return nullptr;
    }
    BOOST_LOG(info) << "gamescope session: XOpenDisplay "sv << DisplayString(dpy);
    if (!previous_x11_error) {
      previous_x11_error = XSetErrorHandler(ignore_stale_window);
    }
    if (std::strcmp(name, platf::gamescope::session_x11_name()) == 0) {
      cached_overlay = dpy;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (platf::gamescope::session_x11_touch_name(i) && std::strcmp(name, platf::gamescope::session_x11_touch_name(i)) == 0) {
        cached_touch[i] = dpy;
      }
    }
    return dpy;
  }

  /**
   * @brief Steam / overlay X11 connection (`:0`).
   *
   * @return Display pointer, or `nullptr`.
   */
  Display *x11_display() {
    return x11_open(platf::gamescope::session_x11_name());
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
   * @brief Read a CARDINAL list (FOCUS_DISPLAY is three values).
   *
   * @param dpy X11 display.
   * @param window Window or root.
   * @param name Atom name.
   * @param max_items Maximum values to return.
   * @return Values when present.
   */
  std::optional<std::vector<std::uint32_t>> get_cardinals(Display *dpy, Window window, const char *name, unsigned long max_items) {
    const auto atom = XInternAtom(dpy, name, True);
    if (!atom || max_items == 0) {
      return std::nullopt;
    }
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, window, atom, 0, static_cast<long>(max_items), False, XA_CARDINAL, &actual, &format, &nitems, &after, &prop) != Success || !prop || nitems < 1) {
      if (prop) {
        XFree(prop);
      }
      return std::nullopt;
    }
    std::vector<std::uint32_t> values;
    values.reserve(nitems);
    const auto *longs = reinterpret_cast<unsigned long *>(prop);
    for (unsigned long i = 0; i < nitems; ++i) {
      values.push_back(static_cast<std::uint32_t>(longs[i]));
    }
    XFree(prop);
    return values;
  }

  /**
   * @brief Replace a CARDINAL list.
   *
   * @param dpy X11 display.
   * @param window Window or root.
   * @param name Atom name.
   * @param values Cardinals to store.
   */
  void set_cardinals(Display *dpy, Window window, const char *name, const std::vector<std::uint32_t> &values) {
    const auto atom = XInternAtom(dpy, name, False);
    std::vector<unsigned long> stored(values.begin(), values.end());
    XChangeProperty(dpy, window, atom, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char *>(stored.data()), static_cast<int>(stored.size()));
  }

  bool overlay_is_on(Display *dpy, Window bpm);
  Window find_steam_overlay_window(Display *dpy);

  /**
   * @brief Route gamescope mouse/touch to nested `:1` while Cemu is playing.
   *
   * Steam / a kms restart can leave `GAMESCOPE_FOCUS_DISPLAY` at middle `0`.
   * Cemu and GamePad View live on `:1`; XSendEvent then looks like dead taps.
   * Keep the live first/third. Do not reclaim overlay, QAM, or Steam menus.
   */
  void ensure_nested_mouse_focus() {
    auto *dpy = x11_display();
    if (!dpy) {
      return;
    }
    const auto root = DefaultRootWindow(dpy);
    platf::gamescope::focus_display_t current;
    if (const auto values = get_cardinals(dpy, root, "GAMESCOPE_FOCUS_DISPLAY", 3); values && values->size() >= 3) {
      current.server = (*values)[0];
      current.nested = (*values)[1];
      current.token = (*values)[2];
    }
    const auto mouse = get_cardinals(dpy, root, "GAMESCOPE_MOUSE_FOCUS_DISPLAY", 3);
    if (current.nested == 1 && mouse && mouse->size() >= 3 && (*mouse)[1] == 1) {
      return;
    }
    const auto app = get_cardinal(dpy, root, "GAMESCOPE_FOCUSED_APP");
    if (app && *app == platf::gamescope::STEAM_CLIENT_APPID) {
      return;
    }
    const auto blur = get_cardinal(dpy, root, "GAMESCOPE_BLUR_MODE");
    if (blur && *blur != 0) {
      return;
    }
    const auto bpm = find_steam_overlay_window(dpy);
    if (bpm != None && overlay_is_on(dpy, bpm)) {
      return;
    }

    const auto want = platf::gamescope::focus_display_with_middle(current, 1);
    const std::vector<std::uint32_t> tuple {want.server, want.nested, want.token};
    set_cardinals(dpy, root, "GAMESCOPE_FOCUS_DISPLAY", tuple);
    set_cardinals(dpy, root, "GAMESCOPE_KEYBOARD_FOCUS_DISPLAY", tuple);
    set_cardinals(dpy, root, "GAMESCOPE_MOUSE_FOCUS_DISPLAY", tuple);
    // GamePad inject talks to :1. Without this flush the :0 write sits in
    // the Xlib buffer and Steam keeps middle 0.
    XFlush(dpy);
    static bool logged = false;
    if (!logged) {
      logged = true;
      BOOST_LOG(info) << "GamePad inject: FOCUS_DISPLAY middle 1 ("sv
                      << want.server << ',' << want.nested << ',' << want.token << ')';
    }
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
   * is a silent no-op without that fallback. Prefer an already-on overlay atom
   * on a **mapped** surface, then a BPM-sized mapped surface. Unmapped 200×200
   * steamwebhelper helpers do not composite; tagging those looks like overlay
   * on in the log with nothing on screen.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window find_steam_overlay_window(Display *dpy) {
    const auto titled = find_window(dpy, platf::gamescope::title_is_steam_big_picture);
    if (titled != None) {
      XWindowAttributes titled_attr {};
      if (XGetWindowAttributes(dpy, titled, &titled_attr) && titled_attr.map_state == IsViewable) {
        return titled;
      }
    }

    std::vector<Window> windows;
    collect_windows(dpy, DefaultRootWindow(dpy), windows);
    Window overlay_on = None;
    Window sized = None;
    int sized_area = -1;
    for (const auto window : windows) {
      XWindowAttributes attr {};
      if (!XGetWindowAttributes(dpy, window, &attr) || attr.map_state != IsViewable || attr.width <= 0 || attr.height <= 0) {
        continue;
      }
      if (overlay_is_on(dpy, window)) {
        overlay_on = window;
      }
      const auto steam_game = get_cardinal(dpy, window, "STEAM_GAME");
      if (!steam_game || *steam_game != platf::gamescope::STEAM_CLIENT_APPID) {
        continue;
      }
      const int area = attr.width * attr.height;
      if (attr.width >= 1280 && attr.height >= 720 && area > sized_area) {
        sized = window;
        sized_area = area;
      }
    }
    if (overlay_on != None) {
      return overlay_on;
    }
    return sized;
  }

  /**
   * @brief Ask the running Steam client to toggle Game Mode overlay.
   *
   * GDS `STEAM_OVERLAY` only composites when Steam Big Picture is mapped.
   * Standalone Flatpak games often leave only an unmapped steamwebhelper.
   * `steam://overlay/toggle` is Steam's own IPC (same as `steam -ifrunning`).
   */
  void steam_overlay_toggle_via_steam() {
    std::thread([] {
      // DISPLAY is unset in kms. Steam's X11 client still talks to session :0.
      (void) std::system("env DISPLAY=:0 /usr/bin/steam -ifrunning steam://overlay/toggle >/dev/null 2>&1");
    }).detach();
    BOOST_LOG(info) << "gamescope overlay: steam://overlay/toggle (no mapped Big Picture)"sv;
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
   * @brief Strip screen number from an X11 display name (`:0.0` → `:0`).
   *
   * @param name `DisplayString` or sidecar `display=`.
   * @return Display without a trailing `.0`.
   */
  std::string display_stem(std::string_view name) {
    std::string out {name};
    if (out.size() >= 2 && out.compare(out.size() - 2, 2, ".0") == 0) {
      out.resize(out.size() - 2);
    }
    return out;
  }

  /**
   * @brief Read `$XDG_RUNTIME_DIR/second-screen-touch` (or `SECOND_SCREEN_TOUCH_FILE`).
   *
   * @return Parsed target, or nullopt when missing / `:2`.
   */
  std::optional<platf::gamescope::second_screen_touch_t> load_second_screen_touch() {
    std::string path;
    if (const char *env = std::getenv("SECOND_SCREEN_TOUCH_FILE")) {
      if (env[0]) {
        path = env;
      }
    }
    if (path.empty()) {
      if (const char *rt = std::getenv("XDG_RUNTIME_DIR")) {
        path = std::string {rt} + "/second-screen-touch";
      }
    }
    if (path.empty()) {
      return std::nullopt;
    }
    std::ifstream in {path};
    if (!in) {
      return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return platf::gamescope::parse_second_screen_touch(buf.str());
  }

  /**
   * @brief True when *window* is the live Second Screen mirror source on *dpy*.
   *
   * @param dpy Session Xwayland.
   * @param window Candidate xid.
   * @return True when the sidecar matches this display and xid.
   */
  bool sidecar_xid_on_display(Display *dpy, Window window) {
    const auto touch = load_second_screen_touch();
    if (!touch) {
      return false;
    }
    if (window != static_cast<Window>(touch->xid)) {
      return false;
    }
    return display_stem(DisplayString(dpy)) == display_stem(touch->display);
  }

  /**
   * @brief Source xid from the Second Screen QAM mirror sidecar.
   *
   * @param dpy Session Xwayland (`:0` / `:1`).
   * @return Window id, or `None`.
   */
  Window second_screen_touch_window(Display *dpy) {
    const auto touch = load_second_screen_touch();
    if (!touch) {
      return None;
    }
    if (display_stem(DisplayString(dpy)) != display_stem(touch->display)) {
      return None;
    }
    const auto window = static_cast<Window>(touch->xid);
    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, window, &attr) || attr.width <= 0 || attr.height <= 0) {
      return None;
    }
    return window;
  }

  /**
   * @brief Return the GamePad View or Azahar Secondary Window, refreshing a stale cache.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window gamepad_view_window(Display *dpy) {
    if (cached_pad != None && cached_pad_dpy == dpy) {
      XWindowAttributes attr {};
      if (XGetWindowAttributes(dpy, cached_pad, &attr) && attr.width > 0 && attr.height > 0) {
        const auto title = window_title(dpy, cached_pad);
        if (platf::gamescope::title_is_touch_surface(title) || sidecar_xid_on_display(dpy, cached_pad)) {
          return cached_pad;
        }
      }
      cached_pad = None;
      cached_pad_dpy = nullptr;
    }
    cached_pad = find_window(dpy, platf::gamescope::title_is_touch_surface);
    if (cached_pad == None) {
      cached_pad = second_screen_touch_window(dpy);
      if (cached_pad != None) {
        BOOST_LOG(info) << "GamePad inject: Second Screen mirror xid="sv << cached_pad
                        << " on "sv << DisplayString(dpy);
      }
    }
    cached_pad_dpy = cached_pad != None ? dpy : nullptr;
    return cached_pad;
  }

  /**
   * @brief Session Xwayland that currently has GamePad View / Azahar Secondary.
   *
   * Prefers `:1` (`FOCUS_DISPLAY=1`), then `:0`. Never `:2`.
   *
   * @return Display pointer, or `nullptr` when neither Xwayland has a pad.
   */
  Display *x11_touch_display() {
    if (cached_pad_dpy && cached_pad != None) {
      if (gamepad_view_window(cached_pad_dpy) != None) {
        return cached_pad_dpy;
      }
    }

    const auto count = platf::gamescope::session_x11_touch_count();
    for (std::size_t i = 0; i < count; ++i) {
      const auto *name = platf::gamescope::session_x11_touch_name(i);
      auto *dpy = x11_open(name);
      if (!dpy) {
        continue;
      }
      if (gamepad_view_window(dpy) != None) {
        BOOST_LOG(info) << "GamePad inject: using "sv << DisplayString(dpy);
        return dpy;
      }
    }
    static bool logged_missing = false;
    if (!logged_missing) {
      BOOST_LOG(warning) << "GamePad inject: no GamePad View / Azahar Secondary Window / Second Screen mirror on :1 or :0"sv;
      logged_missing = true;
    }
    return nullptr;
  }

  /**
   * @brief Return Cemu TV or Azahar Primary Window, refreshing a stale cache.
   *
   * @param dpy X11 display.
   * @return Window id, or `None`.
   */
  Window hdmi_surface_window(Display *dpy) {
    if (cached_hdmi != None && cached_hdmi_dpy == dpy) {
      XWindowAttributes attr {};
      if (XGetWindowAttributes(dpy, cached_hdmi, &attr) && attr.width > 0 && attr.height > 0) {
        const auto title = window_title(dpy, cached_hdmi);
        if (platf::gamescope::title_is_hdmi_surface(title)) {
          return cached_hdmi;
        }
      }
      cached_hdmi = None;
      cached_hdmi_dpy = nullptr;
    }
    cached_hdmi = find_window(dpy, platf::gamescope::title_is_hdmi_surface);
    cached_hdmi_dpy = cached_hdmi != None ? dpy : nullptr;
    return cached_hdmi;
  }

  /**
   * @brief Session Xwayland that currently has Cemu TV / Azahar / Eden.
   *
   * Prefers `:1` (`FOCUS_DISPLAY=1`), then `:0`. Never `:2`.
   *
   * @return Display pointer, or `nullptr` when neither Xwayland has the TV.
   */
  Display *x11_hdmi_display() {
    if (cached_hdmi_dpy && cached_hdmi != None) {
      if (hdmi_surface_window(cached_hdmi_dpy) != None) {
        return cached_hdmi_dpy;
      }
    }

    const auto count = platf::gamescope::session_x11_touch_count();
    for (std::size_t i = 0; i < count; ++i) {
      const auto *name = platf::gamescope::session_x11_touch_name(i);
      auto *dpy = x11_open(name);
      if (!dpy) {
        continue;
      }
      if (hdmi_surface_window(dpy) != None) {
        BOOST_LOG(info) << "HDMI inject: using "sv << DisplayString(dpy);
        return dpy;
      }
    }
    static bool logged_missing = false;
    if (!logged_missing) {
      BOOST_LOG(warning) << "HDMI inject: no Cemu TV / Azahar / Eden window on :1 or :0"sv;
      logged_missing = true;
    }
    return nullptr;
  }

  /**
   * @brief Steam Big Picture on `:0` when hold-Select overlay is showing.
   *
   * @return Overlay window, or `None`.
   */
  Window steam_overlay_window_if_on() {
    auto *dpy = x11_display();
    if (!dpy) {
      return None;
    }
    const auto bpm = find_steam_overlay_window(dpy);
    if (bpm == None || !overlay_is_on(dpy, bpm)) {
      return None;
    }
    return bpm;
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
    static int remaining = 48;
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
   * under the TV. `XWarpPointer` dest is that window (not the 4K nested
   * root) so `XQueryPointer` stays in GamePad-local pixels. Restore
   * `FOCUS_DISPLAY` middle `1` first — Steam / kms restart can leave it
   * on `:0` and Cemu drops the events.
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
    ensure_nested_mouse_focus();
    const auto [rx, ry] = root_xy(dpy, window, x, y);
    // Dest is the GamePad/TV window, not the nested root. On a 4K :1 the
    // GamePad is 1920×1080 at 0,0 — warping the root to those pixels puts
    // XQueryPointer on the Cemu TV (full 3840×2160) in the top-left quarter.
    XWarpPointer(dpy, None, window, 0, 0, 0, 0, x, y);

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

  bool xtest_ready(Display *dpy);

  /**
   * @brief Deliver a pointer event to the HDMI / TV game surface.
   *
   * Steam overlay stays `XSendEvent` (BPM accepts it). Cemu TV / Azahar
   * keep the proven `XSendEvent` path. Eden HDMI is not this function —
   * those taps are a relative trackpad via `move_mouse` / `button_mouse`.
   *
   * @param dpy X11 display.
   * @param frame HDMI surface frame (Cemu TV / Azahar / overlay).
   * @param window Input target (GL child or the frame).
   * @param type `ButtonPress`, `ButtonRelease`, or `MotionNotify`.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @param state Modifier / button mask (XSendEvent only).
   * @param button Button number, or `0` for motion.
   */
  void send_hdmi_pointer(Display *dpy, Window frame, Window window, int type, int x, int y, unsigned int state, unsigned int button) {
    (void) frame;
    send_pointer(dpy, window, type, x, y, state, button);
  }

  /**
   * @brief Delete a window property if the atom exists.
   *
   * @param dpy X11 display.
   * @param window Target window.
   * @param name Atom name.
   */
  void delete_property(Display *dpy, Window window, const char *name) {
    const auto atom = XInternAtom(dpy, name, True);
    if (atom) {
      XDeleteProperty(dpy, window, atom);
    }
  }

  /**
   * @brief True when this Xwayland has the XTEST extension.
   *
   * @param dpy X11 display.
   * @return True when `XTestFake*` can inject real pointer events.
   */
  bool xtest_ready(Display *dpy) {
    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    return XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor) == True;
  }

  /**
   * @brief Make GamePad hittable for XTEST (overlay-tagged windows are skipped).
   *
   * @param dpy X11 display.
   * @param pad GamePad View frame.
   * @param target GL child (or the frame).
   */
  void expose_gamepad_hit(Display *dpy, Window pad, Window target) {
    delete_property(dpy, pad, "GAMESCOPE_EXTERNAL_OVERLAY");
    delete_property(dpy, pad, "_NET_WM_WINDOW_OPACITY");
    if (target != pad) {
      delete_property(dpy, target, "GAMESCOPE_EXTERNAL_OVERLAY");
      delete_property(dpy, target, "_NET_WM_WINDOW_OPACITY");
    }
    XRaiseWindow(dpy, pad);
    if (target != pad) {
      XRaiseWindow(dpy, target);
    }
  }

  /**
   * @brief Hide GamePad from HDMI scanout again after XTEST.
   *
   * @param dpy X11 display.
   * @param pad GamePad View frame.
   * @param target GL child (or the frame).
   */
  void cover_gamepad_hit(Display *dpy, Window pad, Window target) {
    set_cardinal(dpy, pad, "GAMESCOPE_EXTERNAL_OVERLAY", 1);
    set_cardinal(dpy, pad, "_NET_WM_WINDOW_OPACITY", 0);
    if (target != pad) {
      set_cardinal(dpy, target, "GAMESCOPE_EXTERNAL_OVERLAY", 1);
    }
  }

  /**
   * @brief Deliver a real pointer event to GamePad View.
   *
   * wx/GTK drop `XSendEvent` (`send_event=True`). Overlay-tag + opacity 0
   * also skip gamescope hit-test, so `XQueryPointer` stays on the 4K TV.
   * Briefly un-hide the pad, `XTestFake*` (real events), then re-cover.
   *
   * @param dpy X11 display.
   * @param pad GamePad View frame.
   * @param window GL child.
   * @param type `ButtonPress`, `ButtonRelease`, or `MotionNotify`.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @param button Button number, or `0` for motion.
   * @param cover_after Re-apply overlay hide after this event.
   */
  void send_gamepad_pointer(Display *dpy, Window pad, Window window, int type, int x, int y, unsigned int button, bool cover_after) {
    ensure_nested_mouse_focus();
    expose_gamepad_hit(dpy, pad, window);
    const auto [rx, ry] = root_xy(dpy, window, x, y);
    XWarpPointer(dpy, None, window, 0, 0, 0, 0, x, y);
    if (xtest_ready(dpy)) {
      XTestFakeMotionEvent(dpy, DefaultScreen(dpy), rx, ry, CurrentTime);
      if (type == ButtonPress) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
      } else if (type == ButtonRelease) {
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
      }
    } else {
      send_pointer(dpy, window, type, x, y, type == ButtonRelease ? Button1Mask : 0, button);
    }
    if (cover_after) {
      cover_gamepad_hit(dpy, pad, window);
    }
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
        BOOST_LOG(warning) << "GamePad inject: no GamePad View / Azahar Secondary Window on this Xwayland"sv;
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
    (void) state;
    const bool cover_after = type == ButtonRelease || (type == MotionNotify && !pad_pointer_down);
    send_gamepad_pointer(dpy, pad, target, type, x, y, button, cover_after);
    XFlush(dpy);
    return true;
  }

  /**
   * @brief Log the first few HDMI pointer events with mapped pixels.
   *
   * @param kind Short label (`abs`, `button`).
   * @param nx Unit X before clamp.
   * @param ny Unit Y before clamp.
   * @param x Window-local X.
   * @param y Window-local Y.
   * @param window Target xid.
   * @param width Target width.
   * @param height Target height.
   */
  void log_hdmi_pointer(std::string_view kind, float nx, float ny, int x, int y, Window window, int width, int height) {
    static int remaining = 12;
    if (remaining <= 0) {
      return;
    }
    --remaining;
    BOOST_LOG(info) << "HDMI "sv << kind << " unit="sv << nx << ',' << ny
                    << " px="sv << x << ',' << y
                    << " xid="sv << window << ' ' << width << 'x' << height;
  }

  /**
   * @brief Resolve Cemu TV, map unit coords, and send a pointer event.
   *
   * Same delivery as GamePad inject (`XWarpPointer` + `XSendEvent` mask 0).
   * Host uinput does not reach wx/GTK on session `:1`.
   *
   * @param dpy X11 display.
   * @param frame Cemu TV / Azahar Primary, or Steam Big Picture when overlay is on.
   * @param nx Unit X in `[0, 1]`.
   * @param ny Unit Y in `[0, 1]`.
   * @param kind Log label.
   * @param type `ButtonPress`, `ButtonRelease`, or `MotionNotify`.
   * @param state Modifier / button mask.
   * @param button Button number, or `0` for motion.
   * @return True when the HDMI surface exists and the event was flushed.
   */
  bool inject_hdmi_unit(Display *dpy, Window frame, float nx, float ny, std::string_view kind, int type, unsigned int state, unsigned int button) {
    if (frame == None) {
      static bool logged_missing = false;
      if (!logged_missing) {
        BOOST_LOG(warning) << "HDMI inject: no Cemu TV / Eden / Steam overlay window on this Xwayland"sv;
        logged_missing = true;
      }
      return false;
    }
    const auto target = gamepad_input_window(dpy, frame);

    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, target, &attr) || attr.width <= 0 || attr.height <= 0) {
      return false;
    }

    const auto [x, y] = platf::gamescope::touch_to_window_xy(nx, ny, attr.width, attr.height);
    last_hdmi_x = x;
    last_hdmi_y = y;
    last_hdmi_xy_valid = true;
    log_hdmi_pointer(kind, nx, ny, x, y, target, attr.width, attr.height);
    send_hdmi_pointer(dpy, frame, target, type, x, y, state, button);
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
    // Cemu TV / Azahar Primary live on :1 after FOCUS_DISPLAY=1. Searching
    // only :0 leaves hide_overlay(tv=0) and Steam cannot restore the game.
    Window tv = find_window(dpy, platf::gamescope::title_is_hdmi_surface);
    Display *hdmi_dpy = nullptr;
    if (tv == None) {
      hdmi_dpy = x11_hdmi_display();
      if (hdmi_dpy) {
        tv = hdmi_surface_window(hdmi_dpy);
      }
    }
    remember_appid(hdmi_dpy ? hdmi_dpy : dpy, tv);
    const auto appid = remembered_appid;
    if (bpm == None) {
      steam_overlay_toggle_via_steam();
      if (appid != 0) {
        set_cardinal(dpy, DefaultRootWindow(dpy), "GAMESCOPE_FOCUSED_APP_GFX", appid);
        XFlush(dpy);
      }
      return;
    }
    if (platf::gamescope::overlay_toggle_action(overlay_is_on(dpy, bpm)) == platf::gamescope::overlay_action_e::hide) {
      hide_overlay(dpy, bpm, tv, appid);
    } else {
      show_overlay(dpy, bpm, appid);
    }
#endif
  }

  bool inject_gamepad_view_touch(const touch_port_t &touch_port, const touch_input_t &touch) {
#ifdef SUNSHINE_BUILD_X11
    if (!platf::gamescope::touch_is_second_display(touch.pointerId)) {
      return false;
    }

    // prepare_absolute_pointer_data yields desktop pixels, not [0, 1].
    float nx = touch.x;
    float ny = touch.y;
    if (touch_port.width > 0 && touch_port.height > 0) {
      const auto unit = platf::gamescope::abs_to_unit(
        touch.x,
        touch.y,
        touch_port.offset_x,
        touch_port.offset_y,
        touch_port.width,
        touch_port.height
      );
      nx = unit.first;
      ny = unit.second;
    }

    std::scoped_lock lock {x11_lock};
    auto *dpy = x11_touch_display();
    if (!dpy) {
      return false;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL_ALL:
        if (pad_pointer_down) {
          const auto ok = inject_unit(dpy, nx, ny, "touch-cancel"sv, ButtonRelease, Button1Mask, Button1);
          pad_pointer_down = false;
          return ok;
        }
        return true;
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        pad_pointer_down = false;
        return inject_unit(dpy, nx, ny, "touch-up"sv, ButtonRelease, Button1Mask, Button1);
      case LI_TOUCH_EVENT_DOWN:
        if (!inject_unit(dpy, nx, ny, "touch-move"sv, MotionNotify, 0, 0)) {
          return false;
        }
        pad_pointer_down = true;
        return inject_unit(dpy, nx, ny, "touch-down"sv, ButtonPress, 0, Button1);
      case LI_TOUCH_EVENT_MOVE:
        return inject_unit(dpy, nx, ny, "touch-drag"sv, MotionNotify, pad_pointer_down ? Button1Mask : 0, 0);
      case LI_TOUCH_EVENT_HOVER:
        return inject_unit(dpy, nx, ny, "touch-hover"sv, MotionNotify, 0, 0);
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
    auto *dpy = x11_touch_display();
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
    auto *dpy = x11_touch_display();
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
    send_gamepad_pointer(dpy, pad, target, type, last_pad_x, last_pad_y, x_button, release);
    pad_pointer_down = !release && x_button == Button1;
    XFlush(dpy);
    return true;
#else
    (void) button;
    (void) release;
    return false;
#endif
  }

  bool inject_hdmi_surface_abs_mouse(const touch_port_t &touch_port, float x, float y) {
#ifdef SUNSHINE_BUILD_X11
    std::scoped_lock lock {x11_lock};
    const auto [nx, ny] = platf::gamescope::abs_to_unit(
      x,
      y,
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port.width,
      touch_port.height
    );
    const auto overlay = steam_overlay_window_if_on();
    if (overlay != None) {
      auto *dpy = x11_display();
      if (!dpy) {
        return false;
      }
      static bool logged_overlay = false;
      if (!logged_overlay) {
        BOOST_LOG(info) << "HDMI inject: overlay on "sv << DisplayString(dpy);
        logged_overlay = true;
      }
      return inject_hdmi_unit(dpy, overlay, nx, ny, "overlay-abs"sv, MotionNotify, hdmi_pointer_down ? Button1Mask : 0, 0);
    }
    auto *dpy = x11_hdmi_display();
    if (!dpy) {
      return false;
    }
    const auto frame = hdmi_surface_window(dpy);
    if (frame == None) {
      return false;
    }
    const auto title = window_title(dpy, frame);
    if (!platf::gamescope::hdmi_surface_uses_absolute_clicks(title)) {
      static bool logged_trackpad = false;
      if (!logged_trackpad) {
        BOOST_LOG(info) << "HDMI inject: trackpad (no abs clicks) for "sv << title;
        logged_trackpad = true;
      }
      return false;
    }
    return inject_hdmi_unit(dpy, frame, nx, ny, "abs"sv, MotionNotify, hdmi_pointer_down ? Button1Mask : 0, 0);
#else
    (void) touch_port;
    (void) x;
    (void) y;
    return false;
#endif
  }

  bool inject_hdmi_surface_button(int button, bool release) {
#ifdef SUNSHINE_BUILD_X11
    const auto x_button = x11_button(button);
    if (x_button == 0) {
      return false;
    }

    std::scoped_lock lock {x11_lock};
    if (!last_hdmi_xy_valid) {
      return false;
    }

    Display *dpy = nullptr;
    Window frame = None;
    const auto overlay = steam_overlay_window_if_on();
    if (overlay != None) {
      dpy = x11_display();
      frame = overlay;
    } else {
      dpy = x11_hdmi_display();
      frame = dpy ? hdmi_surface_window(dpy) : None;
    }
    if (!dpy || frame == None) {
      return false;
    }
    if (overlay == None && !platf::gamescope::hdmi_surface_uses_absolute_clicks(window_title(dpy, frame))) {
      return false;
    }

    const auto target = gamepad_input_window(dpy, frame);
    XWindowAttributes attr {};
    if (!XGetWindowAttributes(dpy, target, &attr) || attr.width <= 0 || attr.height <= 0) {
      return false;
    }

    const auto nx = attr.width > 1 ? static_cast<float>(last_hdmi_x) / static_cast<float>(attr.width - 1) : 0.0F;
    const auto ny = attr.height > 1 ? static_cast<float>(last_hdmi_y) / static_cast<float>(attr.height - 1) : 0.0F;
    log_hdmi_pointer(release ? "button-up"sv : "button-down"sv, nx, ny, last_hdmi_x, last_hdmi_y, target, attr.width, attr.height);
    const auto type = release ? ButtonRelease : ButtonPress;
    const unsigned int state = release ? Button1Mask : 0;
    send_hdmi_pointer(dpy, frame, target, type, last_hdmi_x, last_hdmi_y, state, x_button);
    hdmi_pointer_down = !release && x_button == Button1;
    XFlush(dpy);
    return true;
#else
    (void) button;
    (void) release;
    return false;
#endif
  }

  bool inject_hdmi_surface_touch(const touch_port_t &touch_port, const touch_input_t &touch) {
#ifdef SUNSHINE_BUILD_X11
    if (platf::gamescope::touch_is_second_display(touch.pointerId)) {
      return false;
    }

    float nx = touch.x;
    float ny = touch.y;
    if (touch_port.width > 0 && touch_port.height > 0) {
      const auto unit = platf::gamescope::abs_to_unit(
        touch.x,
        touch.y,
        touch_port.offset_x,
        touch_port.offset_y,
        touch_port.width,
        touch_port.height
      );
      nx = unit.first;
      ny = unit.second;
    }

    std::scoped_lock lock {x11_lock};
    const auto overlay = steam_overlay_window_if_on();
    Display *dpy = nullptr;
    Window frame = None;
    if (overlay != None) {
      dpy = x11_display();
      frame = overlay;
    } else {
      dpy = x11_hdmi_display();
      frame = dpy ? hdmi_surface_window(dpy) : None;
    }
    if (!dpy || frame == None) {
      return false;
    }
    if (overlay == None && !platf::gamescope::hdmi_surface_uses_absolute_clicks(window_title(dpy, frame))) {
      return false;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL_ALL:
        if (hdmi_pointer_down) {
          const auto ok = inject_hdmi_unit(dpy, frame, nx, ny, "touch-cancel"sv, ButtonRelease, Button1Mask, Button1);
          hdmi_pointer_down = false;
          return ok;
        }
        return true;
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        hdmi_pointer_down = false;
        return inject_hdmi_unit(dpy, frame, nx, ny, "touch-up"sv, ButtonRelease, Button1Mask, Button1);
      case LI_TOUCH_EVENT_DOWN:
        if (!inject_hdmi_unit(dpy, frame, nx, ny, "touch-move"sv, MotionNotify, 0, 0)) {
          return false;
        }
        hdmi_pointer_down = true;
        return inject_hdmi_unit(dpy, frame, nx, ny, "touch-down"sv, ButtonPress, 0, Button1);
      case LI_TOUCH_EVENT_MOVE:
        return inject_hdmi_unit(dpy, frame, nx, ny, "touch-drag"sv, MotionNotify, hdmi_pointer_down ? Button1Mask : 0, 0);
      case LI_TOUCH_EVENT_HOVER:
        return inject_hdmi_unit(dpy, frame, nx, ny, "touch-hover"sv, MotionNotify, 0, 0);
      default:
        return false;
    }
#else
    (void) touch;
    return false;
#endif
  }

}  // namespace platf
