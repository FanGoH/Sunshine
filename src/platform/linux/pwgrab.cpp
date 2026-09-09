/**
 * @file src/platform/linux/pwgrab.cpp
 * @brief Grab an existing PipeWire Video/Source as a Sunshine display.
 *
 * Game Mode has no KWin `zkde_screencast_unstable_v1`. A headless gamescope
 * publishes a PipeWire node; KMS cannot see that plane. Primary capture stays
 * KMS (`HDMI-A-1`). The second stream opens this backend when
 * `dual_display_source` is `gamescope-virtual` or `pipewire:<serial>`.
 */
#include "pipewire.cpp"

// standard includes
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

using namespace std::literals;

namespace {
  /**
   * @brief Sidecar written by the playbook headless-gamescope helper.
   *
   * @return Absolute path, or empty when `XDG_RUNTIME_DIR` is unset.
   */
  std::string sidecar_path() {
    if (const char *env = std::getenv("SUNSHINE_DS_GAMESCOPE_VIRTUAL_FILE")) {
      if (env[0]) {
        return env;
      }
    }
    if (const char *rt = std::getenv("XDG_RUNTIME_DIR")) {
      return std::string {rt} + "/sunshine-ds-gamemode-virtual";
    }
    return {};
  }

  /**
   * @brief Parse `key=value` lines from the Game Mode virtual-display sidecar.
   *
   * @param path Sidecar path.
   * @param serial PipeWire object.serial.
   * @param node PipeWire node id.
   * @param width Frame width.
   * @param height Frame height.
   * @return True when a serial or node id was present.
   */
  bool load_sidecar(const std::string &path, uint64_t &serial, uint32_t &node, int &width, int &height) {
    std::ifstream in {path};
    if (!in) {
      return false;
    }
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
      const auto val = line.substr(eq + 1);
      if (key == "serial" && !val.empty()) {
        serial = std::strtoull(val.c_str(), nullptr, 10);
      } else if (key == "node" && !val.empty()) {
        node = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
      } else if (key == "width" && !val.empty()) {
        width = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
      } else if (key == "height" && !val.empty()) {
        height = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
      }
    }
    return serial != SPA_ID_INVALID || node != PW_ID_ANY;
  }

  /**
   * @brief Find the headless gamescope Video/Source via `pw-dump`.
   *
   * @param serial PipeWire object.serial.
   * @param node PipeWire node id.
   * @return True when a matching Video/Source was found.
   */
  bool lookup_headless_gamescope(uint64_t &serial, uint32_t &node) {
    constexpr auto cmd =
      "python3 - <<'PY'\n"
      "import json, subprocess, sys\n"
      "data = json.loads(subprocess.check_output(['pw-dump']))\n"
      "clients = set()\n"
      "for obj in data:\n"
      "    if obj.get('type') != 'PipeWire:Interface:Client':\n"
      "        continue\n"
      "    pid = str(((obj.get('info') or {}).get('props') or {}).get('application.process.id') or '')\n"
      "    if not pid:\n"
      "        continue\n"
      "    try:\n"
      "        cmd = open(f'/proc/{pid}/cmdline', 'rb').read().replace(b'\\0', b' ').decode()\n"
      "    except OSError:\n"
      "        continue\n"
      "    if '--backend headless' in cmd:\n"
      "        clients.add(str(obj.get('id')))\n"
      "for obj in data:\n"
      "    if obj.get('type') != 'PipeWire:Interface:Node':\n"
      "        continue\n"
      "    props = (obj.get('info') or {}).get('props') or {}\n"
      "    if props.get('media.class') != 'Video/Source':\n"
      "        continue\n"
      "    if str(props.get('client.id') or '') not in clients:\n"
      "        continue\n"
      "    print(props.get('object.serial') or '', obj.get('id') or '')\n"
      "    sys.exit(0)\n"
      "sys.exit(1)\n"
      "PY";
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
      return false;
    }
    char buf[128] {};
    const auto *got = fgets(buf, sizeof(buf), pipe);
    const int rc = pclose(pipe);
    if (!got || rc != 0) {
      return false;
    }
    unsigned long long parsed_serial = 0;
    unsigned long parsed_node = 0;
    if (std::sscanf(buf, "%llu %lu", &parsed_serial, &parsed_node) < 1) {
      return false;
    }
    if (parsed_serial != 0) {
      serial = parsed_serial;
    }
    if (parsed_node != 0) {
      node = static_cast<uint32_t>(parsed_node);
    }
    BOOST_LOG(info) << "[pwgrab] pw-dump serial="sv << serial << " node="sv << node;
    return serial != SPA_ID_INVALID || node != PW_ID_ANY;
  }

  /**
   * @brief Resolve a display name to a PipeWire node and/or object serial.
   *
   * @param display_name `gamescope-virtual`, `pipewire:<serial>`, or `pipewire:node:<id>`.
   * @param serial PipeWire object.serial.
   * @param node PipeWire node id.
   * @param width Frame width.
   * @param height Frame height.
   * @return True when capture can connect.
   */
  bool resolve_target(const std::string &display_name, uint64_t &serial, uint32_t &node, int &width, int &height) {
    serial = SPA_ID_INVALID;
    node = PW_ID_ANY;
    width = 1920;
    height = 1080;

    if (const char *env = std::getenv("SUNSHINE_DS_PIPEWIRE_SERIAL")) {
      if (env[0]) {
        serial = std::strtoull(env, nullptr, 10);
      }
    }
    if (const char *env = std::getenv("SUNSHINE_DS_PIPEWIRE_NODE")) {
      if (env[0]) {
        node = static_cast<uint32_t>(std::strtoul(env, nullptr, 10));
      }
    }

    if (display_name.rfind("pipewire:node:", 0) == 0) {
      node = static_cast<uint32_t>(std::strtoul(display_name.c_str() + 14, nullptr, 10));
      return node != 0 && node != PW_ID_ANY;
    }
    if (display_name.rfind("pipewire:", 0) == 0) {
      serial = std::strtoull(display_name.c_str() + 9, nullptr, 10);
      return serial != 0 && serial != SPA_ID_INVALID;
    }

    const auto path = sidecar_path();
    if (!path.empty() && load_sidecar(path, serial, node, width, height)) {
      BOOST_LOG(info) << "[pwgrab] sidecar "sv << path
                      << " serial="sv << serial << " node="sv << node
                      << " "sv << width << 'x' << height;
      return true;
    }

    if (serial != SPA_ID_INVALID || node != PW_ID_ANY) {
      return true;
    }

    BOOST_LOG(warning) << "[pwgrab] no sidecar at "sv << path
                       << "; looking up headless gamescope via pw-dump"sv;
    return lookup_headless_gamescope(serial, node);
  }
}  // namespace

namespace pwgrab {
  /**
   * @brief PipeWire display that attaches to an already published Video/Source.
   */
  class node_display_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief Skip wl_output discovery; this node is not a KWin/KMS monitor.
     */
    void verify_and_update_display_parameters() override {
      if (logical_width <= 0) {
        logical_width = width;
      }
      if (logical_height <= 0) {
        logical_height = height;
      }
      if (env_width <= 0) {
        env_width = width;
      }
      if (env_height <= 0) {
        env_height = height;
      }
      if (env_logical_width <= 0) {
        env_logical_width = width;
      }
      if (env_logical_height <= 0) {
        env_logical_height = height;
      }
    }

    /**
     * @brief Point the PipeWire stream at the configured existing node.
     *
     * @param display_name Capture selector.
     * @param out_pipewire_fd Unused; local PipeWire core.
     * @param out_pipewire_node Node id, or `PW_ID_ANY` when using object.serial.
     * @param out_pipewire_objectserial PipeWire object.serial.
     * @return 0 when a target was resolved.
     */
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      uint64_t serial = SPA_ID_INVALID;
      uint32_t node = PW_ID_ANY;
      int w = 1920;
      int h = 1080;
      if (!resolve_target(display_name, serial, node, w, h) ||
          (node == PW_ID_ANY && (serial & SPA_ID_INVALID) == SPA_ID_INVALID)) {
        BOOST_LOG(error) << "[pwgrab] no PipeWire Video/Source for "sv << display_name;
        return -1;
      }
      out_pipewire_fd = -1;
      out_pipewire_node = node;
      out_pipewire_objectserial = serial;
      offset_x = 0;
      offset_y = 0;
      width = w;
      height = h;
      logical_width = w;
      logical_height = h;
      env_width = w;
      env_height = h;
      env_logical_width = w;
      env_logical_height = h;
      BOOST_LOG(info) << "[pwgrab] attach serial="sv << serial << " node="sv << node
                      << " "sv << w << 'x' << h;
      return 0;
    }
  };
}  // namespace pwgrab

namespace platf {
  /**
   * @brief Create a PipeWire capture backend for an existing Video/Source node.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name `gamescope-virtual` or `pipewire:<serial>`.
   * @param config Configuration values to apply.
   * @return PipeWire display backend, or nullptr when the node cannot be opened.
   */
  std::shared_ptr<display_t> pipewire_node_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[pwgrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<pwgrab::node_display_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }
}  // namespace platf
