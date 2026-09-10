/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <boost/regex.hpp>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief Position mapping.
   */
  constexpr pa_channel_position_t position_mapping[] {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  /**
   * @brief Convert a PulseAudio operation result to a log string.
   *
   * @param name Human-readable name to assign.
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @return Value converted to string.
   */
  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

  /**
   * @brief PulseAudio recording stream and channel metadata.
   *
   * Game Mode's null-sink monitor is often silent (Cemu plays to gamescope).
   * `pa_simple_read` and `pa_mainloop_poll` can block forever on that source,
   * so session::join never finishes, the control thread exits on app stop,
   * and Moonlight hits control establishment error / Initial Ping Timeout.
   * Drive Pulse with non-blocking `pa_mainloop_iterate` so shutdown is observed.
   */
  struct mic_attr_t: public mic_t {
    std::unique_ptr<pa_mainloop, void (*)(pa_mainloop *)> loop {nullptr, pa_mainloop_free};
    std::unique_ptr<pa_context, void (*)(pa_context *)> ctx {nullptr, pa_context_unref};
    std::unique_ptr<pa_stream, void (*)(pa_stream *)> stream {nullptr, pa_stream_unref};
    std::vector<std::uint8_t> pending;

    ~mic_attr_t() override {
      if (stream) {
        pa_stream_set_read_callback(stream.get(), nullptr, nullptr);
        const auto st = pa_stream_get_state(stream.get());
        if (st != PA_STREAM_UNCONNECTED && st != PA_STREAM_FAILED && st != PA_STREAM_TERMINATED) {
          pa_stream_disconnect(stream.get());
        }
      }
      if (ctx) {
        const auto st = pa_context_get_state(ctx.get());
        if (st != PA_CONTEXT_UNCONNECTED && st != PA_CONTEXT_FAILED && st != PA_CONTEXT_TERMINATED) {
          pa_context_disconnect(ctx.get());
        }
      }
    }

    static void stream_read(pa_stream *s, size_t /*nbytes*/, void *userdata) {
      auto *self = static_cast<mic_attr_t *>(userdata);
      const void *data = nullptr;
      size_t count = 0;
      if (pa_stream_peek(s, &data, &count) < 0) {
        return;
      }
      if (data && count > 0) {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        self->pending.insert(self->pending.end(), bytes, bytes + count);
      }
      if (count > 0) {
        pa_stream_drop(s);
      }
    }

    bool iterate(int timeout_usec) {
      if (!loop) {
        return false;
      }
      // pa_mainloop_poll() can ignore prepare()'s timeout on a silent
      // Game Mode PipeWire-pulse monitor and block forever, so session::join
      // never returns and the next Moonlight tap hits control establishment
      // error. Drive the loop non-blocking and sleep ourselves.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(std::max(timeout_usec, 0));
      for (;;) {
        int retval = 0;
        const int dispatched = pa_mainloop_iterate(loop.get(), 0, &retval);
        if (dispatched < 0) {
          return false;
        }
        if (dispatched > 0 || std::chrono::steady_clock::now() >= deadline) {
          return true;
        }
        std::this_thread::sleep_for(5ms);
      }
    }

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_buf Sample buf.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_buf) override {
      const auto want = sample_buf.size() * sizeof(float);
      const auto deadline = std::chrono::steady_clock::now() + 200ms;
      while (pending.size() < want) {
        if (stream) {
          const auto st = pa_stream_get_state(stream.get());
          if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) {
            return capture_e::error;
          }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return capture_e::timeout;
        }
        if (!iterate(50000)) {
          return capture_e::error;
        }
      }
      std::memcpy(sample_buf.data(), pending.data(), want);
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(want));
      return capture_e::ok;
    }
  };

  /**
   * @brief Create a microphone capture stream for the requested layout.
   *
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @param sample_rate Audio sample rate in hertz.
   * @param frame_size Number of samples captured per audio frame.
   * @param source_name Source name.
   * @return Microphone capture object for the requested audio layout.
   */
  std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    auto mic = std::make_unique<mic_attr_t>();
    mic->loop.reset(pa_mainloop_new());
    if (!mic->loop) {
      BOOST_LOG(error) << "pa_mainloop_new() failed"sv;
      return nullptr;
    }

    mic->ctx.reset(pa_context_new(pa_mainloop_get_api(mic->loop.get()), "sunshine-record"));
    if (!mic->ctx) {
      BOOST_LOG(error) << "pa_context_new() failed"sv;
      return nullptr;
    }

    if (auto status = pa_context_connect(mic->ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr)) {
      BOOST_LOG(error) << "pa_context_connect() failed: "sv << pa_strerror(status);
      return nullptr;
    }

    const auto ctx_deadline = std::chrono::steady_clock::now() + 2s;
    while (pa_context_get_state(mic->ctx.get()) != PA_CONTEXT_READY) {
      const auto state = pa_context_get_state(mic->ctx.get());
      if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED ||
          std::chrono::steady_clock::now() >= ctx_deadline) {
        BOOST_LOG(error) << "PulseAudio context not ready for record"sv;
        return nullptr;
      }
      if (!mic->iterate(50000)) {
        return nullptr;
      }
    }

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
    pa_channel_map pa_map {};
    pa_map.channels = (std::uint8_t) channels;
    std::for_each_n(pa_map.map, pa_map.channels, [mapping](auto &channel) mutable {
      channel = position_mapping[*mapping++];
    });

    mic->stream.reset(pa_stream_new(mic->ctx.get(), "sunshine-record", &ss, &pa_map));
    if (!mic->stream) {
      BOOST_LOG(error) << "pa_stream_new() failed"sv;
      return nullptr;
    }
    pa_stream_set_read_callback(mic->stream.get(), &mic_attr_t::stream_read, mic.get());

    pa_buffer_attr pa_attr = {
      .maxlength = uint32_t(-1),
      .tlength = uint32_t(-1),
      .prebuf = uint32_t(-1),
      .minreq = uint32_t(-1),
      .fragsize = uint32_t(frame_size * channels * sizeof(float))
    };

    const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND);
    if (auto status = pa_stream_connect_record(mic->stream.get(), source_name.c_str(), &pa_attr, flags)) {
      BOOST_LOG(error) << "pa_stream_connect_record() failed: "sv << pa_strerror(status);
      return nullptr;
    }

    const auto stream_deadline = std::chrono::steady_clock::now() + 2s;
    while (pa_stream_get_state(mic->stream.get()) != PA_STREAM_READY) {
      const auto state = pa_stream_get_state(mic->stream.get());
      if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED ||
          std::chrono::steady_clock::now() >= stream_deadline) {
        BOOST_LOG(error) << "PulseAudio record stream not ready"sv;
        return nullptr;
      }
      if (!mic->iterate(50000)) {
        return nullptr;
      }
    }

    return mic;
  }

  namespace pa {
    template<bool B, class T>
    struct add_const_helper;

    /**
     * @brief Template helper that preserves constness for const inputs.
     */
    template<class T>
    struct add_const_helper<true, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const std::remove_pointer_t<T> *;
    };

    /**
     * @brief Template helper that leaves non-const inputs mutable.
     */
    template<class T>
    struct add_const_helper<false, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const T *;
    };

    /**
     * @brief PulseAudio callback info type with pointer constness normalized.
     */
    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    /**
     * @brief Release memory allocated by PulseAudio.
     *
     * @param p Pointer allocated by PulseAudio and released with `pa_xfree`.
     */
    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    /**
     * @brief Owning pointer for a PulseAudio context.
     */
    using ctx_t = util::safe_ptr<pa_context, pa_context_unref>;
    /**
     * @brief Owning pointer for a PulseAudio mainloop.
     */
    using loop_t = util::safe_ptr<pa_mainloop, pa_mainloop_free>;
    /**
     * @brief Owning pointer for a PulseAudio asynchronous operation.
     */
    using op_t = util::safe_ptr<pa_operation, pa_operation_unref>;
    /**
     * @brief Owning pointer for PulseAudio strings allocated with `pa_xmalloc`.
     */
    using string_t = util::safe_ptr<char, pa_free<char>>;

    /**
     * @brief Callback wrapper for PulseAudio introspection results without an end marker.
     */
    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    /**
     * @brief Handle PulseAudio sink-input introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    /**
     * @brief Callback wrapper for PulseAudio introspection results with an end marker.
     */
    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    /**
     * @brief Handle PulseAudio source introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param eol PulseAudio end-of-list marker.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }

      f(ctx, i, eol);
    }

    /**
     * @brief Forward a PulseAudio integer callback value into a Sunshine alarm.
     *
     * @param ctx PulseAudio context that emitted the callback.
     * @param i Integer value returned by the PulseAudio operation.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void cb_i(ctx_t::pointer ctx, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    /**
     * @brief Translate PulseAudio context state changes into server events.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void ctx_state_cb(ctx_t::pointer ctx, void *userdata) {
      auto &f = *(std::function<void(ctx_t::pointer)> *) userdata;

      f(ctx);
    }

    /**
     * @brief Record completion of a PulseAudio asynchronous operation.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param status Native status code returned by the platform API.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void success_cb(ctx_t::pointer ctx, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    /**
     * @brief PulseAudio server controller that creates and removes Sunshine sinks.
     */
    class server_t: public audio_control_t {
      enum ctx_event_e : int {
        ready,
        terminated,
        failed
      };

    public:
      loop_t loop;  ///< PulseAudio threaded mainloop instance.
      ctx_t ctx;  ///< PulseAudio threaded mainloop context.
      std::string requested_sink;  ///< Requested sink.

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;  ///< PulseAudio module index for the stereo null sink.
        std::uint32_t surround51 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 5.1 null sink.
        std::uint32_t surround71 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 7.1 null sink.
      } index;  ///< PulseAudio module indexes for Sunshine-created null sinks.

      std::unique_ptr<safe::event_t<ctx_event_e>> events;  ///< Event queue receiving PulseAudio context state changes.
      std::unique_ptr<std::function<void(ctx_t::pointer)>> events_cb;  ///< Callback that translates PulseAudio context updates into events.

      std::jthread worker;  ///< Thread running the PulseAudio mainloop.

      /**
       * @brief Initialize PulseAudio mainloop, context, and Sunshine null sinks.
       *
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init() {
        events = std::make_unique<safe::event_t<ctx_event_e>>();
        loop.reset(pa_mainloop_new());
        ctx.reset(pa_context_new(pa_mainloop_get_api(loop.get()), "sunshine"));

        events_cb = std::make_unique<std::function<void(ctx_t::pointer)>>([this](ctx_t::pointer ctx) {
          switch (pa_context_get_state(ctx)) {
            case PA_CONTEXT_READY:
              events->raise(ready);
              break;
            case PA_CONTEXT_TERMINATED:
              BOOST_LOG(debug) << "PulseAudio context terminated"sv;
              events->raise(terminated);
              break;
            case PA_CONTEXT_FAILED:
              BOOST_LOG(debug) << "PulseAudio context failed"sv;
              events->raise(failed);
              break;
            case PA_CONTEXT_CONNECTING:
              BOOST_LOG(debug) << "Connecting to pulseaudio"sv;
            case PA_CONTEXT_UNCONNECTED:
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_SETTING_NAME:
              break;
          }
        });

        pa_context_set_state_callback(ctx.get(), ctx_state_cb, events_cb.get());

        auto status = pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr);
        if (status) {
          BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(status);
          return -1;
        }

        worker = std::jthread {
          [](loop_t::pointer loop) {
            int retval;
            platf::set_thread_name("audio::pulseaudio");
            auto status = pa_mainloop_run(loop, &retval);

            if (status < 0) {
              BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
              return;
            }
          },
          loop.get()
        };

        auto event = events->pop();
        if (event == failed) {
          return -1;
        }

        return 0;
      }

      /**
       * @brief Create a PulseAudio null sink for one channel layout.
       *
       * @param name Human-readable name to assign.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @return PulseAudio module index for the new sink, or PA_INVALID_INDEX on failure.
       */
      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_load_module(
            ctx.get(),
            "module-null-sink",
            to_string(name, channel_mapping, channels).c_str(),
            cb_i,
            alarm.get()
          ),
        };

        alarm->wait();
        return *alarm->status();
      }

      /**
       * @brief Unload a Sunshine-created PulseAudio null sink.
       *
       * @param i PulseAudio introspection info supplied to the callback.
       * @return 0 when the sink is absent or unloaded; nonzero on PulseAudio failure.
       */
      int unload_null(std::uint32_t i) {
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_unload_module(ctx.get(), i, success_cb, alarm.get())
        };

        alarm->wait();

        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        return 0;
      }

      /**
       * @brief Query host and virtual sink names available to Sunshine.
       *
       * @return Host and virtual sink names when the backend can report them.
       */
      std::optional<sink_t> sink_info() override {
        constexpr auto stereo = "sink-sunshine-stereo";
        constexpr auto surround51 = "sink-sunshine-surround51";
        constexpr auto surround71 = "sink-sunshine-surround71";

        auto alarm = safe::make_alarm<int>();

        sink_t sink;

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(ctx));

              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          // Ensure Sunshine won't create a sink that already exists.
          if (!std::strcmp(sink_info->name, stereo)) {
            index.stereo = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround51)) {
            index.surround51 = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround71)) {
            index.surround71 = sink_info->owner_module;

            ++nullcount;
          }
        };

        op_t op {pa_context_get_sink_info_list(ctx.get(), cb<pa_sink_info *>, &f)};

        if (!op) {
          BOOST_LOG(error) << "Couldn't create card info operation: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return std::nullopt;
        }

        alarm->wait();

        if (*alarm->status()) {
          return std::nullopt;
        }

        auto sink_name = get_default_sink_name();
        sink.host = sink_name;

        if (index.stereo == PA_INVALID_INDEX) {
          index.stereo = load_null(stereo, speaker::map_stereo.data(), static_cast<int>(speaker::map_stereo.size()));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51.data(), static_cast<int>(speaker::map_surround51.size()));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71.data(), static_cast<int>(speaker::map_surround71.size()));
          if (index.surround71 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-71: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (sink_name.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          sink.null = std::make_optional(sink_t::null_t {stereo, surround51, surround71});
        }

        return std::make_optional(std::move(sink));
      }

      /**
       * @brief Get default sink name.
       *
       * @return PulseAudio name of the current default sink, or an empty string.
       */
      std::string get_default_sink_name() {
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(ctx));
            alarm->ring(-1);
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        alarm->wait();
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      /**
       * @brief Get monitor name.
       *
       * @param sink_name Sink name.
       * @return PulseAudio monitor source name for the supplied sink, or an empty string.
       */
      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          monitor_name = sink_info->monitor_source_name;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        alarm->wait();
        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      /**
       * @brief Create a microphone capture stream for the requested layout.
       *
       * @param mapping Opus channel mapping table for the requested layout.
       * @param channels Number of audio channels in the stream.
       * @param sample_rate Audio sample rate in hertz.
       * @param frame_size Number of samples captured per audio frame.
       * @param continuous_audio Continuous audio.
       * @param host_audio_enabled Whether host playback should remain enabled during capture.
       * @return Microphone capture object for the requested audio layout.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
        // Sink choice priority:
        // 1. Config sink
        // 2. Last sink swapped to (Usually virtual in this case)
        // 3. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = config::audio.sink;
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        return ::platf::microphone(mapping, channels, sample_rate, frame_size, get_monitor_name(sink_name));
      }

      bool is_sink_available(const std::string &sink) override {
        BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
        return true;
      }

      /**
       * @brief Update the sink value on the backend.
       *
       * @param sink Audio sink name to route or capture.
       * @return Status from updating sink.
       */
      int set_sink(const std::string &sink) override {
        auto alarm = safe::make_alarm<int>();

        BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
        op_t op {
          pa_context_set_default_sink(
            ctx.get(),
            sink.c_str(),
            success_cb,
            alarm.get()
          ),
        };

        if (!op) {
          BOOST_LOG(error) << "Couldn't create set default-sink operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        alarm->wait();
        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return -1;
        }

        requested_sink = sink;

        return 0;
      }

      ~server_t() override {
        unload_null(index.stereo);
        unload_null(index.surround51);
        unload_null(index.surround71);

        if (worker.joinable()) {
          pa_context_disconnect(ctx.get());

          KITTY_WHILE_LOOP(auto event = events->pop(), event != terminated && event != failed, {
            event = events->pop();
          })

          pa_mainloop_quit(loop.get(), 0);
          worker.join();
        }
      }
    };
  }  // namespace pa

  /**
   * @brief Create the platform audio controller.
   */
  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
