/**
 * @file src/platform/linux/pipewire.cpp
 * @brief Shared classes for pipewire-based capture methods.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <tuple>
#include <cerrno>

// lib includes
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <pipewire/link.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "src/main.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

#if !PW_CHECK_VERSION(1, 6, 0)
constexpr int SPA_VIDEO_TRANSFER_SMPTE2084 = 14;  ///< Protocol or platform constant for spa video transfer smpte2084.
#endif

#if PW_CHECK_VERSION(0, 3, 75)
// Runtime linked library version checks are available. Check for pipewire 0.3.64 which documented object serial support and deprecated node id.
const bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = pw_check_library_version(0, 3, 64);
#elifdef PW_KEY_TARGET_OBJECT
// Runtime linked library version checks are UNAVAILABLE but necessary PW_KEY_TARGET_OBJECT for object serial support is available.
constexpr bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = true;
#else
// Pipewire object serials are unsupported without PW_KEY_TARGET_OBJECT (we define it here so compilation won't break but don't use it).
constexpr bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = false;  ///< Whether PipeWire object serials should be used for matching.
  /**
   * @def PW_KEY_TARGET_OBJECT
   * @brief Macro for PW KEY TARGET OBJECT.
   */
  #define PW_KEY_TARGET_OBJECT "target.object"
#endif

namespace {
  // Buffer and limit constants
  constexpr int SPA_POD_BUFFER_SIZE = 4096;
  constexpr int MAX_PARAMS = 200;
  constexpr int MAX_DMABUF_FORMATS = 200;
  constexpr int MAX_DMABUF_MODIFIERS = 200;

  bool dma_buf_sync(int fd, uint64_t flags) {
    if (fd < 0) {
      return false;
    }
    struct dma_buf_sync sync {};
    sync.flags = flags;
    int ret = 0;
    do {
      ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret == -1 && (errno == EAGAIN || errno == EINTR));
    return ret == 0;
  }

  void wait_dmabuf_fence(int fd) {
    if (fd < 0) {
      return;
    }
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
    struct dma_buf_export_sync_file exp {};
    exp.flags = DMA_BUF_SYNC_READ;
    exp.fd = -1;
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0 && exp.fd >= 0) {
      struct pollfd pfd {};
      pfd.fd = exp.fd;
      pfd.events = POLLIN;
      poll(&pfd, 1, 100);
      close(exp.fd);
    }
#endif
  }

  // Wait for the producer GPU write, then start a CPU read. EXPORT_SYNC_FILE
  // is the explicit-sync wait; DMA_BUF_IOCTL_SYNC also flushes CPU caches.
  // Without this, mmap of KWin LINEAR DMA-BUF on AMD is all zeros.
  bool wait_dmabuf_readable(int fd) {
    wait_dmabuf_fence(fd);
    return dma_buf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  }

  void dma_buf_sync_end(int fd) {
    dma_buf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
  }

  size_t count_nonzero_samples(const uint8_t *p, size_t n) {
    if (!p || n == 0) {
      return 0;
    }
    const size_t chunk = std::min(n, static_cast<size_t>(4096));
    size_t z = 0;
    for (size_t i = 0; i < chunk; ++i) {
      z += p[i] != 0;
    }
    if (n > chunk * 2) {
      const auto *mid = p + (n / 2);
      for (size_t i = 0; i < chunk; ++i) {
        z += mid[i] != 0;
      }
      const auto *tail = p + (n - chunk);
      for (size_t i = 0; i < chunk; ++i) {
        z += tail[i] != 0;
      }
    }
    return z;
  }
}  // namespace

using namespace std::literals;

namespace pipewire {
  /**
   * @brief PipeWire SPA format mapped to Sunshine pixel format.
   */
  struct format_map_t {
    uint64_t fourcc;  ///< DRM fourcc pixel format.
    int32_t pw_format;  ///< Matching PipeWire SPA video format.
  };

  static constexpr std::array<format_map_t, 7> format_map = {{
    {DRM_FORMAT_XBGR2101010, SPA_VIDEO_FORMAT_xBGR_210LE},
    {DRM_FORMAT_BGRA1010102, SPA_VIDEO_FORMAT_ARGB_210LE},
    {DRM_FORMAT_RGBA1010102, SPA_VIDEO_FORMAT_ABGR_210LE},
    {DRM_FORMAT_ABGR2101010, SPA_VIDEO_FORMAT_RGBA_102LE},
    {DRM_FORMAT_ARGB2101010, SPA_VIDEO_FORMAT_BGRA_102LE},
    {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA},
    {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
  }};

  /**
   * @brief PipeWire capture state shared with callback threads.
   */
  struct shared_state_t {
    std::atomic<int> negotiated_width {0};  ///< Width negotiated with PipeWire for the stream.
    std::atomic<int> negotiated_height {0};  ///< Height negotiated with PipeWire for the stream.
    std::atomic<int> color_primaries {0};  ///< PipeWire color-primaries metadata for the stream.
    std::atomic<int> transfer_function {0};  ///< PipeWire transfer-function metadata for the stream.
    std::atomic<bool> stream_dead {false};  ///< Whether the PipeWire stream has been destroyed.
    pw_stream_state previous_state;  ///< Previous PipeWire stream state reported by callbacks.
    pw_stream_state current_state;  ///< Current PipeWire stream state reported by callbacks.
    std::string err_msg;  ///< Last PipeWire error message reported by the stream.
  };

  /**
   * @brief PipeWire stream handle, format, and shared state pointer.
   */
  struct stream_data_t {
    struct pw_stream *stream;  ///< PipeWire stream handle used for screencast frames.
    struct spa_hook stream_listener;  ///< Hook registering callbacks on the PipeWire stream.
    struct spa_video_info format;  ///< Negotiated PipeWire video format.
    struct pw_buffer *current_buffer;  ///< PipeWire buffer currently exposed to the capture thread.
    uint64_t drm_format;  ///< DRM format.
    std::shared_ptr<shared_state_t> shared;  ///< State shared between PipeWire callbacks and the capture backend.
    std::mutex frame_mutex;  ///< Synchronizes access to the current PipeWire frame.
    std::condition_variable frame_cv;  ///< Signals arrival or release of a PipeWire frame.
    size_t local_stride = 0;  ///< Local stride.
    bool frame_ready = false;  ///< Whether a PipeWire frame is ready to consume.
    // Two distinct memory pools
    std::vector<uint8_t> buffer_a;  ///< First staging buffer used for CPU-copy PipeWire frames.
    std::vector<uint8_t> buffer_b;  ///< Second staging buffer used for CPU-copy PipeWire frames.
    // Points to the buffer currently owned by fill_img
    std::vector<uint8_t> *front_buffer;  ///< Staging buffer currently readable by `fill_img`.
    // Points to the buffer currently being written by on_process
    std::vector<uint8_t> *back_buffer;  ///< Staging buffer currently writable by PipeWire callbacks.
    struct pw_core *core = nullptr;  ///< PipeWire core used to create a fallback capture link.
    uint32_t target_node = PW_ID_ANY;  ///< KWin screencast node id for an explicit link-factory fallback.
    bool link_requested = false;  ///< Whether an explicit capture link has already been requested.
    bool format_negotiated = false;  ///< Ports exist only after SPA_PARAM_Format.
    bool cpu_frame_valid = false;  ///< Whether `front_buffer` holds a copied CPU frame.
    int link_attempts = 0;  ///< link-factory tries; ports can lag the PAUSED state.
    enum pw_stream_state pw_state = PW_STREAM_STATE_UNCONNECTED;  ///< Last stream state callback.

    stream_data_t():
        front_buffer(&buffer_a),
        back_buffer(&buffer_b) {}
  };

  /**
   * @brief DMA-BUF format and modifier list advertised by PipeWire.
   */
  struct dmabuf_format_info_t {
    int32_t format;  ///< PipeWire SPA video format being advertised.
    uint64_t *modifiers;  ///< DRM format modifiers supported for the format.
    int n_modifiers;  ///< Number of entries in `modifiers`.
  };

  /**
   * @brief Pipewire image assembled for encoding.
   */
  struct img_descriptor_t: public egl::img_descriptor_t {
    ~img_descriptor_t() override {
      // Only free buffers this image actually owns. The memory-buffer capture
      // path points img->data at the PipeWire staging vector (front_buffer),
      // which is owned by pipewire_t -- deleting it here corrupts the heap.
      if (data && data_owned) {
        delete[] data;
      }
      data = nullptr;
      data_owned = false;
    }

    bool data_owned = false;  ///< Whether img->data is owned by this image and must be freed.
  };

  /**
   * @brief PipeWire core, context, and stream setup used for screencast capture.
   */
  class pipewire_t {
  public:
    pipewire_t():
        loop(pw_thread_loop_new("Pipewire thread", nullptr)) {
      BOOST_LOG(debug) << "[pipewire] Start PW thread loop"sv;
      pw_thread_loop_start(loop);
    }

    ~pipewire_t() {
      BOOST_LOG(debug) << "[pipewire] Destroying pipewire_t"sv;
      pw_thread_loop_lock(loop);

      // Lock the frame mutex to stop fill_img
      BOOST_LOG(debug) << "[pipewire] Stop fill_img"sv;
      {
        std::scoped_lock lock(stream_data.frame_mutex);
        stream_data.frame_ready = false;
        stream_data.current_buffer = nullptr;
      }

      // Release pipewire stream
      if (stream_data.stream) {
        BOOST_LOG(debug) << "[pipewire] Disconnect stream"sv;
        pw_stream_disconnect(stream_data.stream);
        BOOST_LOG(debug) << "[pipewire] Destroy stream"sv;
        pw_stream_destroy(stream_data.stream);
        stream_data.stream = nullptr;
      }
      // Release pipewire core
      if (core) {
        BOOST_LOG(debug) << "[pipewire] Disconnect PW core"sv;
        pw_core_disconnect(core);
        core = nullptr;
      }
      // Release pipewire context
      if (context) {
        BOOST_LOG(debug) << "[pipewire] Destroy PW context"sv;
        pw_context_destroy(context);
        context = nullptr;
      }
      // Release pipewire file descriptor
      if (fd >= 0) {
        BOOST_LOG(debug) << "[pipewire] Close pipewire_fd"sv;
        close(fd);
      }
      // Release pipewire thread loop
      BOOST_LOG(debug) << "[pipewire] Stop PW thread loop"sv;
      pw_thread_loop_unlock(loop);
      pw_thread_loop_stop(loop);
      BOOST_LOG(debug) << "[pipewire] Destroy PW thread loop"sv;
      pw_thread_loop_destroy(loop);
    }

    /**
     * @brief Return the mutex protecting PipeWire frame state.
     *
     * @return Mutex used by producer and capture threads.
     */
    std::mutex &frame_mutex() {
      return stream_data.frame_mutex;
    }

    /**
     * @brief Return the condition variable signaled when frame state changes.
     *
     * @return Condition variable used to wait for frames or shutdown.
     */
    std::condition_variable &frame_cv() {
      return stream_data.frame_cv;
    }

    /**
     * @brief Check whether frame ready.
     *
     * @return True when PipeWire has delivered a frame ready for capture.
     */
    bool is_frame_ready() const {
      return stream_data.frame_ready;
    }

    bool is_cpu_frame_valid() const {
      return stream_data.cpu_frame_valid;
    }

    void release_current_buffer() {
      pw_thread_loop_lock(loop);
      {
        std::scoped_lock lock(stream_data.frame_mutex);
        if (stream_data.current_buffer && stream_data.stream) {
          pw_stream_queue_buffer(stream_data.stream, stream_data.current_buffer);
          stream_data.current_buffer = nullptr;
        }
      }
      pw_thread_loop_unlock(loop);
    }

    /**
     * @brief Check and log whether the active session will require Sunshine to perform pacing.
     *
     * @param requested_framerate The framerate that we requested.
     * @param requested_delay The delay corresponding to the requested framerate.
     * @return True when Sunshine pacing is required.
     */
    bool is_pacing_required(AVRational requested_framerate, std::chrono::nanoseconds requested_delay) {
      AVRational negotiated_rate =
        {
          static_cast<int32_t>(stream_data.format.info.raw.max_framerate.num),
          static_cast<int32_t>(stream_data.format.info.raw.max_framerate.denom)
      };
      int rate_comparison = av_cmp_q(negotiated_rate, requested_framerate);
      bool variable_rate = negotiated_rate.num == 0 && negotiated_rate.den == 1;
      bool pacing_required = variable_rate || rate_comparison > 0;

      if (!variable_rate && rate_comparison < 0) {
        BOOST_LOG(warning)
          << "[pipewire] Sunshine frame pacing: disabled (negotiated rate lower than requested rate)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Sunshine frame pacing: "sv
                        << (pacing_required ? std::format("enabled ({}ms)", std::chrono::duration<double, std::milli>(requested_delay).count()) : "disabled (event-driven capture)");
      }

      return pacing_required;
    }

    /**
     * @brief Set frame ready.
     *
     * @param ready Whether the PipeWire frame is ready for capture.
     */
    void set_frame_ready(bool ready) {
      stream_data.frame_ready = ready;
    }

    /**
     * @brief Initialize PipeWire core objects and optional stream negotiation.
     *
     * @param stream_fd Stream fd.
     * @param stream_node Stream node.
     * @param stream_object_serial Stream object serial.
     * @param shared_state Shared state.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const int stream_fd, const uint32_t stream_node, const uint64_t stream_object_serial, std::shared_ptr<shared_state_t> shared_state) {
      fd = stream_fd;
      node = stream_node;
      object_serial = stream_object_serial;
      stream_data.shared = std::move(shared_state);

      pw_thread_loop_lock(loop);
      BOOST_LOG(debug) << "[pipewire] Setup PW context"sv;
      context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      if (context) {
        BOOST_LOG(debug) << "[pipewire] Connect PW context to fd"sv;
        if (fd >= 0) {
          core = pw_context_connect_fd(context, fd, nullptr, 0);
        } else {
          core = pw_context_connect(context, nullptr, 0);
        }
        if (core) {
          pw_core_add_listener(core, &core_listener, &core_events, &stream_data);
        } else {
          BOOST_LOG(debug) << "[pipewire] Failed to connect to PW core. Error: "sv << errno << "(" << strerror(errno) << ")"sv;
          return -1;
        }
      } else {
        BOOST_LOG(debug) << "[pipewire] Failed to setup PW context. Error: "sv << errno << "(" << strerror(errno) << ")"sv;
        return -1;
      }

      pw_thread_loop_unlock(loop);
      return 0;
    }

    /**
     * @brief Create the PipeWire stream if it is not already active.
     *
     * @param mem_type Mem type.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     * @param target_framerate Target framerate expressed as AVRational.
     * @param dmabuf_infos Dmabuf infos.
     * @param n_dmabuf_infos N dmabuf infos.
     * @param display_is_nvidia Display is nvidia.
     * @return 0 when the PipeWire stream is configured; nonzero on negotiation failure.
     */
    int ensure_stream(const platf::mem_type_e mem_type, const uint32_t width, const uint32_t height, const AVRational target_framerate, const struct dmabuf_format_info_t *dmabuf_infos, const int n_dmabuf_infos, const bool display_is_nvidia) {
      pw_thread_loop_lock(loop);
      int result = 0;
      if (!stream_data.stream) {
        if (!core) {
          BOOST_LOG(debug) << "[pipewire] PW core not available. Cannot ensure stream."sv;
          pw_thread_loop_unlock(loop);
          return -1;
        }

        struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);

        // pw_stream_new takes ownership of props. TARGET_OBJECT must be set first:
        // setting it afterwards is a no-op (or UAF), AUTOCONNECT to PW_ID_ANY then
        // never links, and capture encodes dummy_img() black frames.
        const bool have_serial = SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL && (object_serial & SPA_ID_INVALID) != SPA_ID_INVALID;
        const bool have_node = node != PW_ID_ANY;
        // PipeWire 1.4+ docs: TARGET_OBJECT must be object.serial or node.name.
        // Passing a node id as target_id overwrites that property and WirePlumber
        // looks up the wrong object, so the stream stays in "connecting".
        if (have_serial) {
          pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, object_serial);
        }

        BOOST_LOG(info) << "[pipewire] Create PW stream fd="sv << fd
                        << " node="sv << node
                        << " object_serial="sv << object_serial
                        << " target="sv << (have_serial ? "serial"sv : (have_node ? "node-link"sv : "none"sv));
        stream_data.stream = pw_stream_new(core, "Sunshine Video Capture", props);
        props = nullptr;
        stream_data.core = core;
        stream_data.target_node = have_node ? node : PW_ID_ANY;
        stream_data.link_requested = false;
        stream_data.format_negotiated = false;
        stream_data.link_attempts = 0;
        stream_data.pw_state = PW_STREAM_STATE_UNCONNECTED;
        pw_stream_add_listener(stream_data.stream, &stream_data.stream_listener, &stream_events, &stream_data);

        std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
        struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());

        int n_params = 0;
        std::array<const struct spa_pod *, MAX_PARAMS> params;

        // Prefer DMA-BUF whenever the compositor advertises modifiers — including
        // software encode. KWin's MemFd/BGRA fallback is empty from Distrobox
        // (MAP_BUFFERS leaves zeros), which encodes as skip:100% black frames.
        // Hybrid Intel+NVIDIA CUDA still skips DMA-BUF: those fds are Intel and
        // cannot be imported into CUDA.
        const bool use_dmabuf = n_dmabuf_infos > 0 &&
                                (mem_type != platf::mem_type_e::cuda || display_is_nvidia);
        BOOST_LOG(info) << "[pipewire] DMA-BUF offer="sv << (use_dmabuf ? "yes"sv : "no"sv)
                        << " formats="sv << n_dmabuf_infos
                        << " mem_type="sv << static_cast<int>(mem_type);
        if (use_dmabuf) {
          for (int i = 0; i < n_dmabuf_infos; i++) {
            auto format_param = build_format_parameter(&pod_builder, width, height, target_framerate, dmabuf_infos[i].format, dmabuf_infos[i].modifiers, dmabuf_infos[i].n_modifiers);
            params[n_params] = format_param;
            n_params++;
          }
        }

        // Add fallback for memptr
        for (const auto &fmt : format_map) {
          auto format_param = build_format_parameter(&pod_builder, width, height, target_framerate, fmt.pw_format, nullptr, 0);
          params[n_params] = format_param;
          n_params++;
        }

        // PW_ID_ANY + TARGET_OBJECT (object.serial). Do not wait here: encoder
        // probe and session start share this path, and a blocking wait made
        // Moonlight connect/disconnect stall for seconds.
        const auto flags = static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
        BOOST_LOG(info) << "[pipewire] Connect PW stream PW_ID_ANY serial="sv << object_serial;
        result = pw_stream_connect(stream_data.stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params.data(), n_params);
        if (result < 0) {
          BOOST_LOG(error) << "[pipewire] pw_stream_connect failed: "sv << result << " ("sv << strerror(-result) << ")"sv;
        }
      }

      pw_thread_loop_unlock(loop);
      return result;
    }

    /**
     * @brief Close img fds.
     *
     * @param img_descriptor Image descriptor whose duplicated DMA-BUF fds are closed.
     */
    static void close_img_fds(egl::img_descriptor_t *img_descriptor) {
      for (int &fd : img_descriptor->sd.fds) {
        if (fd >= 0) {
          close(fd);
          fd = -1;
        }
      }
    }

    /**
     * @brief Copy PipeWire metadata into the Sunshine image descriptor.
     *
     * @param img_descriptor Image descriptor receiving timestamps, sequence, and damage flags.
     * @param buf Raw byte buffer used for serialization.
     */
    static void fill_img_metadata(egl::img_descriptor_t *img_descriptor, struct spa_buffer *buf) {
      img_descriptor->frame_timestamp = std::chrono::steady_clock::now();

      struct spa_meta_header *h = static_cast<struct spa_meta_header *>(
        spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h))
      );
      if (h) {
        img_descriptor->seq = h->seq;
        img_descriptor->pts = h->pts;
      }

      if (buf->n_datas > 0) {
        img_descriptor->pw_flags = buf->datas[0].chunk->flags;
      }

      struct spa_meta_region *damage = static_cast<struct spa_meta_region *>(
        spa_buffer_find_meta_data(buf, SPA_META_VideoDamage, sizeof(*damage))
      );
      img_descriptor->pw_damage = (damage && damage->region.size.width > 0 && damage->region.size.height > 0) ? std::optional<bool>(true) : std::nullopt;
    }

    /**
     * @brief Populate a Sunshine image descriptor from PipeWire DMA-BUF planes.
     *
     * @param img_descriptor Image descriptor receiving duplicated fds and plane layout.
     * @param buf Raw byte buffer used for serialization.
     * @param d PipeWire listener data passed to the callback.
     */
    static void fill_img_dmabuf(egl::img_descriptor_t *img_descriptor, struct spa_buffer *buf, const stream_data_t &d) {
      img_descriptor->sd.width = d.format.info.raw.size.width;
      img_descriptor->sd.height = d.format.info.raw.size.height;
      img_descriptor->sd.modifier = d.format.info.raw.modifier;
      img_descriptor->sd.fourcc = d.drm_format;
      int plane = 0;
      for (uint32_t i = 0; i < buf->n_datas && plane < 4; ++i) {
        if (buf->datas[i].type != SPA_DATA_DmaBuf || buf->datas[i].fd < 0) {
          continue;
        }
        img_descriptor->sd.fds[plane] = dup(buf->datas[i].fd);
        img_descriptor->sd.pitches[plane] = buf->datas[i].chunk ? buf->datas[i].chunk->stride : 0;
        img_descriptor->sd.offsets[plane] = buf->datas[i].chunk ? buf->datas[i].chunk->offset : 0;
        ++plane;
      }
    }

    /**
     * @brief Copy the latest PipeWire frame into Sunshine's image buffer.
     *
     * @param img Image or frame object to read from or populate.
     */
    void fill_img(platf::img_t *img) {
      pw_thread_loop_lock(loop);
      std::scoped_lock lock(stream_data.frame_mutex);

      if (stream_data.shared && stream_data.shared->stream_dead.load()) {
        close_img_fds(static_cast<egl::img_descriptor_t *>(img));
        pw_thread_loop_unlock(loop);
        return;
      }

      auto *img_descriptor = static_cast<img_descriptor_t *>(img);

      // CPU staging first so software encode has pixels even when we also
      // attach DMA-BUF fds for hardware encode.
      if (stream_data.cpu_frame_valid && stream_data.front_buffer && !stream_data.front_buffer->empty() && img->data) {
        const auto src_stride = stream_data.local_stride > 0 ? stream_data.local_stride : static_cast<size_t>(img->row_pitch);
        const auto dst_stride = static_cast<size_t>(img->row_pitch);
        const auto copy_w = std::min(src_stride, dst_stride);
        const auto rows = std::max(img->height, 0);
        const auto *src = stream_data.front_buffer->data();
        const auto src_size = stream_data.front_buffer->size();
        for (int y = 0; y < rows; ++y) {
          const auto src_off = static_cast<size_t>(y) * src_stride;
          if (src_off + copy_w > src_size) {
            break;
          }
          std::memcpy(img->data + static_cast<size_t>(y) * dst_stride, src + src_off, copy_w);
        }
        img_descriptor->pixel_pitch = (stream_data.format.info.raw.format == SPA_VIDEO_FORMAT_NV12) ? 1 : 4;
      }

      if (stream_data.current_buffer && stream_data.current_buffer->buffer->datas[0].type == SPA_DATA_DmaBuf) {
        struct spa_buffer *buf = stream_data.current_buffer->buffer;
        if (buf->datas[0].chunk && buf->datas[0].chunk->size != 0) {
          fill_img_metadata(img_descriptor, buf);
          fill_img_dmabuf(img_descriptor, buf, stream_data);
        }
      }

      pw_thread_loop_unlock(loop);
    }

    /**
     * @brief Set negotiate maxframerate.
     *
     * @param negotiate_maxframerate Negotiate maxframerate.
     */
    void set_negotiate_maxframerate(bool negotiate_maxframerate) {
      negotiate_maxframerate_ = negotiate_maxframerate;
    }

  private:
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct stream_data_t stream_data;
    int fd;
    uint32_t node;
    uint64_t object_serial;
    bool negotiate_maxframerate_ = true;

    struct spa_pod *build_format_parameter(struct spa_pod_builder *b, uint32_t width, uint32_t height, AVRational target_framerate, int32_t format, uint64_t *modifiers, int n_modifiers) {
      struct spa_pod_frame object_frame;
      struct spa_pod_frame modifier_frame;
      std::array<struct spa_rectangle, 3> sizes;
      std::array<struct spa_fraction, 3> framerates;

      sizes[0] = SPA_RECTANGLE(width, height);  // Preferred
      sizes[1] = SPA_RECTANGLE(1, 1);
      sizes[2] = SPA_RECTANGLE(8192, 4096);

      framerates[0] = SPA_FRACTION(uint32_t(target_framerate.num), uint32_t(target_framerate.den));  // default/preferred
      framerates[1] = SPA_FRACTION(0, 1);  // min
      framerates[2] = SPA_FRACTION(1000, 1);  // max

      spa_pod_builder_push_object(b, &object_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
      spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sizes[0], &sizes[1], &sizes[2]), 0);
      if (negotiate_maxframerate_) {
        // Always request variable rate (0, 1) for framerate when populating maxFramerate with default,min,max values
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerates[1]), 0);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&framerates[0], &framerates[1], &framerates[2]), 0);
      } else {
        // Request target framerate (target_framerate) for framerate in fallback case
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerates[0]), 0);
      }

      if (format == SPA_VIDEO_FORMAT_xBGR_210LE) {
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_Id(SPA_VIDEO_COLOR_PRIMARIES_BT2020), 0);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_transferFunction, SPA_POD_Id(SPA_VIDEO_TRANSFER_SMPTE2084), 0);
      }

      if (n_modifiers) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);

        // Preferred value, we pick the first modifier be the preferred one
        spa_pod_builder_long(b, modifiers[0]);
        for (uint32_t i = 0; i < n_modifiers; i++) {
          spa_pod_builder_long(b, modifiers[i]);
        }

        spa_pod_builder_pop(b, &modifier_frame);
      }

      return static_cast<struct spa_pod *>(spa_pod_builder_pop(b, &object_frame));
    }

    static void on_core_info_cb([[maybe_unused]] void *user_data, const struct pw_core_info *pw_info) {
      BOOST_LOG(info) << "[pipewire] Connected to pipewire version "sv << pw_info->version;
    }

    static void on_core_error_cb(void *user_data, const uint32_t id, const int seq, [[maybe_unused]] int res, const char *message) {
      BOOST_LOG(info) << "[pipewire] Pipewire Error, id:"sv << id << " seq:"sv << seq << " message: "sv << message;
      auto *d = static_cast<stream_data_t *>(user_data);
      if (d && message && (std::string_view(message).find("unknown input port") != std::string_view::npos ||
                           std::string_view(message).find("unknown output port") != std::string_view::npos)) {
        // Ports lag PAUSED; STREAMING and the connect wait loop retry.
        d->link_requested = false;
      }
    }

    constexpr static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .info = on_core_info_cb,
      .error = on_core_error_cb,
    };

    /**
     * @brief Link the KWin screencast node to this capture stream.
     *
     * WirePlumber AUTOCONNECT cannot see KWin nodes with object.register=false,
     * and a broken session manager leaves the stream in "connecting" forever.
     * link-factory talks to the daemon directly.
     */
    static void ensure_capture_link(stream_data_t *d) {
      if (!d || !d->core || !d->stream) {
        return;
      }
      if (d->link_requested) {
        return;
      }
      if (d->link_attempts >= 8) {
        BOOST_LOG(error) << "[pipewire] capture link gave up after "sv << d->link_attempts << " tries"sv;
        return;
      }
      if (d->target_node == PW_ID_ANY) {
        BOOST_LOG(warning) << "[pipewire] capture link skipped: no KWin node id"sv;
        return;
      }
      // Do not wait for STREAMING: that state needs a link, so gating on it
      // deadlocks when AUTOCONNECT cannot see the KWin node.
      const uint32_t self_id = pw_stream_get_node_id(d->stream);
      if (self_id == SPA_ID_INVALID || self_id == PW_ID_ANY || self_id == 0) {
        BOOST_LOG(info) << "[pipewire] capture link waiting for stream node id (target="sv << d->target_node << ")"sv;
        return;
      }
      char out_id[16];
      char in_id[16];
      std::snprintf(out_id, sizeof(out_id), "%u", d->target_node);
      std::snprintf(in_id, sizeof(in_id), "%u", self_id);
      struct pw_properties *link_props = pw_properties_new(
        PW_KEY_LINK_OUTPUT_NODE, out_id,
        PW_KEY_LINK_INPUT_NODE, in_id,
        nullptr
      );
      BOOST_LOG(info) << "[pipewire] Creating capture link "sv << d->target_node << " -> "sv << self_id
                      << " (state="sv << pw_stream_state_as_string(d->pw_state) << ")"sv;
      pw_core_create_object(d->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &link_props->dict, 0);
      pw_properties_free(link_props);
      d->link_requested = true;
      d->link_attempts++;
    }

    static void on_stream_state_changed(void *user_data, enum pw_stream_state old, enum pw_stream_state state, const char *err_msg) {
      if (err_msg != nullptr) {
        BOOST_LOG(info) << "[pipewire] PipeWire stream error '" << err_msg << "' on state: " << pw_stream_state_as_string(old)
                        << " -> " << pw_stream_state_as_string(state);
      } else {
        BOOST_LOG(info) << "[pipewire] PipeWire stream state: " << pw_stream_state_as_string(old)
                        << " -> " << pw_stream_state_as_string(state);
      }

      auto *d = static_cast<stream_data_t *>(user_data);
      d->pw_state = state;
      if (state == PW_STREAM_STATE_STREAMING) {
        // AUTOCONNECT already linked. Do not create a second link-factory
        // object: it fails with "unknown input/output port (null)" and can
        // leave KWin producing only the first (cleared) buffer.
        d->link_requested = true;
      }

      switch (state) {
        case PW_STREAM_STATE_PAUSED:
          if (d->shared && old == PW_STREAM_STATE_STREAMING) {
            {
              std::scoped_lock lock(d->frame_mutex);
              d->frame_ready = false;
              d->current_buffer = nullptr;
              d->shared->stream_dead.store(true);
              d->shared->current_state = state;
              d->shared->previous_state = old;
              d->shared->err_msg = "";
            }
            d->frame_cv.notify_all();
          }
          break;
        case PW_STREAM_STATE_ERROR:
          {
            std::scoped_lock lock(d->frame_mutex);
            d->shared->current_state = state;
            d->shared->previous_state = old;
            d->shared->err_msg = std::string(err_msg);
          }
          [[fallthrough]];
        case PW_STREAM_STATE_UNCONNECTED:
          if (d->shared) {
            d->shared->stream_dead.store(true);
            d->frame_cv.notify_all();
          }
          break;
        default:
          break;
      }
    }

    static void on_process(void *user_data) {
      const auto d = static_cast<struct stream_data_t *>(user_data);
      struct pw_buffer *b = nullptr;

      // 1. Drain the queue: Always grab the most recent buffer
      while (struct pw_buffer *aux = pw_stream_dequeue_buffer(d->stream)) {
        if (b) {
          pw_stream_queue_buffer(d->stream, b);  // Return the older, unused buffer
        }
        b = aux;
      }

      if (!b) {
        return;
      }

      struct spa_data *d0 = &b->buffer->datas[0];

      // 2. Fast Path: DMA-BUF
      // 2. Fast Path: DMA-BUF. Keep the pw_buffer for hardware encode, and
      // also stage a CPU copy when the fd is mmap-able (linear 1920x1080 BGRA
      // is 8294400 bytes / stride 7680). Distrobox EGL import of host KWin
      // buffers is unreliable; mmap still works for LINEAR.
      if (d0->type == SPA_DATA_DmaBuf) {
        size_t size = d0->chunk ? d0->chunk->size : 0;
        size_t offset = d0->chunk ? d0->chunk->offset : 0;
        uint8_t *src = static_cast<uint8_t *>(d0->data);
        void *mapped = nullptr;
        size_t map_size = 0;
        bool synced = false;
        if (src == nullptr && d0->fd >= 0 && size > 0) {
          map_size = d0->maxsize > 0 ? d0->maxsize : (offset + size);
          synced = wait_dmabuf_readable(d0->fd);
          mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, d0->fd, static_cast<off_t>(d0->mapoffset));
          if (mapped == MAP_FAILED) {
            mapped = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, d0->fd, static_cast<off_t>(d0->mapoffset));
          }
          if (mapped == MAP_FAILED) {
            static std::atomic<int> mmap_fail {0};
            if (mmap_fail.fetch_add(1) < 4) {
              BOOST_LOG(info) << "[pipewire] dma-buf mmap failed fd="sv << d0->fd
                              << " maxsize="sv << d0->maxsize << " mapoffset="sv << d0->mapoffset
                              << " : "sv << strerror(errno);
            }
            mapped = nullptr;
          } else {
            src = static_cast<uint8_t *>(mapped) + offset;
          }
        }
        bool cpu_copied = false;
        size_t sampled = 0;
        if (src != nullptr && size > 0) {
          if (d->back_buffer->size() < size) {
            d->back_buffer->resize(size);
          }
          std::memcpy(d->back_buffer->data(), src, size);
          cpu_copied = true;
        }
        if (synced) {
          dma_buf_sync_end(d0->fd);
        }
        if (mapped != nullptr) {
          munmap(mapped, map_size);
          mapped = nullptr;
        }
        {
          std::scoped_lock lock(d->frame_mutex);
          if (d->current_buffer) {
            pw_stream_queue_buffer(d->stream, d->current_buffer);
          }
          if (cpu_copied) {
            std::swap(d->front_buffer, d->back_buffer);
            d->local_stride = d0->chunk ? d0->chunk->stride : 0;
            d->cpu_frame_valid = true;
            // Return the pw_buffer immediately so KWin can produce the next
            // frame. Holding it made probe attempts 1-3 time out, and the
            // later EGL import of an unsynced DMA-BUF overwrote pixels with zeros.
            d->current_buffer = nullptr;
            sampled = count_nonzero_samples(d->front_buffer->data(), d->front_buffer->size());
          } else {
            d->current_buffer = b;
          }
          d->frame_ready = true;
        }
        if (cpu_copied) {
          pw_stream_queue_buffer(d->stream, b);
        }
        static std::atomic<int> dma {0};
        const int n = dma.fetch_add(1);
        if (n < 8) {
          BOOST_LOG(info) << "[pipewire] dma-buf fd="sv << d0->fd
                          << " size="sv << size << " stride="sv << (d0->chunk ? d0->chunk->stride : 0)
                          << " modifier="sv << d->format.info.raw.modifier
                          << " mmap="sv << (cpu_copied ? "yes"sv : "no"sv)
                          << " synced="sv << synced
                          << " nonzero_sampled="sv << sampled << " n="sv << n;
        }
      }
      // 3. CPU path: MemPtr, or MemFd that MAP_BUFFERS did not map (common in Distrobox).
      else {
        size_t size = d0->chunk ? d0->chunk->size : 0;
        size_t offset = d0->chunk ? d0->chunk->offset : 0;
        uint8_t *src = static_cast<uint8_t *>(d0->data);
        void *mapped = nullptr;
        size_t map_size = 0;
        if (src == nullptr && d0->fd >= 0 && (d0->type == SPA_DATA_MemFd || d0->type == SPA_DATA_MemPtr)) {
          map_size = d0->maxsize > 0 ? d0->maxsize : (offset + size);
          mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, d0->fd, static_cast<off_t>(d0->mapoffset));
          if (mapped == MAP_FAILED) {
            mapped = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, d0->fd, static_cast<off_t>(d0->mapoffset));
          }
          if (mapped == MAP_FAILED) {
            BOOST_LOG(error) << "[pipewire] mmap capture fd failed type="sv << d0->type
                             << " fd="sv << d0->fd << " maxsize="sv << d0->maxsize
                             << " : "sv << strerror(errno);
            mapped = nullptr;
          } else {
            src = static_cast<uint8_t *>(mapped) + offset;
          }
        }
        if (src != nullptr && size > 0) {
          if (d->back_buffer->size() < size) {
            d->back_buffer->resize(size);
          }
          std::memcpy(d->back_buffer->data(), src, size);
          {
            std::scoped_lock lock(d->frame_mutex);
            std::swap(d->front_buffer, d->back_buffer);
            d->local_stride = d0->chunk ? d0->chunk->stride : 0;
            d->cpu_frame_valid = true;
            d->current_buffer = nullptr;
            d->frame_ready = true;
          }
          static std::atomic<int> copied {0};
          const int n = copied.fetch_add(1);
          if (n < 8) {
            size_t nonzero = 0;
            const auto *p = d->front_buffer->data();
            const auto ncheck = std::min(d->front_buffer->size(), size);
            for (size_t i = 0; i < ncheck; ++i) {
              nonzero += p[i] != 0;
            }
            BOOST_LOG(info) << "[pipewire] cpu frame type="sv << d0->type
                            << " size="sv << size << " stride="sv << (d0->chunk ? d0->chunk->stride : 0)
                            << " nonzero="sv << nonzero << "/"sv << ncheck << " n="sv << n;
          }
          pw_stream_queue_buffer(d->stream, b);
        } else {
          static std::atomic<int> dropped {0};
          const int n = dropped.fetch_add(1);
          if (n < 8 || n % 120 == 0) {
            BOOST_LOG(warning) << "[pipewire] dropped buffer type="sv << d0->type
                               << " size="sv << size << " fd="sv << d0->fd
                               << " data="sv << static_cast<const void *>(d0->data)
                               << " n="sv << n;
          }
          pw_stream_queue_buffer(d->stream, b);
        }
        if (mapped != nullptr && mapped != MAP_FAILED) {
          munmap(mapped, map_size);
        }
      }

      d->frame_cv.notify_one();
    }

    static void on_param_changed(void *user_data, uint32_t id, const struct spa_pod *param) {
      const auto d = static_cast<struct stream_data_t *>(user_data);

      d->current_buffer = nullptr;

      if (param == nullptr || id != SPA_PARAM_Format) {
        return;
      }
      if (spa_format_parse(param, &d->format.media_type, &d->format.media_subtype) < 0) {
        return;
      }
      if (d->format.media_type != SPA_MEDIA_TYPE_video || d->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
      }
      if (spa_format_video_raw_parse(param, &d->format.info.raw) < 0) {
        return;
      }

      BOOST_LOG(info) << "[pipewire] Video format: "sv << d->format.info.raw.format;
      BOOST_LOG(info) << "[pipewire] Size: "sv << d->format.info.raw.size.width << "x"sv << d->format.info.raw.size.height;
      BOOST_LOG(info) << "[pipewire] Color primaries: "sv << d->format.info.raw.color_primaries;
      BOOST_LOG(info) << "[pipewire] Transfer function: "sv << d->format.info.raw.transfer_function;
      if (d->format.info.raw.max_framerate.num == 0 && d->format.info.raw.max_framerate.denom == 1) {
        BOOST_LOG(info) << "[pipewire] Compositor negotiated frame rate: 0/1 (variable rate capture)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Compositor negotiated frame rate: "sv
                        << d->format.info.raw.framerate.num << "/"sv << d->format.info.raw.framerate.denom
                        << ", max: "sv << d->format.info.raw.max_framerate.num << "/"sv << d->format.info.raw.max_framerate.denom;
      }

      int physical_w = d->format.info.raw.size.width;
      int physical_h = d->format.info.raw.size.height;

      if (d->shared) {
        int old_w = d->shared->negotiated_width.load();
        int old_h = d->shared->negotiated_height.load();
        int old_color_primaries = d->shared->color_primaries.load();
        int old_transfer_function = d->shared->transfer_function.load();

        if (physical_w != old_w || physical_h != old_h) {
          d->shared->negotiated_width.store(physical_w);
          d->shared->negotiated_height.store(physical_h);
        }

        if (d->format.info.raw.color_primaries != old_color_primaries || d->format.info.raw.transfer_function != old_transfer_function) {
          d->shared->color_primaries.store(d->format.info.raw.color_primaries);
          d->shared->transfer_function.store(d->format.info.raw.transfer_function);
        }
      }

      uint64_t drm_format = 0;
      for (const auto &fmt : format_map) {
        if (fmt.pw_format == d->format.info.raw.format) {
          drm_format = fmt.fourcc;
        }
      }
      d->drm_format = drm_format;

      uint32_t buffer_types = 0;
      if (spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr && d->drm_format) {
        BOOST_LOG(info) << "[pipewire] using DMA-BUF buffers"sv;
        buffer_types |= 1 << SPA_DATA_DmaBuf;
      } else {
        BOOST_LOG(info) << "[pipewire] using memory buffers"sv;
        buffer_types |= (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd);
      }

      // Ack the buffer type and metadata
      std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
      std::array<const struct spa_pod *, 3> params;
      int n_params = 0;
      struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
      auto buffer_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types)));
      params[n_params] = buffer_param;
      n_params++;
      auto meta_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))));
      params[n_params] = meta_param;
      n_params++;
      int videoDamageRegionCount = 16;
      auto damage_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * videoDamageRegionCount, sizeof(struct spa_meta_region) * 1, sizeof(struct spa_meta_region) * videoDamageRegionCount)));
      params[n_params] = damage_param;
      n_params++;

      pw_stream_update_params(d->stream, params.data(), n_params);
      d->format_negotiated = true;
    }

    constexpr static const struct pw_stream_events stream_events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = on_stream_state_changed,
      .param_changed = on_param_changed,
      .process = on_process,
    };
  };

  /**
   * @brief Display capture backend that consumes frames from a PipeWire stream.
   */
  class pipewire_display_t: public platf::display_t {
  public:
    /**
     * @brief Initialize pipewire and check hwdevice type.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @return True when PipeWire is initialized and the hardware device type is supported.
     */
    static bool init_pipewire_and_check_hwdevice_type(platf::mem_type_e hwdevice_type) {
      // Initialize pipewire to load necessary modules
      pw_init(nullptr, nullptr);

      // Check if we have a matching hwdevice_type
      switch (hwdevice_type) {
        using enum platf::mem_type_e;
        case system:
        case vaapi:
        case cuda:
        case vulkan:
          return true;
        default:
          return false;
      }
    }

    /**
     *  @brief Configure the pipewire stream
     *  @param display_name provide a stream for this display_name
     *  @param out_pipewire_fd set to the pipewire fd for the stream during function call (or -1 for using the local context)
     *  @param out_pipewire_node set to the pipewire node of the stream during function call (or PW_ID_ANY to refer to object_serial)
     *  @param out_pipewire_objectserial set the pipewire object serial of the stream during function call
     *  @returns 0 if the stream successfully configured
     */
    virtual int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) = 0;

    /**
     *  @brief Verify and update display parameters for logical dimensions, desktop dimensions and logical desktop dimensions (default is adapted from wlgrab)
     */
    virtual void verify_and_update_display_parameters() {
      // Query outputs directly using wayland wl::monitors()
      if (logical_height <= 0 || logical_width <= 0 || env_logical_height <= 0 || env_logical_width <= 0 || env_height <= 0 || env_width <= 0) {
        int desktop_width = 0;
        int desktop_height = 0;
        int desktop_logical_width = 0;
        int desktop_logical_height = 0;
        for (const auto &monitor : wl::monitors()) {
          BOOST_LOG(debug) << "[pipewire] Found output: '"sv << monitor->name << "' offset: "sv << monitor->viewport.offset_x << 'x' << monitor->viewport.offset_y << " resolution: "sv << monitor->viewport.width << 'x' << monitor->viewport.height << " logical resolution: "sv << monitor->viewport.logical_width << 'x' << monitor->viewport.logical_height;
          // If logical_width and logical_height are not valid try to update them to correct values by matching to monitor
          // position/dimension or position/logical dimensions here since we're iterating for maximum environment size anyway
          if ((logical_width <= 0 || logical_height <= 0) && monitor->viewport.offset_x == offset_x && monitor->viewport.offset_y == offset_y && ((monitor->viewport.width == width && monitor->viewport.height == height) || (monitor->viewport.logical_width == width && monitor->viewport.logical_height == height))) {
            this->logical_width = monitor->viewport.logical_width;
            this->logical_height = monitor->viewport.logical_height;
            BOOST_LOG(debug) << "[pipewire] Set logical resolution: "sv << logical_width << 'x' << logical_height;
          }
          // Update desktop dimensions to setup maximum environment size over all screens
          desktop_width = std::max(desktop_width, monitor->viewport.offset_x + monitor->viewport.width);
          desktop_height = std::max(desktop_height, monitor->viewport.offset_y + monitor->viewport.height);
          // Update desktop logical dimensions to setup maximum logical environment size over all screens
          desktop_logical_width = std::max(desktop_logical_width, monitor->viewport.offset_x + monitor->viewport.logical_width);
          desktop_logical_height = std::max(desktop_logical_height, monitor->viewport.offset_y + monitor->viewport.logical_height);
        }
        if (env_height <= 0 || env_width <= 0) {
          this->env_width = desktop_width;
          this->env_height = desktop_height;
          BOOST_LOG(debug) << "[pipewire] Set desktop resolution: "sv << env_width << 'x' << env_height;
        }
        if (env_logical_height <= 0 || env_logical_width <= 0) {
          this->env_logical_width = desktop_logical_width;
          this->env_logical_height = desktop_logical_height;
          BOOST_LOG(debug) << "[pipewire] Set desktop logical resolution: "sv << env_logical_width << 'x' << env_logical_height;
        }
      }
    }

    /**
     * @brief Initialize the PipeWire display backend for a selected stream.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      // calculate frame interval we should capture at
      delay = ::video::capture_frame_interval(config);

      // WORKAROUND: if the active compositor is KWin, request variable rate (0, 1) capture only for versions 5.x-6.7.x.
      // Ref: https://bugs.kde.org/show_bug.cgi?id=524129
      // Also negotiate variable rate for all other compositors. Mutter's variable rate pacing is superior.
      const static std::vector<int> kwin_version = get_running_kwin_version();
      const static bool negotiate_variable_rate = kwin_version.empty() || (kwin_version[0] == 5 || (kwin_version[0] == 6 && kwin_version[1] < 8));

      const AVRational fps = (negotiate_variable_rate ? AVRational {0, 1} : ::video::framerate_to_rational(config));
      if (fps.den != 1) {
        BOOST_LOG(info) << "[pipewire] Requested frame rate: "sv << fps.num << "/"sv << fps.den << ", approx. "sv << av_q2d(fps) << " fps"sv;
      } else if (fps.num == 0 && fps.den == 1) {
        BOOST_LOG(info) << "[pipewire] Requested variable frame rate (Sunshine pacing required: "sv << std::chrono::duration<double, std::milli>(delay).count() << "ms)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Requested frame rate: "sv << fps.num << "fps"sv;
      }
      this->target_framerate = fps;
      mem_type = hwdevice_type;

      if (get_dmabuf_modifiers() < 0) {
        return -1;
      }

      int pipewire_fd = -1;
      auto pipewire_node = PW_ID_ANY;  // Default for invalid stream from pipewire docs
      uint64_t pipewire_object_serial = SPA_ID_INVALID;  // Default for invalid stream from pipewire docs for PW_KEY_OBJECT_SERIAL
      // Fetch stream info
      if (configure_stream(display_name, pipewire_fd, pipewire_node, pipewire_object_serial) < 0 || (pipewire_node == PW_ID_ANY && (pipewire_object_serial & SPA_ID_INVALID) == SPA_ID_INVALID)) {
        BOOST_LOG(error) << "[pipewire] Could not find display with name: '"sv << display_name << "'";
        return -1;
      }
      BOOST_LOG(info) << "[pipewire] Streaming display '"sv << display_name << "' offset: "sv << offset_x << "x"sv << offset_y << " resolution: "sv << width << "x"sv << height;

      // Verify or update display parameters for streaming to ensure absolute touch inputs work as expected
      verify_and_update_display_parameters();

      if (!shared_state) {
        shared_state = std::make_shared<shared_state_t>();
      } else {
        shared_state->stream_dead.store(false);
        shared_state->negotiated_width.store(0);
        shared_state->negotiated_height.store(0);
        shared_state->color_primaries.store(0);
        shared_state->transfer_function.store(0);
      }

      if (pipewire.init(pipewire_fd, pipewire_node, pipewire_object_serial, shared_state) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to init pipewire. pipewire_t::init() failed.";
        return -1;
      }

      // Start PipeWire now so format negotiation can proceed before capture start
      if (pipewire.ensure_stream(mem_type, width, height, target_framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to ensure pipewire stream. pipewire_t::init() failed.";
        return -1;
      }

      // Wait for pipewire negotiation to finish so we have the proper negotiated dimensions
      int timeout_ms = 1500;
      int negotiated_w = 0;
      int negotiated_h = 0;
      while (timeout_ms > 0) {
        negotiated_w = shared_state->negotiated_width.load();
        negotiated_h = shared_state->negotiated_height.load();
        if (negotiated_w > 0 && negotiated_h > 0) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timeout_ms -= 10;
      }
      // Set width and height to the values negotiated by pipewire
      if (negotiated_w > 0 && negotiated_h > 0 && (negotiated_w != width || negotiated_h != height)) {
        width = negotiated_w;
        height = negotiated_h;
        BOOST_LOG(info) << "[pipewire] Using negotiated Resolution: "sv << width << "x" << height;

        // Reset and update display parameters for negotiated resolution
        env_width = 0;
        env_height = 0;
        logical_height = 0;
        logical_width = 0;
        env_logical_height = 0;
        env_logical_width = 0;
        verify_and_update_display_parameters();
      }

      if (mem_type == platf::mem_type_e::system && capture_egl_ready) {
        static std::atomic<bool> software_dmabuf_probed {false};
        if (!software_dmabuf_probed.exchange(true)) {
          int best_nonzero = -1;
          for (int attempt = 0; attempt < 12; ++attempt) {
            std::shared_ptr<platf::img_t> probe_img;
            const pull_free_image_cb_t pull = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
              img_out = alloc_img();
              return static_cast<bool>(img_out);
            };
            const auto st = snapshot(pull, probe_img, 250ms, true);
            int nonzero = 0;
            if (probe_img && probe_img->data) {
              const auto nbytes = static_cast<size_t>(std::max(probe_img->height, 0)) * static_cast<size_t>(std::max(probe_img->row_pitch, 0));
              nonzero = static_cast<int>(count_nonzero_samples(probe_img->data, nbytes));
            }
            BOOST_LOG(info) << "[pipewire] software DMA-BUF probe snapshot status="sv << std::to_underlying(st)
                            << " nonzero="sv << nonzero << " attempt="sv << attempt;
            best_nonzero = std::max(best_nonzero, nonzero);
            if (nonzero > 0) {
              break;
            }
          }
        }
      }

      return 0;
    }

    /**
     * @brief Copy a PipeWire DMA-BUF into the software encoder's CPU image.
     *
     * KWin writes real pixels to DMA-BUF. The MemFd fallback is empty from
     * Distrobox, so software encode has to import the GPU buffer like kmsgrab.
     */
    int copy_dmabuf_to_cpu(egl::img_descriptor_t *img) {
      if (!img || img->sd.fds[0] < 0 || !img->data) {
        return -1;
      }
      if (!capture_egl_ready) {
        BOOST_LOG(error) << "[pipewire] software DMA-BUF copy needs an EGL context"sv;
        return -1;
      }

      wait_dmabuf_fence(img->sd.fds[0]);

      auto *disp = std::get<0>(capture_egl_ctx.el);
      auto ctx = std::get<1>(capture_egl_ctx.el);
      if (!eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        BOOST_LOG(error) << "[pipewire] eglMakeCurrent failed: "sv << util::hex(eglGetError()).to_string_view();
        return -1;
      }

      auto rgb_opt = egl::import_source(capture_egl_display.get(), img->sd);
      if (!rgb_opt) {
        return -1;
      }

      auto &rgb = *rgb_opt;
      gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);
      int tex_w = 0;
      int tex_h = 0;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      const auto copy_w = std::min(img->width, std::max(tex_w, 0));
      const auto copy_h = std::min(img->height, std::max(tex_h, 0));
      if (copy_w <= 0 || copy_h <= 0) {
        BOOST_LOG(error) << "[pipewire] DMA-BUF texture size "sv << tex_w << "x"sv << tex_h;
        return -1;
      }

      // GetTextureSubImage on an imported DMA-BUF texture returns zeros on
      // this AMD/Mesa + Distrobox path. Blit into a regular FBO first, which
      // also waits on the implicit write fence.
      GLuint src_fbo = 0;
      GLuint dst_fbo = 0;
      GLuint dst_tex = 0;
      gl::ctx.GenFramebuffers(1, &src_fbo);
      gl::ctx.GenFramebuffers(1, &dst_fbo);
      gl::ctx.GenTextures(1, &dst_tex);
      gl::ctx.BindTexture(GL_TEXTURE_2D, dst_tex);
      gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, copy_w, copy_h);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

      gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
      gl::ctx.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgb->tex[0], 0);
      const auto src_status = gl::ctx.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
      gl::ctx.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
      const auto dst_status = gl::ctx.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
      if (src_status != GL_FRAMEBUFFER_COMPLETE || dst_status != GL_FRAMEBUFFER_COMPLETE) {
        BOOST_LOG(error) << "[pipewire] DMA-BUF blit FBO incomplete src="sv << src_status << " dst="sv << dst_status
                         << " fourcc="sv << img->sd.fourcc << " modifier="sv << img->sd.modifier;
        gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl::ctx.DeleteFramebuffers(1, &src_fbo);
        gl::ctx.DeleteFramebuffers(1, &dst_fbo);
        gl::ctx.DeleteTextures(1, &dst_tex);
        return -1;
      }

      gl::ctx.BlitFramebuffer(0, 0, copy_w, copy_h, 0, 0, copy_w, copy_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      gl::ctx.Finish();
      gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
      gl::ctx.PixelStorei(GL_PACK_ROW_LENGTH, img->row_pitch / std::max(img->pixel_pitch, 1));
      gl::ctx.ReadPixels(0, 0, copy_w, copy_h, GL_BGRA, GL_UNSIGNED_BYTE, img->data);
      gl::ctx.PixelStorei(GL_PACK_ROW_LENGTH, 0);
      const auto gl_err = gl::ctx.GetError();
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl::ctx.DeleteFramebuffers(1, &src_fbo);
      gl::ctx.DeleteFramebuffers(1, &dst_fbo);
      gl::ctx.DeleteTextures(1, &dst_tex);

      static std::atomic<int> copies {0};
      const int n = copies.fetch_add(1);
      if (n < 8) {
        size_t nonzero = 0;
        const auto nbytes = static_cast<size_t>(copy_h) * static_cast<size_t>(img->row_pitch);
        for (size_t i = 0; i < nbytes; ++i) {
          nonzero += img->data[i] != 0;
        }
        BOOST_LOG(info) << "[pipewire] DMA-BUF copied "sv << copy_w << "x"sv << copy_h
                        << " nonzero="sv << nonzero << "/"sv << nbytes
                        << " fourcc="sv << img->sd.fourcc << " modifier="sv << img->sd.modifier
                        << " gl_err="sv << gl_err
                        << " renderer="sv << (gl::ctx.GetString ? reinterpret_cast<const char *>(gl::ctx.GetString(GL_RENDERER)) : "?")
                        << " n="sv << n;
      }

      img->reset();
      return 0;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured PipeWire image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param show_cursor Show cursor.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool show_cursor) {
      // FIXME: show_cursor is ignored
      auto deadline = std::chrono::steady_clock::now() + timeout;
      int retries = 0;

      while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_for_frame(deadline)) {
          return platf::capture_e::timeout;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        auto *img_egl = static_cast<egl::img_descriptor_t *>(img_out.get());
        img_egl->reset();
        pipewire.fill_img(img_egl);

        const auto nbytes = static_cast<size_t>(std::max(img_egl->height, 0)) * static_cast<size_t>(std::max(img_egl->row_pitch, 0));
        auto cpu_pixels = [&]() {
          return img_egl->data && count_nonzero_samples(img_egl->data, nbytes) > 0;
        };

        if (mem_type == platf::mem_type_e::system && img_egl->sd.fds[0] >= 0) {
          if (!(pipewire.is_cpu_frame_valid() && cpu_pixels())) {
            copy_dmabuf_to_cpu(img_egl);
          }
          img_egl->reset();
        }

        if (mem_type == platf::mem_type_e::system) {
          // Give KWin the buffer back so the next vblank can be captured.
          // The first DMA-BUF is often a cleared placeholder.
          pipewire.release_current_buffer();
          if (cpu_pixels() && !is_buffer_redundant(img_egl)) {
            update_metadata(img_egl, retries);
            return platf::capture_e::ok;
          }
        } else if ((img_egl->sd.fds[0] >= 0 || img_egl->data != nullptr) && !is_buffer_redundant(img_egl)) {
          update_metadata(img_egl, retries);
          return platf::capture_e::ok;
        }

        // No valid frame yet, or it was a duplicate
        retries++;
      }
      return platf::capture_e::timeout;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      // Note: this img_t type is also used for memory buffers
      auto img = std::make_shared<img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      const auto bytes = static_cast<size_t>(std::max(img->height, 0)) * static_cast<size_t>(std::max(img->row_pitch, 0));
      img->data = bytes > 0 ? new uint8_t[bytes]() : nullptr;
      img->data_owned = img->data != nullptr;
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    /**
     * @brief Check stream dead.
     *
     * @param out_status Out status.
     * @return True when the PipeWire stream can no longer produce frames.
     */
    virtual bool check_stream_dead(platf::capture_e &out_status) {
      return false;  // Return to default stream dead handling.
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      if (pipewire.ensure_stream(mem_type, width, height, target_framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to ensure pipewire stream. capture() failed with error.";
        return platf::capture_e::error;
      }
      sleep_overshoot_logger.reset();

      // Check if pacing is required
      bool pacing_required = pipewire.is_pacing_required(target_framerate, delay);

      while (true) {
        // Check if PipeWire signaled a dead stream
        if (shared_state->stream_dead.exchange(false)) {
          // Additional custom error-handling for subclasses on stream dead event
          if (platf::capture_e status; check_stream_dead(status)) {
            return status;
          }
          // Re-init the capture if the stream is dead for any other reason
          BOOST_LOG(warning) << "[pipewire] PipeWire stream disconnected. Forcing session reset."sv;
          return platf::capture_e::reinit;
        }

        // Use unpaced event driven capture when possible
        if (pacing_required) {
          platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);
        }

        std::shared_ptr<platf::img_t> img_out;
        switch (const auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor)) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            pipewire.frame_cv().notify_all();
            return status;
          case platf::capture_e::timeout:
            if (!pull_free_image_cb(img_out)) {
              // Detect if shutdown is pending
              BOOST_LOG(debug) << "[pipewire] PipeWire: timeout -> shutdown pending -> interrupt nudge";
              pipewire.frame_cv().notify_all();
              return platf::capture_e::interrupted;
            }
            if (!push_captured_image_cb(std::move(img_out), false)) {
              BOOST_LOG(debug) << "[pipewire] PipeWire: timeout -> !push_captured_image_cb -> ok";
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              BOOST_LOG(debug) << "[pipewire] PipeWire: ok -> !push_captured_image_cb -> ok";
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "[pipewire] Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, n_dmabuf_infos > 0);
      }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan && n_dmabuf_infos > 0) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (display_is_nvidia && n_dmabuf_infos > 0) {
          // Display GPU is NVIDIA - can use DMA-BUF directly
          return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
        } else {
          // Hybrid system (Intel display + NVIDIA encode) - use memory buffer path
          // DMA-BUFs from Intel GPU cannot be imported into CUDA
          return cuda::make_avcodec_encode_device(width, height, false);
        }
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(platf::img_t *img) override {
      // Software encoders convert the dummy image immediately; provide a valid
      // (black) buffer instead of leaving img->data null, which makes sws fail
      // with EINVAL. The buffer is new[]-allocated and marked as owned so the
      // destructor releases it.
      if (img->data == nullptr) {
        const auto w = img->width;
        const auto h = img->height;
        if (w > 0 && h > 0) {
          img->data = new uint8_t[static_cast<size_t>(w) * h * 4]();  // NOSONAR(cpp:S5025) - buffer is owned by the image and freed by img_descriptor_t's destructor
          static_cast<img_descriptor_t *>(img)->data_owned = true;
          img->row_pitch = w * 4;
          img->pixel_pitch = 4;
        }
      }
      return 0;
    }

    /**
     * @brief Report whether the active display mode is HDR.
     *
     * @return True when the active display mode is HDR.
     */
    bool is_hdr() override {
      int color_primaries = shared_state->color_primaries.load();
      int transfer_function = shared_state->transfer_function.load();

      if (color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT2020 && transfer_function == SPA_VIDEO_TRANSFER_SMPTE2084) {
        return true;
      }

      return false;
    }

    /**
     * @brief Read HDR metadata for the active display mode.
     *
     * @param metadata Output structure populated with HDR metadata.
     * @return True when HDR metadata was written to the output structure.
     */
    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      int color_primaries = shared_state->color_primaries.load();
      int transfer_function = shared_state->transfer_function.load();

      if (color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT2020 && transfer_function == SPA_VIDEO_TRANSFER_SMPTE2084) {
        // Report Rec 2020 primaries
        metadata.displayPrimaries[0].x = 0.708f * 50000;
        metadata.displayPrimaries[0].y = 0.292f * 50000;
        metadata.displayPrimaries[1].x = 0.170f * 50000;
        metadata.displayPrimaries[1].y = 0.797f * 50000;
        metadata.displayPrimaries[2].x = 0.131f * 50000;
        metadata.displayPrimaries[2].y = 0.046f * 50000;
        metadata.whitePoint.x = 0.3127f * 50000;
        metadata.whitePoint.y = 0.3290f * 50000;

        // This is according to HDR10+ standards, should probably be based on actual data
        metadata.maxDisplayLuminance = 4000;
        metadata.minDisplayLuminance = 1;

        // These are content-specific metadata parameters that this interface doesn't give us
        metadata.maxContentLightLevel = 0;
        metadata.maxFrameAverageLightLevel = 0;
        metadata.maxFullFrameLuminance = 0;

        return true;
      }

      return false;
    }

    /**
     * Fetch the currently running KWin version (if available from its DBUS support information method)
     *
     * @return A vector with 3 elements containing KWin's major.minor.micro version or an empty vector if KWin's version could not be determined
     */
    static std::vector<int> get_running_kwin_version() {
#if !GLIB_CHECK_VERSION(2, 74, 0)
      // Compatibility for Ubuntu 22.04 (Glib 2.72)
      constexpr auto G_REGEX_DEFAULT = static_cast<GRegexCompileFlags>(0);
      constexpr auto G_REGEX_MATCH_DEFAULT = static_cast<GRegexMatchFlags>(0);
#endif
      auto conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      std::vector<int> result;

      if (!conn) {
        return result;
      }

      auto reply = g_dbus_connection_call_sync(
        conn,
        "org.kde.KWin",
        "/KWin",
        "org.kde.KWin",
        "supportInformation",
        nullptr,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
      );

      if (!reply) {
        g_clear_object(&conn);
        return result;
      }

      g_autofree gchar *support_info = nullptr;
      g_variant_get(reply, "(s)", &support_info);

      if (!support_info) {
        g_variant_unref(reply);
        g_clear_object(&conn);
        return result;
      }

      auto *regex = g_regex_new(
        "KWin version: ([0-9]+)\\.([0-9]+)\\.([0-9]+)",
        G_REGEX_DEFAULT,
        G_REGEX_MATCH_DEFAULT,
        nullptr
      );

      if (!regex) {
        g_variant_unref(reply);
        g_clear_object(&conn);
        return result;
      }

      GMatchInfo *match_info = nullptr;
      g_regex_match(regex, support_info, G_REGEX_MATCH_DEFAULT, &match_info);

      if (g_match_info_matches(match_info)) {
        g_autofree const gchar *major =
          g_match_info_fetch(match_info, 1);
        g_autofree const gchar *minor =
          g_match_info_fetch(match_info, 2);
        g_autofree const gchar *micro =
          g_match_info_fetch(match_info, 3);

        result.emplace_back(std::atoi(major));
        result.emplace_back(std::atoi(minor));
        result.emplace_back(std::atoi(micro));
      }

      g_match_info_free(match_info);
      g_regex_unref(regex);
      g_variant_unref(reply);
      g_clear_object(&conn);

      return result;
    }

  private:
    bool is_buffer_redundant(const egl::img_descriptor_t *img) {
      // Check for corrupted frame
      if (img->pw_flags.has_value() && (img->pw_flags.value() & SPA_CHUNK_FLAG_CORRUPTED)) {
        return true;
      }

      // If PTS is identical, only drop if damage metadata confirms no change
      if (img->pts.has_value() && last_pts.has_value() && img->pts.value() == last_pts.value()) {
        return img->pw_damage.has_value() && !img->pw_damage.value();
      }

      return false;
    }

    void update_metadata(egl::img_descriptor_t *img, int retries) {
      last_seq = img->seq;
      last_pts = img->pts;
      img->sequence = ++sequence;

      if (retries > 0) {
        BOOST_LOG(debug) << "[pipewire] Processed frame after " << retries << " redundant events."sv;
      }
    }

    bool wait_for_frame(std::chrono::steady_clock::time_point deadline) {
      std::unique_lock<std::mutex> lock(pipewire.frame_mutex());

      bool success = pipewire.frame_cv().wait_until(lock, deadline, [&] {
        return pipewire.is_frame_ready() || shared_state->stream_dead.load();
      });

      if (success) {
        pipewire.set_frame_ready(false);
        return true;
      }
      return false;
    }

    static bool pw_format_supported(uint64_t fourcc, std::array<EGLint, MAX_DMABUF_FORMATS> dmabuf_formats) {
      for (const auto &drm_format : dmabuf_formats) {
        if (drm_format == fourcc) {
          return true;
        }
      }
      return false;
    }

    void query_dmabuf_formats(EGLDisplay egl_display) {
      EGLint num_dmabuf_formats = 0;
      std::array<EGLint, MAX_DMABUF_FORMATS> dmabuf_formats = {0};
      eglQueryDmaBufFormatsEXT(egl_display, MAX_DMABUF_FORMATS, dmabuf_formats.data(), &num_dmabuf_formats);

      if (num_dmabuf_formats > MAX_DMABUF_FORMATS) {
        BOOST_LOG(warning) << "[pipewire] Some DMA-BUF formats are being ignored"sv;
      }

      for (const auto &fmt : format_map) {
        if (n_dmabuf_infos >= MAX_DMABUF_FORMATS) {
          break;
        }

        if (!pw_format_supported(fmt.fourcc, dmabuf_formats)) {
          continue;
        }

        EGLint num_modifiers = 0;
        std::array<EGLuint64KHR, MAX_DMABUF_MODIFIERS> mods = {0};
        eglQueryDmaBufModifiersEXT(egl_display, fmt.fourcc, MAX_DMABUF_MODIFIERS, mods.data(), nullptr, &num_modifiers);

        if (num_modifiers > MAX_DMABUF_MODIFIERS) {
          BOOST_LOG(warning) << "[pipewire] Some DMA-BUF modifiers are being ignored"sv;
        }

        dmabuf_infos[n_dmabuf_infos].format = fmt.pw_format;
        dmabuf_infos[n_dmabuf_infos].n_modifiers = MIN(num_modifiers, MAX_DMABUF_MODIFIERS);
        dmabuf_infos[n_dmabuf_infos].modifiers =
          static_cast<uint64_t *>(g_memdup2(mods.data(), sizeof(uint64_t) * dmabuf_infos[n_dmabuf_infos].n_modifiers));
        ++n_dmabuf_infos;
      }
    }

    int get_dmabuf_modifiers() {
      n_dmabuf_infos = 0;
      capture_egl_ready = false;

      if (wl_display.init() < 0) {
        return -1;
      }

      capture_egl_display = egl::make_display(wl_display.get());
      if (!capture_egl_display) {
        return -1;
      }

      if (auto ctx_opt = egl::make_ctx(capture_egl_display.get())) {
        capture_egl_ctx = std::move(*ctx_opt);
        capture_egl_ready = true;
      } else {
        BOOST_LOG(warning) << "[pipewire] EGL context unavailable; software DMA-BUF copy disabled"sv;
      }

      // Detect if this is a pure NVIDIA system (not hybrid Intel+NVIDIA)
      // On hybrid systems, the wayland compositor typically runs on Intel,
      // so DMA-BUFs from portal will come from Intel and cannot be imported into CUDA.
      // Check if Intel GPU exists - if so, assume hybrid system and disable CUDA DMA-BUF.
      bool has_intel_gpu = std::ifstream("/sys/class/drm/card0/device/vendor").good() ||
                           std::ifstream("/sys/class/drm/card1/device/vendor").good();
      if (has_intel_gpu) {
        // Read vendor IDs to check for Intel (0x8086)
        auto check_intel = [](const std::string &path) {
          if (std::ifstream f(path); f.good()) {
            std::string vendor;
            f >> vendor;
            return vendor == "0x8086";
          }
          return false;
        };
        bool intel_present = check_intel("/sys/class/drm/card0/device/vendor") ||
                             check_intel("/sys/class/drm/card1/device/vendor");
        if (intel_present) {
          BOOST_LOG(info) << "[pipewire] Hybrid GPU system detected (Intel + discrete) - CUDA will use memory buffers"sv;
          display_is_nvidia = false;
        } else {
          // No Intel GPU found, check if NVIDIA is present
          const char *vendor = eglQueryString(capture_egl_display.get(), EGL_VENDOR);
          if (vendor && std::string_view(vendor).contains("NVIDIA")) {
            BOOST_LOG(info) << "[pipewire] Pure NVIDIA system - DMA-BUF will be enabled for CUDA"sv;
            display_is_nvidia = true;
          }
        }
      }

      if (eglQueryDmaBufFormatsEXT && eglQueryDmaBufModifiersEXT) {
        query_dmabuf_formats(capture_egl_display.get());
      }

      return 0;
    }

    platf::mem_type_e mem_type;
    wl::display_t wl_display;
    egl::display_t capture_egl_display;
    egl::ctx_t capture_egl_ctx;
    bool capture_egl_ready = false;
    std::array<struct dmabuf_format_info_t, MAX_DMABUF_FORMATS> dmabuf_infos;
    int n_dmabuf_infos = 0;
    bool display_is_nvidia = false;  // Track if display GPU is NVIDIA
    std::chrono::nanoseconds delay;
    std::optional<std::uint64_t> last_pts {};
    std::optional<std::uint64_t> last_seq {};
    std::uint64_t sequence {};
    AVRational target_framerate;

  protected:
    // Allow subclasses to access for pipewire requirements setup and stream dead checks
    pipewire_t pipewire;  ///< Pipewire.
    std::shared_ptr<shared_state_t> shared_state;  ///< Shared state.
  };
}  // namespace pipewire
