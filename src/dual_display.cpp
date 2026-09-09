/**
 * @file src/dual_display.cpp
 * @brief Definitions for second-display streaming.
 */
// local includes
#include "dual_display.h"

#include "config.h"
#include "display_device.h"
#include "logging.h"
#include "platform/common.h"

// lib includes
#include <display_device/factory.h>
#include <display_device/noop_audio_context.h>
#include <display_device/noop_settings_persistence.h>
#include <display_device/settings_manager_interface.h>

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <setupapi.h>
#elif defined(__linux__)
  #include <cerrno>
  #include <csignal>
  #include <cstdlib>
  #include <dirent.h>
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/socket.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char **environ;
#endif

using namespace std::literals;

namespace dual_display {

  namespace {

    /**
     * @brief The configured value that selects the virtual display.
     *
     * A reserved word rather than a separate boolean, so the setting reads as
     * one question — "where does the second display come from" — with one
     * answer. Two settings would allow the contradictory state of a named
     * monitor with the virtual flag also set, which somebody would then have to
     * decide the meaning of.
     */
    constexpr auto VIRTUAL = "virtual"sv;

    /**
     * @brief Game Mode second stream: headless gamescope PipeWire node.
     *
     * Not the KWin helper (`virtual` / `Virtual-sunshine-ds`). Primary capture
     * can stay KMS. Do not treat this name as a DRM connector.
     */
    constexpr auto GAMESCOPE_VIRTUAL = "gamescope-virtual"sv;

    /**
     * @brief Whether the configured second display is an existing PipeWire node.
     *
     * @param source `dual_display_source` value.
     * @return True for `gamescope-virtual` or `pipewire:<serial>`.
     */
    [[nodiscard]] bool is_pipewire_second_display(std::string_view source) {
      return source == GAMESCOPE_VIRTUAL || source.rfind("pipewire:", 0) == 0;
    }

    /**
     * @brief A real monitor that already exists.
     *
     * Nothing is created and nothing is destroyed; the lease is a name and a
     * mode. The mode is what the client asked for rather than what the monitor
     * is, because the encoder scales to the requested size and the alternative —
     * querying the monitor and streaming its native mode — sends a 4K desktop to
     * a panel that cannot show it.
     */
    class physical_lease_t: public lease_t {
    public:
      /**
       * @brief Retain the capture name of an already attached display.
       *
       * @param name Platform capture output name.
       */
      explicit physical_lease_t(std::string name):
          m_name {std::move(name)} {
      }

      /**
       * @brief Return the retained capture output name.
       *
       * @return Platform capture output name.
       */
      [[nodiscard]] std::string output_name() const override {
        return m_name;
      }

    private:
      std::string m_name;  ///< Platform capture output name.
    };

    /**
     * @brief Resolve a configured capture selector against attached outputs.
     *
     * Display-device configuration normally stores a stable device identifier,
     * while capture uses a GDI/DXGI output name. A raw output name remains
     * accepted for hand-written configurations.
     *
     * @param source Stable display identifier or platform output name.
     * @return Attached platform output name, or an empty string when the source
     * cannot be resolved.
     */
    [[nodiscard]] std::string resolve_output(const std::string &source) {
      const auto outputs = platf::display_names(platf::mem_type_e::system);
      const auto mapped = display_device::map_output_name(source);

      if (std::find(std::begin(outputs), std::end(outputs), mapped) != std::end(outputs)) {
        return mapped;
      }
      if (std::find(std::begin(outputs), std::end(outputs), source) != std::end(outputs)) {
        return source;
      }
      return {};
    }

    /**
     * @brief Resolve the output used by the primary video stream.
     *
     * @return Primary platform output name, or an empty string when no output is
     * attached.
     */
    [[nodiscard]] std::string primary_output() {
      if (!config::video.output_name.empty()) {
        if (auto configured = resolve_output(config::video.output_name); !configured.empty()) {
          return configured;
        }
      }

#ifdef _WIN32
      for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW display {};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0)) {
          break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
            (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) {
          const std::wstring name {display.DeviceName};
          return {std::begin(name), std::end(name)};
        }
      }
#endif

      const auto outputs = platf::display_names(platf::mem_type_e::system);
      return outputs.empty() ? std::string {} : outputs.front();
    }

#ifdef _WIN32
    /**
     * @brief Protocol version implemented by this SudoVDA client.
     */
    struct sudovda_protocol_version_t {
      std::uint8_t major;  ///< Breaking protocol generation.
      std::uint8_t minor;  ///< Backward-compatible feature generation.
      std::uint8_t incremental;  ///< Patch generation.
      std::uint8_t test_build;  ///< Nonzero for a development driver build.
    };

    /**
     * @brief Parameters for the SudoVDA add-monitor request.
     */
    struct sudovda_add_params_t {
      UINT width;  ///< Requested monitor width in pixels.
      UINT height;  ///< Requested monitor height in pixels.
      UINT refresh_rate;  ///< Requested whole-number refresh rate.
      GUID monitor_guid;  ///< Stable identity of the new virtual monitor.
      CHAR device_name[14];  ///< EDID display name, including its terminator.
      CHAR serial_number[14];  ///< EDID serial, including its terminator.
    };

    /**
     * @brief SudoVDA adapter and target identifiers returned after monitor add.
     */
    struct sudovda_add_result_t {
      LUID adapter_luid;  ///< Adapter containing the new target.
      UINT target_id;  ///< DisplayConfig target identifier for the new monitor.
    };

    /**
     * @brief Parameters for the SudoVDA remove-monitor request.
     */
    struct sudovda_remove_params_t {
      GUID monitor_guid;  ///< Identity originally supplied to add-monitor.
    };

    /**
     * @brief SudoVDA watchdog state.
     */
    struct sudovda_watchdog_t {
      UINT timeout;  ///< Watchdog timeout in seconds, or zero when disabled.
      UINT countdown;  ///< Current driver countdown in seconds.
    };

    /**
     * @brief SudoVDA protocol-version response.
     */
    struct sudovda_protocol_result_t {
      sudovda_protocol_version_t version;  ///< Driver protocol version.
    };

    constexpr GUID SUDOVDA_INTERFACE_GUID {
      0xe5bcc234,
      0x1e0c,
      0x418a,
      {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}
    };  ///< Device-interface GUID published by SudoVDA.
    constexpr sudovda_protocol_version_t SUDOVDA_PROTOCOL_VERSION {0, 2, 1, 1};  ///< Compatible SudoVDA protocol generation.
    constexpr DWORD IOCTL_ADD_VIRTUAL_DISPLAY = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);  ///< Add-monitor request.
    constexpr DWORD IOCTL_REMOVE_VIRTUAL_DISPLAY = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);  ///< Remove-monitor request.
    constexpr DWORD IOCTL_GET_WATCHDOG = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);  ///< Watchdog query.
    constexpr DWORD IOCTL_DRIVER_PING = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);  ///< Watchdog keepalive.
    constexpr DWORD IOCTL_GET_PROTOCOL_VERSION = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ff, METHOD_BUFFERED, FILE_ANY_ACCESS);  ///< Protocol query.

    static_assert(sizeof(sudovda_protocol_version_t) == 4);
    static_assert(sizeof(sudovda_add_params_t) == 56);
    static_assert(sizeof(sudovda_add_result_t) == 12);

    /**
     * @brief Owning Windows handle used for SudoVDA requests.
     */
    class driver_handle_t {
    public:
      driver_handle_t() = default;

      /**
       * @brief Adopt an open driver handle.
       *
       * @param handle Native SudoVDA handle.
       */
      explicit driver_handle_t(HANDLE handle):
          m_handle {handle} {
      }

      driver_handle_t(const driver_handle_t &) = delete;
      driver_handle_t &operator=(const driver_handle_t &) = delete;

      /**
       * @brief Transfer ownership from another handle.
       *
       * @param other Handle whose ownership is transferred.
       */
      driver_handle_t(driver_handle_t &&other) noexcept:
          m_handle {std::exchange(other.m_handle, INVALID_HANDLE_VALUE)} {
      }

      /**
       * @brief Transfer ownership from another handle.
       *
       * @param other Handle whose ownership is transferred.
       * @return This handle.
       */
      driver_handle_t &operator=(driver_handle_t &&other) noexcept {
        if (this != &other) {
          reset();
          m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
      }

      /**
       * @brief Close the owned handle.
       */
      ~driver_handle_t() {
        reset();
      }

      /**
       * @brief Return the native driver handle.
       *
       * @return Native handle, or `INVALID_HANDLE_VALUE` when closed.
       */
      [[nodiscard]] HANDLE get() const {
        return m_handle;
      }

      /**
       * @brief Check whether the handle is open.
       */
      explicit operator bool() const {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
      }

      /**
       * @brief Close the handle if it is open.
       */
      void reset() {
        if (*this) {
          CloseHandle(m_handle);
          m_handle = INVALID_HANDLE_VALUE;
        }
      }

    private:
      HANDLE m_handle {INVALID_HANDLE_VALUE};  ///< Owned Windows handle.
    };

    /**
     * @brief Open the first installed SudoVDA device interface.
     *
     * @return Open driver handle, or an empty handle when the driver is absent.
     */
    [[nodiscard]] driver_handle_t open_sudovda() {
      const auto device_info = SetupDiGetClassDevsW(
        &SUDOVDA_INTERFACE_GUID,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
      );
      if (device_info == INVALID_HANDLE_VALUE) {
        return {};
      }

      auto cleanup = [&]() {
        SetupDiDestroyDeviceInfoList(device_info);
      };

      SP_DEVICE_INTERFACE_DATA interface_data {};
      interface_data.cbSize = sizeof(interface_data);
      if (!SetupDiEnumDeviceInterfaces(device_info, nullptr, &SUDOVDA_INTERFACE_GUID, 0, &interface_data)) {
        cleanup();
        return {};
      }

      DWORD detail_size = 0;
      SetupDiGetDeviceInterfaceDetailW(device_info, &interface_data, nullptr, 0, &detail_size, nullptr);
      if (detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        cleanup();
        return {};
      }

      std::vector<std::byte> detail_storage(detail_size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(device_info, &interface_data, detail, detail_size, nullptr, nullptr)) {
        cleanup();
        return {};
      }

      const auto handle = CreateFileW(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      cleanup();
      return driver_handle_t {handle};
    }

    /**
     * @brief Query whether an open driver implements the required protocol.
     *
     * @param handle Open SudoVDA handle.
     * @return True when the driver's protocol is backward-compatible.
     */
    [[nodiscard]] bool compatible_sudovda(HANDLE handle) {
      sudovda_protocol_result_t result {};
      DWORD returned = 0;
      if (!DeviceIoControl(
            handle,
            IOCTL_GET_PROTOCOL_VERSION,
            nullptr,
            0,
            &result,
            sizeof(result),
            &returned,
            nullptr
          )) {
        return false;
      }

      return returned >= sizeof(result) &&
             result.version.major == SUDOVDA_PROTOCOL_VERSION.major &&
             result.version.minor >= SUDOVDA_PROTOCOL_VERSION.minor;
    }

    /**
     * @brief Hash a stable monitor name into one 64-bit word.
     *
     * @param value Client identity and stream slot being hashed.
     * @param seed Independent initial state for this hash word.
     * @return Stable FNV-1a hash.
     */
    [[nodiscard]] std::uint64_t stable_hash(std::string_view value, std::uint64_t seed) {
      constexpr std::uint64_t prime = 1099511628211ULL;
      auto hash = seed;
      for (const auto byte : value) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= prime;
      }
      return hash;
    }

    /**
     * @brief Create a stable RFC 4122 variant GUID for a paired client's monitor.
     *
     * A stable identity lets Windows retain the Extend topology and position
     * across reconnects instead of accumulating a new ghost monitor for every
     * stream session.
     *
     * @param client_id Stable paired-client identifier.
     * @return Deterministic monitor identity for video stream 1.
     */
    [[nodiscard]] GUID make_monitor_guid(std::string_view client_id) {
      std::string monitor_name {client_id.empty() ? "unknown-client"sv : client_id};
      monitor_name += "/sunshine-ds/video/1";

      const auto first = stable_hash(monitor_name, 14695981039346656037ULL);
      const auto second = stable_hash(monitor_name, 7809847782465536322ULL);
      std::array<std::uint8_t, sizeof(GUID)> bytes {};
      std::memcpy(bytes.data(), &first, sizeof(first));
      std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));

      GUID guid {};
      std::memcpy(&guid, bytes.data(), sizeof(guid));
      guid.Data3 = static_cast<std::uint16_t>((guid.Data3 & 0x0fffU) | 0x5000U);
      guid.Data4[0] = static_cast<std::uint8_t>((guid.Data4[0] & 0x3fU) | 0x80U);
      return guid;
    }

    /**
     * @brief Resolve a newly added SudoVDA target to its GDI capture name.
     *
     * @param added Adapter and target returned by the driver.
     * @return GDI output name, or an empty string while Windows has not attached
     * the target yet.
     */
    [[nodiscard]] std::string added_display_name(const sudovda_add_result_t &added) {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return {};
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &path_count,
            paths.data(),
            &mode_count,
            modes.data(),
            nullptr
          ) != ERROR_SUCCESS) {
        return {};
      }

      const auto path = std::find_if(std::begin(paths), std::begin(paths) + path_count, [&](const auto &candidate) {
        return candidate.targetInfo.id == added.target_id &&
               candidate.targetInfo.adapterId.HighPart == added.adapter_luid.HighPart &&
               candidate.targetInfo.adapterId.LowPart == added.adapter_luid.LowPart;
      });
      if (path == std::begin(paths) + path_count) {
        return {};
      }

      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(source_name);
      source_name.header.adapterId = path->sourceInfo.adapterId;
      source_name.header.id = path->sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
        return {};
      }

      const std::wstring wide_name {source_name.viewGdiDeviceName};
      return {std::begin(wide_name), std::end(wide_name)};
    }

    /**
     * @brief Convert a GDI output name to its Windows wide-character form.
     *
     * GDI output names contain only ASCII characters (`\\.\DISPLAYn`), so no
     * locale-dependent conversion is required.
     *
     * @param output_name Platform capture output name.
     * @return Wide-character output name accepted by display settings APIs.
     */
    [[nodiscard]] std::wstring wide_output_name(std::string_view output_name) {
      return {std::begin(output_name), std::end(output_name)};
    }

    /**
     * @brief Query the active mode of one GDI output.
     *
     * @param output_name Platform capture output name.
     * @return Active display mode, or no value when Windows cannot query it.
     */
    [[nodiscard]] std::optional<DEVMODEW> current_output_mode(std::string_view output_name) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      const auto name = wide_output_name(output_name);
      if (!EnumDisplaySettingsExW(name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
        return std::nullopt;
      }
      return mode;
    }

    /**
     * @brief Check whether two display modes have the same stream-relevant mode.
     *
     * @param left First Windows display mode.
     * @param right Second Windows display mode.
     * @return True when width, height, and refresh rate match.
     */
    [[nodiscard]] bool same_output_mode(const DEVMODEW &left, const DEVMODEW &right) {
      return left.dmPelsWidth == right.dmPelsWidth &&
             left.dmPelsHeight == right.dmPelsHeight &&
             left.dmDisplayFrequency == right.dmDisplayFrequency;
    }

    /**
     * @brief Find the supported display mode nearest to a client request.
     *
     * Pixel dimensions are considered before refresh rate. This preserves the
     * capture geometry when a virtual-display driver does not expose the exact
     * panel mode; the encoder still scales or letterboxes to the client size.
     *
     * @param output_name Platform capture output name.
     * @param request Client-panel mode requested for the stream.
     * @return Closest enumerated mode, or no value when no mode is exposed.
     */
    [[nodiscard]] std::optional<DEVMODEW> closest_output_mode(std::string_view output_name, const request_t &request) {
      const auto name = wide_output_name(output_name);
      std::optional<DEVMODEW> best;
      long double best_pixel_distance = std::numeric_limits<long double>::max();
      std::uint64_t best_refresh_distance = std::numeric_limits<std::uint64_t>::max();

      for (DWORD index = 0;; ++index) {
        DEVMODEW candidate {};
        candidate.dmSize = sizeof(candidate);
        if (!EnumDisplaySettingsExW(name.c_str(), index, &candidate, 0)) {
          break;
        }

        const auto width_delta = static_cast<long double>(candidate.dmPelsWidth) - request.width;
        const auto height_delta = static_cast<long double>(candidate.dmPelsHeight) - request.height;
        const auto pixel_distance = width_delta * width_delta + height_delta * height_delta;
        const auto candidate_frequency = static_cast<std::uint64_t>(candidate.dmDisplayFrequency);
        const auto requested_frequency = static_cast<std::uint64_t>(request.framerate);
        const auto refresh_distance = candidate_frequency > requested_frequency ?
                                        candidate_frequency - requested_frequency :
                                        requested_frequency - candidate_frequency;

        if (!best || pixel_distance < best_pixel_distance ||
            (pixel_distance == best_pixel_distance && refresh_distance < best_refresh_distance)) {
          best = candidate;
          best_pixel_distance = pixel_distance;
          best_refresh_distance = refresh_distance;
        }
      }

      return best;
    }

    /**
     * @brief Apply a display mode without persisting it in the registry.
     *
     * @param output_name Platform capture output name.
     * @param mode Mode to apply.
     * @return True when Windows accepted the mode.
     */
    [[nodiscard]] bool apply_output_mode(std::string_view output_name, const DEVMODEW &mode) {
      auto requested = mode;
      requested.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
      if (mode.dmBitsPerPel != 0) {
        requested.dmFields |= DM_BITSPERPEL;
      }
      const auto name = wide_output_name(output_name);
      return ChangeDisplaySettingsExW(name.c_str(), &requested, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
    }

    /**
     * @brief Return a lower-case copy used for virtual-adapter identification.
     *
     * @param value Windows device metadata.
     * @return Lower-case metadata.
     */
    [[nodiscard]] std::wstring lowercase(std::wstring value) {
      std::transform(std::begin(value), std::end(value), std::begin(value), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
      });
      return value;
    }

    /**
     * @brief Score metadata for known virtual display adapters.
     *
     * @param metadata Adapter and monitor identifiers and descriptions.
     * @return Positive preference score for recognized virtual displays.
     */
    [[nodiscard]] int virtual_metadata_score(const std::wstring &metadata) {
      const auto normalized = lowercase(metadata);
      int score = 0;
      if (normalized.find(L"mtt1337") != std::wstring::npos) {
        score += 1000;
      }
      if (normalized.find(L"mttvdd") != std::wstring::npos ||
          normalized.find(L"mikethetech") != std::wstring::npos) {
        score += 800;
      }
      if (normalized.find(L"virtual display") != std::wstring::npos ||
          normalized.find(L"virtual monitor") != std::wstring::npos) {
        score += 400;
      }
      if (normalized.find(L"root\\display") != std::wstring::npos) {
        score += 100;
      }
      return score;
    }

    /**
     * @brief Identify whether an attached GDI output belongs to a virtual adapter.
     *
     * @param output_name Platform capture output name.
     * @return Preference score, with zero meaning no recognized virtual metadata.
     */
    [[nodiscard]] int virtual_output_score(std::string_view output_name) {
      const auto wanted_name = wide_output_name(output_name);
      for (DWORD adapter_index = 0;; ++adapter_index) {
        DISPLAY_DEVICEW adapter {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapter_index, &adapter, 0)) {
          break;
        }
        if (wanted_name != adapter.DeviceName) {
          continue;
        }

        std::wstring metadata {adapter.DeviceString};
        metadata += L' ';
        metadata += adapter.DeviceID;
        metadata += L' ';
        metadata += adapter.DeviceKey;
        auto score = virtual_metadata_score(metadata);

        for (DWORD monitor_index = 0;; ++monitor_index) {
          DISPLAY_DEVICEW monitor {};
          monitor.cb = sizeof(monitor);
          if (!EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) {
            break;
          }
          std::wstring monitor_metadata {monitor.DeviceString};
          monitor_metadata += L' ';
          monitor_metadata += monitor.DeviceID;
          monitor_metadata += L' ';
          monitor_metadata += monitor.DeviceKey;
          score += virtual_metadata_score(monitor_metadata);
        }
        return score;
      }
      return 0;
    }

    /**
     * @brief Return a lower-case copy of libdisplaydevice metadata.
     *
     * @param value Display-device metadata.
     * @return Lower-case metadata.
     */
    [[nodiscard]] std::string lowercase(std::string value) {
      std::transform(std::begin(value), std::end(value), std::begin(value), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    }

    /**
     * @brief Score one enumerated device for known virtual-display metadata.
     *
     * MikeTheTech VDD exposes an inactive monitor through libdisplaydevice even
     * though it has no GDI capture name yet. Its EDID uses the MTT vendor and
     * 1337 product identifiers, while older releases may expose only the exact
     * "Virtual Display Driver" friendly name.
     *
     * @param device Device returned by libdisplaydevice.
     * @return Positive preference score for a recognized virtual display.
     */
    [[nodiscard]] int virtual_device_score(const display_device::EnumeratedDevice &device) {
      std::string metadata = device.m_device_id + ' ' + device.m_display_name + ' ' + device.m_friendly_name;
      if (device.m_edid) {
        metadata += ' ' + device.m_edid->m_manufacturer_id + ' ' + device.m_edid->m_product_code;
      }

      const auto normalized = lowercase(std::move(metadata));
      int score = 0;
      if (normalized.find("mtt1337") != std::string::npos) {
        score += 1000;
      }
      if (normalized.find("mttvdd") != std::string::npos ||
          normalized.find("mikethetech") != std::string::npos) {
        score += 800;
      }
      if (device.m_edid && lowercase(device.m_edid->m_manufacturer_id) == "mtt") {
        score += 800;
      }
      if (normalized.find("virtual display driver") != std::string::npos ||
          normalized.find("virtual display adapter") != std::string::npos ||
          normalized.find("virtual monitor") != std::string::npos) {
        score += 400;
      }
      return score;
    }

    /**
     * @brief Find the best inactive virtual device without changing topology.
     *
     * @param manager Settings manager used to enumerate active and inactive devices.
     * @return Recognized inactive device, or no value when none is available.
     */
    [[nodiscard]] std::optional<display_device::EnumeratedDevice> find_inactive_virtual_device(
      display_device::SettingsManagerInterface &manager
    ) {
      std::optional<display_device::EnumeratedDevice> best;
      int best_score = 0;
      for (const auto &device : manager.enumAvailableDevices()) {
        if (device.m_info || !device.m_display_name.empty()) {
          continue;
        }

        const auto score = virtual_device_score(device);
        if (score > best_score) {
          best = device;
          best_score = score;
        }
      }
      return best;
    }

    /**
     * @brief Construct an isolated display manager for one virtual-display lease.
     *
     * The manager deliberately uses in-memory persistence and a no-op audio
     * context. It therefore cannot overwrite Sunshine's primary-display
     * persistence file or disturb the primary stream's captured audio context.
     *
     * @return Independent settings manager, or null when unsupported.
     */
    [[nodiscard]] std::unique_ptr<display_device::SettingsManagerInterface> make_virtual_settings_manager() {
      return display_device::makeSettingsManager({
        .m_audio_context_api = std::make_shared<display_device::NoopAudioContext>(),
        .m_settings_persistence_api = std::make_shared<display_device::NoopSettingsPersistence>()
      });
    }

    /**
     * @brief Detect a recognized detached virtual monitor without activating it.
     *
     * @return True when the host display API exposes a suitable inactive device.
     */
    [[nodiscard]] bool inactive_virtual_display_available() {
      auto devices = display_device::enumerate_devices();
      if (devices.empty()) {
        auto manager = make_virtual_settings_manager();
        if (!manager) {
          return false;
        }
        devices = manager->enumAvailableDevices();
      }

      return std::any_of(std::begin(devices), std::end(devices), [](const auto &device) {
        return !device.m_info && device.m_display_name.empty() && virtual_device_score(device) > 0;
      });
    }

    /**
     * @brief Candidate for a pre-existing virtual-display lease.
     */
    struct existing_output_t {
      std::string name;  ///< Platform capture output name.
      bool recognized_virtual;  ///< True for a known virtual-adapter identifier.
    };

    std::mutex existing_output_mutex;  ///< Protects pre-existing output claims.
    std::mutex virtual_activation_mutex;  ///< Serializes activation of detached virtual adapters.
    std::set<std::string> claimed_existing_outputs;  ///< Outputs whose temporary mode is owned by an active session.
    std::set<std::string> claimed_sudovda_identities;  ///< Stable monitor identities owned by active SudoVDA sessions.

    /**
     * @brief Exclusive claim on one stable SudoVDA monitor identity.
     */
    class sudovda_identity_claim_t {
    public:
      /**
       * @brief Try to claim a paired client's stable monitor slot.
       *
       * @param identity Stable paired-client identity.
       */
      explicit sudovda_identity_claim_t(std::string_view identity):
          m_identity {identity.empty() ? "unknown-client" : std::string {identity}} {
        std::scoped_lock lock {existing_output_mutex};
        m_owned = claimed_sudovda_identities.emplace(m_identity).second;
      }

      sudovda_identity_claim_t(const sudovda_identity_claim_t &) = delete;
      sudovda_identity_claim_t &operator=(const sudovda_identity_claim_t &) = delete;

      /**
       * @brief Transfer an exclusive identity claim.
       *
       * @param other Claim whose ownership is transferred.
       */
      sudovda_identity_claim_t(sudovda_identity_claim_t &&other) noexcept:
          m_identity {std::move(other.m_identity)},
          m_owned {std::exchange(other.m_owned, false)} {
      }

      /**
       * @brief Release the stable identity claim.
       */
      ~sudovda_identity_claim_t() {
        if (m_owned) {
          std::scoped_lock lock {existing_output_mutex};
          claimed_sudovda_identities.erase(m_identity);
        }
      }

      /**
       * @brief Check whether this object owns the identity.
       */
      explicit operator bool() const {
        return m_owned;
      }

    private:
      std::string m_identity;  ///< Stable paired-client key.
      bool m_owned {};  ///< True when this object inserted the global claim.
    };

    /**
     * @brief Find an unclaimed virtual output distinct from the primary stream.
     *
     * Only positively identified virtual adapters are eligible. A non-primary
     * physical monitor is not necessarily unused, and changing its mode without
     * an explicit named-display configuration would be destructive.
     *
     * @return Best unclaimed candidate, or no value if only the primary exists.
     */
    [[nodiscard]] std::optional<existing_output_t> find_existing_virtual_output() {
      const auto primary = primary_output();
      const auto outputs = platf::display_names(platf::mem_type_e::system);
      std::scoped_lock lock {existing_output_mutex};

      std::optional<existing_output_t> best;
      int best_score = -1;
      for (const auto &output : outputs) {
        if (output.empty() || output == primary || claimed_existing_outputs.contains(output)) {
          continue;
        }
        const auto score = virtual_output_score(output);
        if (score <= 0) {
          continue;
        }
        if (!best || score > best_score) {
          best = existing_output_t {output, true};
          best_score = score;
        }
      }
      return best;
    }

    /**
     * @brief Lease a pre-existing display without taking ownership of the device.
     */
    class existing_virtual_lease_t final: public lease_t {
    public:
      /**
       * @brief Adopt a claimed output and its optional temporary mode change.
       *
       * @param output_name Platform capture output name.
       * @param original_mode Mode active before Sunshine changed it.
       * @param applied_mode Mode Sunshine applied, if any.
       * @param settings_manager Manager that activated this output, if Sunshine
       * owns the topology change.
       */
      existing_virtual_lease_t(
        std::string output_name,
        std::optional<DEVMODEW> original_mode,
        std::optional<DEVMODEW> applied_mode,
        std::unique_ptr<display_device::SettingsManagerInterface> settings_manager = nullptr
      ):
          m_output_name {std::move(output_name)},
          m_original_mode {std::move(original_mode)},
          m_applied_mode {std::move(applied_mode)},
          m_settings_manager {std::move(settings_manager)} {
      }

      /**
       * @brief Restore Sunshine's temporary mode and release the non-owning claim.
       */
      ~existing_virtual_lease_t() override {
        if (m_original_mode && m_applied_mode && m_output_name != primary_output()) {
          const auto current = current_output_mode(m_output_name);
          if (current && same_output_mode(*current, *m_applied_mode)) {
            if (!apply_output_mode(m_output_name, *m_original_mode)) {
              BOOST_LOG(warning) << "Second display: failed to restore the previous mode on "sv << m_output_name;
            }
          } else if (current) {
            BOOST_LOG(info) << "Second display: preserving a mode changed by another component on "sv << m_output_name;
          }
        }

        if (m_settings_manager) {
          const auto result = m_settings_manager->revertSettings();
          if (result != display_device::SettingsManagerInterface::RevertResult::Ok) {
            BOOST_LOG(warning) << "Second display: failed to restore the topology after detaching "sv
                               << m_output_name << "; result="sv << static_cast<int>(result);
          } else {
            BOOST_LOG(info) << "Second display: restored the topology that preceded automatic activation of "sv
                            << m_output_name;
          }
        }

        std::scoped_lock lock {existing_output_mutex};
        claimed_existing_outputs.erase(m_output_name);
      }

      /**
       * @brief Return the claimed capture output name.
       *
       * @return Platform capture output name.
       */
      [[nodiscard]] std::string output_name() const override {
        return m_output_name;
      }

    private:
      std::string m_output_name;  ///< Non-owned platform capture output.
      std::optional<DEVMODEW> m_original_mode;  ///< Mode to restore when unchanged by others.
      std::optional<DEVMODEW> m_applied_mode;  ///< Temporary mode applied by Sunshine.
      std::unique_ptr<display_device::SettingsManagerInterface> m_settings_manager;  ///< Owns an automatic topology change.
    };

    /**
     * @brief Claim an existing virtual output for streaming.
     *
     * @param request Client-panel mode requested for the stream.
     * @param required_output Exact output to claim after automatic activation,
     * or no value to select the best attached output.
     * @param settings_manager Manager that owns the automatic activation, if any.
     * @return Non-owning display lease, or null when no output remains available.
     */
    [[nodiscard]] std::unique_ptr<lease_t> acquire_existing_virtual_display(
      const request_t &request,
      std::optional<std::string> required_output = std::nullopt,
      std::unique_ptr<display_device::SettingsManagerInterface> settings_manager = nullptr
    ) {
      auto cancel_activation = [&settings_manager]() {
        if (settings_manager &&
            settings_manager->revertSettings() != display_device::SettingsManagerInterface::RevertResult::Ok) {
          BOOST_LOG(warning) << "Second display: failed to revert an unusable automatic virtual-display activation"sv;
        }
      };

      for (;;) {
        auto candidate = required_output ?
                           std::make_optional(existing_output_t {*required_output, true}) :
                           find_existing_virtual_output();
        if (!candidate) {
          cancel_activation();
          return nullptr;
        }

        const auto outputs = platf::display_names(platf::mem_type_e::system);
        if (candidate->name.empty() || candidate->name == primary_output() ||
            std::find(std::begin(outputs), std::end(outputs), candidate->name) == std::end(outputs) ||
            virtual_output_score(candidate->name) <= 0) {
          cancel_activation();
          return nullptr;
        }

        {
          std::scoped_lock lock {existing_output_mutex};
          if (!claimed_existing_outputs.emplace(candidate->name).second) {
            if (required_output) {
              cancel_activation();
              return nullptr;
            }
            continue;
          }
        }

        auto original_mode = current_output_mode(candidate->name);
        std::optional<DEVMODEW> applied_mode;
        if (const auto requested_mode = closest_output_mode(candidate->name, request)) {
          if (requested_mode->dmPelsWidth != static_cast<DWORD>(request.width) ||
              requested_mode->dmPelsHeight != static_cast<DWORD>(request.height) ||
              requested_mode->dmDisplayFrequency != static_cast<DWORD>(request.framerate)) {
            BOOST_LOG(warning) << "Second display: requested "sv << request.width << 'x' << request.height
                               << '@' << request.framerate << ", using nearest driver mode "sv
                               << requested_mode->dmPelsWidth << 'x' << requested_mode->dmPelsHeight
                               << '@' << requested_mode->dmDisplayFrequency;
          }

          if (!original_mode || !same_output_mode(*original_mode, *requested_mode)) {
            if (apply_output_mode(candidate->name, *requested_mode)) {
              applied_mode = current_output_mode(candidate->name).value_or(*requested_mode);
            } else {
              BOOST_LOG(warning) << "Second display: Windows refused the requested mode on "sv << candidate->name
                                 << "; capturing its current mode and scaling to the client"sv;
            }
          }
        }

        const auto capture_mode = current_output_mode(candidate->name);
        BOOST_LOG(info) << "Second display: leasing existing "sv
                        << (candidate->recognized_virtual ? "virtual"sv : "unused"sv)
                        << " output "sv << candidate->name << " without device ownership"sv;
        if (capture_mode) {
          BOOST_LOG(info) << "Second display: capture mode is "sv << capture_mode->dmPelsWidth << 'x'
                          << capture_mode->dmPelsHeight << '@' << capture_mode->dmDisplayFrequency
                          << "; encoder output is "sv << request.width << 'x' << request.height
                          << '@' << request.framerate;
        }

        return std::make_unique<existing_virtual_lease_t>(
          std::move(candidate->name),
          std::move(original_mode),
          std::move(applied_mode),
          std::move(settings_manager)
        );
      }
    }

    /**
     * @brief Activate and lease one recognized inactive virtual display.
     *
     * Activation is targeted by libdisplaydevice's stable device identifier.
     * Unlike the global Extend-topology shortcut, this cannot enable an
     * unrelated disconnected physical monitor. The isolated settings manager
     * is transferred to the lease so teardown restores the exact topology it
     * observed before activation.
     *
     * @param request Client-panel mode requested for the stream.
     * @return Lease for the newly attached capture output, or null on failure.
     */
    [[nodiscard]] std::unique_ptr<lease_t> activate_existing_virtual_display(const request_t &request) {
      std::scoped_lock activation_lock {virtual_activation_mutex};

      auto manager = make_virtual_settings_manager();
      if (!manager) {
        return nullptr;
      }

      const auto candidate = find_inactive_virtual_device(*manager);
      if (!candidate) {
        return nullptr;
      }

      const auto primary_before = primary_output();
      display_device::SingleDisplayConfiguration configuration {
        .m_device_id = candidate->m_device_id,
        .m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive
      };
      const auto result = manager->applySettings(configuration);
      if (result != display_device::SettingsManagerInterface::ApplyResult::Ok) {
        BOOST_LOG(warning) << "Second display: could not activate recognized virtual device "sv
                           << candidate->m_friendly_name << "; result="sv << static_cast<int>(result);
        return nullptr;
      }

      std::string output_name;
      auto retry_delay = 20ms;
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        output_name = manager->getDisplayName(candidate->m_device_id);
        if (!output_name.empty()) {
          break;
        }
        std::this_thread::sleep_for(retry_delay);
        retry_delay = std::min(retry_delay * 2, 500ms);
      }

      if (output_name.empty() || output_name == primary_output() || primary_output() != primary_before ||
          virtual_output_score(output_name) <= 0) {
        if (output_name == primary_output() || primary_output() != primary_before) {
          BOOST_LOG(warning) << "Second display: automatic activation changed the primary output; reverting it"sv;
        } else if (output_name.empty()) {
          BOOST_LOG(warning) << "Second display: virtual device activated without exposing a capture output"sv;
        } else {
          BOOST_LOG(warning) << "Second display: activated output is not a recognized virtual adapter: "sv
                             << output_name;
        }
        if (manager->revertSettings() != display_device::SettingsManagerInterface::RevertResult::Ok) {
          BOOST_LOG(warning) << "Second display: failed to revert an invalid automatic activation"sv;
        }
        return nullptr;
      }

      BOOST_LOG(info) << "Second display: automatically activated virtual device "sv
                      << candidate->m_friendly_name << " as "sv << output_name;
      return acquire_existing_virtual_display(request, std::move(output_name), std::move(manager));
    }

    /**
     * @brief A SudoVDA monitor owned for exactly one streaming session.
     */
    class virtual_lease_t final: public lease_t {
    public:
      /**
       * @brief Adopt a created monitor and start its optional watchdog.
       *
       * @param handle Open SudoVDA control handle.
       * @param monitor_guid Identity used to remove the monitor.
       * @param output_name GDI name used by display capture.
       * @param identity_claim Exclusive claim preventing overlapping same-client ADD requests.
       */
      virtual_lease_t(
        driver_handle_t handle,
        GUID monitor_guid,
        std::string output_name,
        sudovda_identity_claim_t identity_claim
      ):
          m_identity_claim {std::move(identity_claim)},
          m_handle {std::move(handle)},
          m_monitor_guid {monitor_guid},
          m_output_name {std::move(output_name)} {
        sudovda_watchdog_t watchdog {};
        DWORD returned = 0;
        if (DeviceIoControl(
              m_handle.get(),
              IOCTL_GET_WATCHDOG,
              nullptr,
              0,
              &watchdog,
              sizeof(watchdog),
              &returned,
              nullptr
            ) && watchdog.timeout > 0) {
          const auto interval = std::chrono::milliseconds {watchdog.timeout * 1000 / 3};
          m_watchdog = std::jthread {[this, interval](std::stop_token stop_token) {
            std::unique_lock lock {m_watchdog_mutex};
            while (!stop_token.stop_requested()) {
              if (m_watchdog_cv.wait_for(lock, interval, [&]() {
                    return stop_token.stop_requested();
                  })) {
                return;
              }

              lock.unlock();
              DWORD ignored = 0;
              if (!DeviceIoControl(
                    m_handle.get(),
                    IOCTL_DRIVER_PING,
                    nullptr,
                    0,
                    nullptr,
                    0,
                    &ignored,
                    nullptr
                  )) {
                BOOST_LOG(warning) << "SudoVDA watchdog ping failed: "sv << GetLastError();
                return;
              }
              lock.lock();
            }
          }};
        }
      }

      virtual_lease_t(const virtual_lease_t &) = delete;
      virtual_lease_t &operator=(const virtual_lease_t &) = delete;

      /**
       * @brief Stop keepalives and remove the leased monitor.
       */
      ~virtual_lease_t() override {
        if (m_watchdog.joinable()) {
          m_watchdog.request_stop();
          m_watchdog_cv.notify_all();
          m_watchdog.join();
        }

        sudovda_remove_params_t params {m_monitor_guid};
        DWORD ignored = 0;
        if (!DeviceIoControl(
              m_handle.get(),
              IOCTL_REMOVE_VIRTUAL_DISPLAY,
              &params,
              sizeof(params),
              nullptr,
              0,
              &ignored,
              nullptr
            )) {
          BOOST_LOG(warning) << "Failed to remove the SudoVDA second display: "sv << GetLastError();
        }

        std::scoped_lock lock {existing_output_mutex};
        claimed_existing_outputs.erase(m_output_name);
      }

      /**
       * @brief Return the owned monitor's capture output name.
       *
       * @return Platform capture output name.
       */
      [[nodiscard]] std::string output_name() const override {
        return m_output_name;
      }

    private:
      sudovda_identity_claim_t m_identity_claim;  ///< Stable GUID claim retained until monitor removal.
      driver_handle_t m_handle;  ///< Driver handle retained for monitor lifetime.
      GUID m_monitor_guid;  ///< Identity used to remove the monitor.
      std::string m_output_name;  ///< GDI capture selector.
      std::mutex m_watchdog_mutex;  ///< Lock used by the stop-aware watchdog wait.
      std::condition_variable_any m_watchdog_cv;  ///< Wakes the watchdog during teardown.
      std::jthread m_watchdog;  ///< Driver keepalive thread, when enabled.
    };

    /**
     * @brief Probe for a creatable, attached, or safely activatable virtual display.
     *
     * @return True when SudoVDA is compatible or a recognized virtual output
     * is attached or can be activated by stable device identifier.
     */
    [[nodiscard]] bool virtual_display_available() {
      auto handle = open_sudovda();
      if (handle && compatible_sudovda(handle.get())) {
        return true;
      }
      return find_existing_virtual_output().has_value() || inactive_virtual_display_available();
    }

    /**
     * @brief Ask SudoVDA to create and attach a monitor for a client.
     *
     * @param request Requested client-panel mode.
     * @return Owning monitor lease, or null on driver or hotplug failure.
     */
    [[nodiscard]] std::unique_ptr<lease_t> acquire_sudovda_display(const request_t &request) {
      auto handle = open_sudovda();
      if (!handle) {
        return nullptr;
      }
      if (!compatible_sudovda(handle.get())) {
        BOOST_LOG(warning) << "Virtual second display requested, but the SudoVDA protocol is incompatible"sv;
        return nullptr;
      }

      sudovda_identity_claim_t identity_claim {request.client_id};
      if (!identity_claim) {
        BOOST_LOG(info) << "SudoVDA monitor identity is still owned by an overlapping client session"sv;
        return nullptr;
      }

      const auto monitor_guid = make_monitor_guid(request.client_id);
      sudovda_add_params_t params {
        static_cast<UINT>(request.width),
        static_cast<UINT>(request.height),
        static_cast<UINT>(request.framerate),
        monitor_guid,
        {},
        {}
      };
      std::snprintf(params.device_name, sizeof(params.device_name), "Sunshine DS");
      std::snprintf(
        params.serial_number,
        sizeof(params.serial_number),
        "%08lx",
        static_cast<unsigned long>(monitor_guid.Data1)
      );

      sudovda_add_result_t added {};
      DWORD returned = 0;
      if (!DeviceIoControl(
            handle.get(),
            IOCTL_ADD_VIRTUAL_DISPLAY,
            &params,
            sizeof(params),
            &added,
            sizeof(added),
            &returned,
            nullptr
          )) {
        BOOST_LOG(warning) << "SudoVDA failed to create a second display: "sv << GetLastError();
        return nullptr;
      }

      auto remove_created_monitor = [&]() {
        sudovda_remove_params_t remove {monitor_guid};
        DWORD ignored = 0;
        if (!DeviceIoControl(
              handle.get(),
              IOCTL_REMOVE_VIRTUAL_DISPLAY,
              &remove,
              sizeof(remove),
              nullptr,
              0,
              &ignored,
              nullptr
            )) {
          BOOST_LOG(warning) << "Failed to remove an unusable SudoVDA display: "sv << GetLastError();
        }
      };

      if (returned < sizeof(added)) {
        BOOST_LOG(warning) << "SudoVDA returned an incomplete add-monitor result"sv;
        remove_created_monitor();
        return nullptr;
      }

      const auto topology_result = SetDisplayConfig(
        0,
        nullptr,
        0,
        nullptr,
        SDC_TOPOLOGY_EXTEND | SDC_APPLY | SDC_ALLOW_CHANGES
      );
      if (topology_result != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SudoVDA monitor was created, but Windows could not apply Extend topology: "sv
                           << topology_result;
      }

      std::string output_name;
      auto retry_delay = 20ms;
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        output_name = added_display_name(added);
        if (!output_name.empty()) {
          break;
        }
        std::this_thread::sleep_for(retry_delay);
        retry_delay = std::min(retry_delay * 2, 500ms);
      }

      const auto primary = primary_output();
      if (output_name.empty() || output_name == primary) {
        if (output_name == primary) {
          BOOST_LOG(warning) << "SudoVDA target is cloned from the primary output; refusing a duplicate second stream"sv;
        } else {
          BOOST_LOG(warning) << "SudoVDA created a monitor, but Windows did not attach a capture output"sv;
        }
        remove_created_monitor();
        return nullptr;
      }

      {
        std::scoped_lock lock {existing_output_mutex};
        if (!claimed_existing_outputs.emplace(output_name).second) {
          BOOST_LOG(warning) << "SudoVDA output is already leased by an overlapping session: "sv << output_name;
          remove_created_monitor();
          return nullptr;
        }
      }

      BOOST_LOG(info) << "Second display: created and owns SudoVDA output "sv << output_name << " at "sv
                      << request.width << 'x' << request.height << '@' << request.framerate
                      << "; it will be removed when the second stream ends"sv;
      if (const auto capture_mode = current_output_mode(output_name)) {
        BOOST_LOG(info) << "Second display: negotiated SudoVDA capture mode "sv
                        << capture_mode->dmPelsWidth << 'x' << capture_mode->dmPelsHeight
                        << '@' << capture_mode->dmDisplayFrequency;
      }
      return std::make_unique<virtual_lease_t>(
        std::move(handle),
        monitor_guid,
        std::move(output_name),
        std::move(identity_claim)
      );
    }

    /**
     * @brief Acquire a managed, attached, or safely activatable virtual output.
     *
     * @param request Requested client-panel mode.
     * @return Display lease, or null when neither source is available.
     */
    [[nodiscard]] std::unique_ptr<lease_t> acquire_virtual_display(const request_t &request) {
      if (auto lease = acquire_sudovda_display(request)) {
        return lease;
      }

      BOOST_LOG(info) << "Second display: falling back to an already-active virtual display"sv;
      if (auto lease = acquire_existing_virtual_display(request)) {
        return lease;
      }

      BOOST_LOG(info) << "Second display: trying to activate a recognized detached virtual display"sv;
      if (auto lease = activate_existing_virtual_display(request)) {
        return lease;
      }

      BOOST_LOG(warning) << "Virtual second display requested, but neither SudoVDA nor a recognized virtual output is available"sv;
      return nullptr;
    }
#elif defined(__linux__)
    /**
     * @brief Linux DS owns at most one KWin virtual output.
     *
     * HDMI is the physical TV (also what Decky Sunshine captures). Azahar,
     * Cemu, and other dual-screen mods use exactly one `Virtual-sunshine-ds`.
     * The playbook helper holds that output for the life of the desktop
     * session. GameStream leases are names only — disconnect must not
     * SIGTERM the helper or spawn a second `--name sunshine-ds`.
     */
    constexpr auto kwin_virtual_helper_name = "sunshine-ds"sv;
    constexpr auto kwin_virtual_capture_name = "Virtual-sunshine-ds"sv;

    std::mutex virtual_output_mu;  ///< Serializes helper spawn so two clients cannot race.

    /**
     * @brief Search PATH for an executable name.
     */
    [[nodiscard]] std::string find_on_path(const std::string &exe) {
      const char *path = std::getenv("PATH");
      if (!path) {
        return {};
      }

      std::string remaining {path};
      while (!remaining.empty()) {
        auto sep = remaining.find(':');
        auto dir = remaining.substr(0, sep);
        remaining = sep == std::string::npos ? std::string {} : remaining.substr(sep + 1);
        if (dir.empty()) {
          continue;
        }
        auto candidate = dir + "/" + exe;
        if (access(candidate.c_str(), X_OK) == 0) {
          return candidate;
        }
      }
      return {};
    }

    /**
     * @brief Locate the KWin virtual-output helper (or krfb as a fallback).
     */
    [[nodiscard]] std::string virtual_output_helper_path() {
      if (const char *env = std::getenv("SUNSHINE_DS_VIRTUAL_HELPER")) {
        if (env[0] && access(env, X_OK) == 0) {
          return env;
        }
      }

      char self[4096];
      const auto n = readlink("/proc/self/exe", self, sizeof(self) - 1);
      if (n > 0) {
        self[n] = '\0';
        std::string dir {self};
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
          auto candidate = dir.substr(0, slash) + "/sunshine-ds-virtual-output";
          if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
          }
        }
      }

      if (auto found = find_on_path("sunshine-ds-virtual-output"); !found.empty()) {
        return found;
      }
      if (const char *env = std::getenv("KRFB_VIRTUALMONITOR")) {
        if (env[0] && access(env, X_OK) == 0) {
          return env;
        }
      }
      return find_on_path("krfb-virtualmonitor");
    }

    /**
     * @brief Helper `--name` for a `Virtual-*` capture output, or the name itself.
     */
    [[nodiscard]] std::string helper_name_for_output(std::string_view output) {
      constexpr auto prefix = "Virtual-"sv;
      if (output.rfind(prefix, 0) == 0) {
        return std::string {output.substr(prefix.size())};
      }
      return std::string {output};
    }

    /**
     * @brief True when `/proc` cmdline is the named virtual-output helper.
     *
     * Scan `/proc` directly. Do not shell out to `pgrep -f` — a command
     * line that contains `sunshine-ds-virtual-output` matches itself.
     */
    [[nodiscard]] bool cmdline_is_named_virtual_helper(const std::string &cmdline, std::string_view helper_name) {
      std::vector<std::string_view> args;
      for (std::size_t i = 0; i < cmdline.size();) {
        const auto n = cmdline.find('\0', i);
        const auto end = n == std::string::npos ? cmdline.size() : n;
        if (end > i) {
          args.emplace_back(cmdline.data() + i, end - i);
        }
        if (n == std::string::npos) {
          break;
        }
        i = n + 1;
      }
      if (args.empty()) {
        return false;
      }

      auto exe = args.front();
      const auto slash = exe.rfind('/');
      if (slash != std::string_view::npos) {
        exe = exe.substr(slash + 1);
      }
      if (exe != "sunshine-ds-virtual-output" && exe != "krfb-virtualmonitor") {
        return false;
      }

      for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--name" && args[i + 1] == helper_name) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Pids of live helpers holding `--name helper_name`.
     */
    [[nodiscard]] std::vector<pid_t> named_virtual_helper_pids(std::string_view helper_name) {
      std::vector<pid_t> pids;
      DIR *proc = opendir("/proc");
      if (!proc) {
        return pids;
      }

      const auto self = getpid();
      while (dirent *ent = readdir(proc)) {
        char *end = nullptr;
        const long pid_l = std::strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || pid_l <= 0 || static_cast<pid_t>(pid_l) == self) {
          continue;
        }

        const auto pid = static_cast<pid_t>(pid_l);
        std::ifstream in {"/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary};
        if (!in) {
          continue;
        }
        const std::string cmdline {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
        if (cmdline_is_named_virtual_helper(cmdline, helper_name)) {
          pids.push_back(pid);
        }
      }
      closedir(proc);
      return pids;
    }

    /**
     * @brief True when a helper already holds this virtual output name.
     */
    [[nodiscard]] bool virtual_helper_running(std::string_view helper_name) {
      return !named_virtual_helper_pids(helper_name).empty();
    }

    /**
     * @brief True when `needle` is an attached capture output.
     *
     * Do **not** call `kscreen-doctor`. Duplicate Virtual-* outputs make
     * it hang, and the hang looks like a missing display so a second helper
     * gets spawned. Prefer `/proc` helper detection: `platf::display_names`
     * opens a throwaway KWin connection and returns a dummy `""` while
     * still elevated, which used to spawn extras.
     */
    [[nodiscard]] bool output_is_attached(const std::string &needle) {
      if (needle.empty()) {
        return false;
      }

      const auto helper_name = helper_name_for_output(needle);
      if (virtual_helper_running(helper_name)) {
        return true;
      }

      const auto outputs = platf::display_names(platf::mem_type_e::system);
      return std::find(std::begin(outputs), std::end(outputs), needle) != std::end(outputs);
    }

    /**
     * @brief True when the singleton virtual GamePad output is already available.
     */
    [[nodiscard]] bool virtual_output_present(const std::string &output) {
      if (output.empty()) {
        return false;
      }
      return output_is_attached(output) || output_is_attached(helper_name_for_output(output));
    }

    [[nodiscard]] bool virtual_display_available() {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        return false;
      }
      return !virtual_output_helper_path().empty();
    }

    [[nodiscard]] bool helper_is_krfb(const std::string &binary) {
      return binary.find("krfb-virtualmonitor") != std::string::npos;
    }

#ifdef __linux__
    /**
     * @brief Close inherited IPv4/IPv6 sockets in the virtual-output child.
     *
     * glibc here has no POSIX_SPAWN_CLOEXEC_DEFAULT. The helper only needs
     * Wayland/DBus (AF_UNIX). If it inherits GameStream listen/UDP sockets and
     * sunshine-ds later dies, :48100 stays held and the next DS cannot bind.
     *
     * @param actions posix_spawn file actions to append close operations to.
     * @param keep_fd Descriptor to leave open in the child, or -1.
     */
    void addclose_inet_sockets(posix_spawn_file_actions_t *actions, int keep_fd) {
      DIR *dir = opendir("/proc/self/fd");
      if (!dir) {
        BOOST_LOG(warning) << "Could not scan fds before spawning virtual-output helper"sv;
        return;
      }
      const int dir_fd = dirfd(dir);
      while (dirent *ent = readdir(dir)) {
        char *end = nullptr;
        const long fd_l = strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || fd_l < 0) {
          continue;
        }
        const int fd = static_cast<int>(fd_l);
        if (fd <= STDERR_FILENO || fd == keep_fd || fd == dir_fd) {
          continue;
        }
        sockaddr_storage addr {};
        socklen_t len = sizeof(addr);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
          continue;
        }
        if (addr.ss_family == AF_INET || addr.ss_family == AF_INET6) {
          posix_spawn_file_actions_addclose(actions, fd);
        }
      }
      closedir(dir);
    }
#endif

    /**
     * @brief Lease the singleton virtual GamePad output.
     *
     * HDMI stays the physical TV. Linux DS never creates a second
     * `Virtual-*` and never SIGTERMs the helper when a stream ends.
     */
    [[nodiscard]] std::unique_ptr<lease_t> acquire_virtual_display(const request_t &request) {
      const std::string name {kwin_virtual_helper_name};
      const std::string capture_name {kwin_virtual_capture_name};
      std::lock_guard lock {virtual_output_mu};

      auto existing_pids = named_virtual_helper_pids(name);
      if (existing_pids.size() > 1) {
        BOOST_LOG(warning) << "Second display: "sv << existing_pids.size()
                           << " helpers already hold --name "sv << name
                           << "; capturing "sv << capture_name
                           << " without spawning another"sv;
      }
      if (!existing_pids.empty() || virtual_output_present(capture_name)) {
        BOOST_LOG(info) << "Second display: reusing "sv << capture_name
                        << " at "sv << request.width << 'x' << request.height
                        << '@' << request.framerate
                        << " (singleton helper; encoder will scale)"sv;
        return std::make_unique<physical_lease_t>(capture_name);
      }

      auto binary = virtual_output_helper_path();
      if (binary.empty()) {
        BOOST_LOG(warning) << "Virtual second display requested, but sunshine-ds-virtual-output is not installed"sv;
        return nullptr;
      }

      const auto width = std::to_string(request.width);
      const auto height = std::to_string(request.height);
      const auto resolution = width + "x" + height;
      const auto port = std::getenv("KRFB_VIRTUALMONITOR_PORT") ? std::getenv("KRFB_VIRTUALMONITOR_PORT") : "5910";
      const bool krfb = helper_is_krfb(binary);

      std::vector<std::string> args {binary};
      if (krfb) {
        args.insert(args.end(), {"--resolution", resolution, "--name", name, "--password", "sunshine-ds", "--scale", "1", "--port", port});
      } else {
        args.insert(args.end(), {"--name", name, "--width", width, "--height", height, "--scale", "1"});
      }

      std::vector<char *> argv;
      argv.reserve(args.size() + 1);
      for (auto &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      int logfd = open("/tmp/sunshine-ds-virtual-output.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (logfd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, logfd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, logfd, STDERR_FILENO);
      }

      posix_spawnattr_t spawn_attr;
      posix_spawnattr_init(&spawn_attr);
      addclose_inet_sockets(&actions, logfd);

      pid_t pid = -1;
      const int spawned = posix_spawn(&pid, binary.c_str(), &actions, &spawn_attr, argv.data(), environ);
      posix_spawnattr_destroy(&spawn_attr);
      posix_spawn_file_actions_destroy(&actions);
      if (logfd >= 0) {
        close(logfd);
      }
      if (spawned != 0) {
        BOOST_LOG(warning) << "Failed to spawn virtual-output helper: "sv << strerror(spawned);
        return nullptr;
      }

      BOOST_LOG(info) << "Second display: spawned singleton helper pid "sv << pid
                      << " for "sv << capture_name << " at "sv << resolution
                      << "; it stays up after clients disconnect"sv;

      auto retry_delay = 50ms;
      const auto deadline = std::chrono::steady_clock::now() + 8s;
      while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
          BOOST_LOG(warning) << "Virtual-output helper exited before the output appeared"sv;
          return nullptr;
        }
        if (virtual_output_present(capture_name)) {
          return std::make_unique<physical_lease_t>(capture_name);
        }
        std::this_thread::sleep_for(retry_delay);
        retry_delay = std::min(retry_delay * 2, 400ms);
      }

      // KWin 6.7 may never advertise the output while the helper still
      // holds it. Do not SIGTERM — capture can still bind the name.
      BOOST_LOG(warning) << "Virtual-output helper is running, but KWin has not advertised "sv
                         << capture_name << " yet; capturing that name anyway"sv;
      return std::make_unique<physical_lease_t>(capture_name);
    }

#else
    /**
     * @brief Report virtual-display availability on unsupported platforms.
     *
     * @return Always false outside Windows and Linux.
     */
    [[nodiscard]] bool virtual_display_available() {
      return false;
    }

    /**
     * @brief Refuse virtual-display acquisition on unsupported platforms.
     *
     * @param request Requested client-panel mode.
     * @return Always null outside Windows and Linux.
     */
    [[nodiscard]] std::unique_ptr<lease_t> acquire_virtual_display([[maybe_unused]] const request_t &request) {
      return nullptr;
    }
#endif

  }  // namespace

  bool valid_request(const request_t &request) {
    constexpr auto max_uint = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    return request.width > 0 && request.height > 0 && request.framerate > 0 &&
           static_cast<std::uint64_t>(request.width) <= max_uint &&
           static_cast<std::uint64_t>(request.height) <= max_uint &&
           static_cast<std::uint64_t>(request.framerate) <= max_uint;
  }

  bool supported() {
    const auto &source = config::video.dual_display_source;

    if (source.empty()) {
      return false;
    }

    if (source == VIRTUAL) {
      return virtual_display_available();
    }

    if (is_pipewire_second_display(source)) {
      return true;
    }

#ifdef __linux__
    // Named Virtual-* is the playbook GamePad output. Do not require
    // kwin_display_names: that returns a dummy "" while still elevated
    // and would hide dual-stream from /serverinfo.
    if (source.rfind("Virtual-", 0) == 0) {
      if (source != kwin_virtual_capture_name) {
        BOOST_LOG(warning) << "Second display: "sv << source
                           << " is not supported; Linux DS serves only "sv
                           << kwin_virtual_capture_name;
      }
      return virtual_display_available();
    }
#endif

    // A named monitor, which must actually be attached. The same output as
    // primary is allowed so both streams can capture HDMI while we bring the
    // virtual GamePad display back.
    const auto output = resolve_output(source);
    return !output.empty();
  }

  std::unique_ptr<lease_t> acquire(const request_t &request) {
    if (!valid_request(request)) {
      BOOST_LOG(warning) << "Client requested an invalid second-display mode"sv;
      return nullptr;
    }

    const auto &source = config::video.dual_display_source;

    if (source == VIRTUAL) {
      return acquire_virtual_display(request);
    }

    if (is_pipewire_second_display(source)) {
      BOOST_LOG(info) << "Second display: PipeWire "sv << source
                      << " at "sv << request.width << 'x' << request.height
                      << '@' << request.framerate
                      << " (headless gamescope; not KWin, not KMS)"sv;
      return std::make_unique<physical_lease_t>(std::string {source});
    }

#ifdef __linux__
    if (source.rfind("Virtual-", 0) == 0) {
      if (source != kwin_virtual_capture_name) {
        BOOST_LOG(warning) << "Second display: capturing "sv << kwin_virtual_capture_name
                           << " instead of "sv << source;
      }
      return acquire_virtual_display(request);
    }
#endif

    const auto output = resolve_output(source);
    if (output.empty()) {
      return nullptr;
    }

    if (output == primary_output()) {
      BOOST_LOG(info) << "Second display: capturing primary output "sv << output
                      << " again (duplicate stream) at "sv
                      << request.width << 'x' << request.height << '@' << request.framerate;
    } else {
      BOOST_LOG(info) << "Second display: capturing "sv << output << " at "sv
                      << request.width << 'x' << request.height << '@' << request.framerate;
    }
    return std::make_unique<physical_lease_t>(output);
  }

}  // namespace dual_display
