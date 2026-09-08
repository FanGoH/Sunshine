/**
 * Hold a KWin compositor virtual output via zkde_screencast_unstable_v1.
 *
 * stream_virtual_output creates the output and a PipeWire feed. Closing the
 * stream (or exiting) destroys only that output. Physical displays are untouched.
 *
 * KWin 6.7 often replies "Could not find output" from workspace()->findOutput
 * right after createVirtualOutput. The output still exists in KScreen. Hold
 * the stream anyway; exiting disconnects Wayland and KWin removes the output.
 * Then enable it with kscreen-doctor so a wl_output is published for capture.
 */
#include "zkde-screencast-unstable-v1.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define MAX_OUTPUTS 32
#define NAME_LEN 128

struct output_slot {
  struct wl_output *output;
  char name[NAME_LEN];
};

struct app {
  struct wl_display *display;
  struct wl_registry *registry;
  struct zkde_screencast_unstable_v1 *screencast;
  struct zkde_screencast_stream_unstable_v1 *stream;
  uint32_t screencast_version;
  struct output_slot outputs[MAX_OUTPUTS];
  int n_outputs;
  bool created;
  bool failed;
  bool closed;
  char error[256];
  volatile sig_atomic_t stop;
};

static struct app *g_app;

static void
on_signal(int sig) {
  (void) sig;
  if (g_app) {
    g_app->stop = 1;
  }
}

static struct output_slot *
find_slot(struct app *app, struct wl_output *output) {
  for (int i = 0; i < app->n_outputs; ++i) {
    if (app->outputs[i].output == output) {
      return &app->outputs[i];
    }
  }
  return NULL;
}

static void
on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model, int32_t transform) {
  (void) data;
  (void) output;
  (void) x;
  (void) y;
  (void) pw;
  (void) ph;
  (void) subpixel;
  (void) make;
  (void) model;
  (void) transform;
}

static void
on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
  (void) data;
  (void) output;
  (void) flags;
  (void) width;
  (void) height;
  (void) refresh;
}

static void
on_output_done(void *data, struct wl_output *output) {
  (void) data;
  (void) output;
}

static void
on_output_scale(void *data, struct wl_output *output, int32_t factor) {
  (void) data;
  (void) output;
  (void) factor;
}

static void
on_output_name(void *data, struct wl_output *output, const char *name) {
  struct app *app = data;
  struct output_slot *slot = find_slot(app, output);
  if (!slot || !name) {
    return;
  }
  snprintf(slot->name, sizeof(slot->name), "%s", name);
  fprintf(stderr, "output name=%s\n", name);
}

static void
on_output_description(void *data, struct wl_output *output, const char *description) {
  (void) data;
  (void) output;
  (void) description;
}

static const struct wl_output_listener output_listener = {
  .geometry = on_output_geometry,
  .mode = on_output_mode,
  .done = on_output_done,
  .scale = on_output_scale,
  .name = on_output_name,
  .description = on_output_description,
};

static void
bind_output(struct app *app, struct wl_registry *reg, uint32_t name, uint32_t version) {
  if (app->n_outputs >= MAX_OUTPUTS) {
    return;
  }
  uint32_t bind_ver = version < 4 ? version : 4;
  struct wl_output *output = wl_registry_bind(reg, name, &wl_output_interface, bind_ver);
  struct output_slot *slot = &app->outputs[app->n_outputs++];
  memset(slot, 0, sizeof(*slot));
  slot->output = output;
  wl_output_add_listener(output, &output_listener, app);
}

static void
on_registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version) {
  struct app *app = data;
  if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
    uint32_t bind_ver = version < 6 ? version : 6;
    if (bind_ver < 2) {
      fprintf(stderr, "zkde_screencast_unstable_v1 version %u is too old for stream_virtual_output\n", version);
      return;
    }
    app->screencast = wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver);
    app->screencast_version = bind_ver;
    fprintf(stderr, "bound %s v%u\n", interface, bind_ver);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    bind_output(app, reg, name, version);
  }
}

static void
on_registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void) data;
  (void) reg;
  (void) name;
}

static const struct wl_registry_listener registry_listener = {
  .global = on_registry_global,
  .global_remove = on_registry_global_remove,
};

static void
on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream) {
  (void) stream;
  struct app *app = data;
  app->closed = true;
  fprintf(stderr, "stream closed by compositor\n");
}

static void
on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream, uint32_t node) {
  (void) stream;
  struct app *app = data;
  app->created = true;
  fprintf(stderr, "stream created node=%u\n", node);
}

static void
on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream, const char *error) {
  (void) stream;
  struct app *app = data;
  app->failed = true;
  snprintf(app->error, sizeof(app->error), "%s", error ? error : "unknown");
  fprintf(stderr, "stream failed: %s\n", app->error);
}

static void
on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream, uint32_t hi, uint32_t lo) {
  (void) data;
  (void) stream;
  fprintf(stderr, "stream serial=%u:%u\n", hi, lo);
}

static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
  .closed = on_stream_closed,
  .created = on_stream_created,
  .failed = on_stream_failed,
  .serial = on_stream_serial,
};

static int
dispatch_timeout(struct app *app, int timeout_ms) {
  struct pollfd pfd = {
    .fd = wl_display_get_fd(app->display),
    .events = POLLIN,
  };
  while (wl_display_prepare_read(app->display) != 0) {
    if (wl_display_dispatch_pending(app->display) < 0) {
      return -1;
    }
  }
  wl_display_flush(app->display);
  int rc = poll(&pfd, 1, timeout_ms);
  if (rc > 0 && (pfd.revents & POLLIN)) {
    if (wl_display_read_events(app->display) < 0) {
      return -1;
    }
  } else {
    wl_display_cancel_read(app->display);
  }
  if (wl_display_dispatch_pending(app->display) < 0) {
    return -1;
  }
  return rc;
}

static void
print_outputs(struct app *app, const char *label) {
  fprintf(stderr, "%s:", label);
  for (int i = 0; i < app->n_outputs; ++i) {
    fprintf(stderr, " %s", app->outputs[i].name[0] ? app->outputs[i].name : "(unnamed)");
  }
  fprintf(stderr, "\n");
}

static bool
name_is_new(struct app *app, const char *name, const char **before, int n_before) {
  if (!name || !name[0]) {
    return false;
  }
  for (int j = 0; j < n_before; ++j) {
    if (strcmp(name, before[j]) == 0) {
      return false;
    }
  }
  (void) app;
  return true;
}

static const char *
pick_new_name(struct app *app, const char **before, int n_before, const char *requested) {
  char virtual_name[NAME_LEN];
  snprintf(virtual_name, sizeof(virtual_name), "Virtual-%s", requested);
  for (int i = 0; i < app->n_outputs; ++i) {
    const char *name = app->outputs[i].name;
    if (name_is_new(app, name, before, n_before) && strcmp(name, virtual_name) == 0) {
      return name;
    }
  }
  for (int i = 0; i < app->n_outputs; ++i) {
    const char *name = app->outputs[i].name;
    if (name_is_new(app, name, before, n_before) && strcmp(name, requested) == 0) {
      return name;
    }
  }
  for (int i = 0; i < app->n_outputs; ++i) {
    const char *name = app->outputs[i].name;
    if (name_is_new(app, name, before, n_before)) {
      return name;
    }
  }
  return requested;
}

static bool
kscreen_has_output(const char *needle) {
  if (!needle || !needle[0]) {
    return false;
  }
  FILE *pipe = popen("kscreen-doctor -o 2>/dev/null", "r");
  if (!pipe) {
    return false;
  }
  char buf[4096];
  bool found = false;
  while (fgets(buf, sizeof(buf), pipe)) {
    if (strstr(buf, needle)) {
      found = true;
      break;
    }
  }
  pclose(pipe);
  return found;
}

static void
usage(const char *argv0) {
  fprintf(stderr,
    "Usage: %s [--name NAME] [--width W] [--height H] [--scale S] [--pid-file PATH]\n"
    "Hold a KWin virtual output until SIGTERM/SIGINT.\n",
    argv0);
}

int
main(int argc, char **argv) {
  const char *name = "sunshine-ds";
  int width = 1280;
  int height = 800;
  double scale = 1.0;
  const char *pid_file = NULL;

  static const struct option opts[] = {
    {"name", required_argument, NULL, 'n'},
    {"width", required_argument, NULL, 'W'},
    {"height", required_argument, NULL, 'H'},
    {"scale", required_argument, NULL, 's'},
    {"pid-file", required_argument, NULL, 'p'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
  };

  int c;
  while ((c = getopt_long(argc, argv, "n:W:H:s:p:h", opts, NULL)) != -1) {
    switch (c) {
      case 'n':
        name = optarg;
        break;
      case 'W':
        width = atoi(optarg);
        break;
      case 'H':
        height = atoi(optarg);
        break;
      case 's':
        scale = atof(optarg);
        break;
      case 'p':
        pid_file = optarg;
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      default:
        usage(argv[0]);
        return 2;
    }
  }

  if (width <= 0 || height <= 0 || scale <= 0) {
    fprintf(stderr, "width, height, and scale must be positive\n");
    return 2;
  }

  struct app app = {0};
  g_app = &app;

  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  if (pid_file) {
    FILE *fp = fopen(pid_file, "w");
    if (fp) {
      fprintf(fp, "%d\n", getpid());
      fclose(fp);
    }
  }

  const char *wl_name = getenv("WAYLAND_DISPLAY");
  app.display = wl_display_connect(wl_name);
  if (!app.display) {
    fprintf(stderr, "cannot connect to WAYLAND_DISPLAY=%s\n", wl_name ? wl_name : "(unset)");
    return 1;
  }

  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &registry_listener, &app);
  wl_display_roundtrip(app.display);
  wl_display_roundtrip(app.display);

  if (!app.screencast) {
    fprintf(stderr,
      "zkde_screencast_unstable_v1 is not in the registry.\n"
      "Install a desktop file whose Exec= is this binary (%s) with\n"
      "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1, then retry.\n"
      "kbuildsycoca6 --noincremental may be required.\n",
      argv[0]);
    wl_display_disconnect(app.display);
    return 1;
  }

  print_outputs(&app, "outputs-before");
  const char *before[MAX_OUTPUTS];
  int n_before = 0;
  for (int i = 0; i < app.n_outputs; ++i) {
    if (app.outputs[i].name[0]) {
      before[n_before++] = app.outputs[i].name;
    }
  }

  app.stream = zkde_screencast_unstable_v1_stream_virtual_output(
    app.screencast,
    name,
    width,
    height,
    wl_fixed_from_double(scale),
    ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
  );
  if (!app.stream) {
    fprintf(stderr, "stream_virtual_output failed to marshal\n");
    return 1;
  }
  zkde_screencast_stream_unstable_v1_add_listener(app.stream, &stream_listener, &app);
  wl_display_flush(app.display);

  const int deadline_ms = 8000;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (!app.created && !app.failed && !app.stop) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int elapsed = (int) ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
    if (elapsed >= deadline_ms) {
      break;
    }
    if (dispatch_timeout(&app, deadline_ms - elapsed) < 0) {
      fprintf(stderr, "wayland dispatch failed: %s\n", strerror(errno));
      return 1;
    }
  }

  /*
   * KWin 6.7 creates the DRM virtual output, then immediately
   * workspace()->findOutput() for the LogicalOutput. That lookup often
   * fails with "Could not find output" even though KScreen already has
   * Virtual-{name}. Exiting the helper disconnects Wayland and removes it.
   */
  char virtual_name[NAME_LEN];
  snprintf(virtual_name, sizeof(virtual_name), "Virtual-%s", name);
  if (app.failed) {
    fprintf(stderr, "screencast failed: %s (holding stream; waiting for %s)\n", app.error, virtual_name);
  } else if (!app.created) {
    fprintf(stderr, "no created event yet; waiting for %s\n", virtual_name);
  }

  const char *resolved = NULL;
  for (int i = 0; i < 80 && !app.stop && !app.closed; ++i) {
    dispatch_timeout(&app, 100);
    wl_display_roundtrip(app.display);
    const char *from_wl = pick_new_name(&app, before, n_before, name);
    if (app.n_outputs > n_before && from_wl && strcmp(from_wl, name) != 0) {
      resolved = from_wl;
      break;
    }
    if (kscreen_has_output(virtual_name)) {
      resolved = virtual_name;
      break;
    }
  }

  print_outputs(&app, "outputs-after");
  if (!resolved) {
    resolved = pick_new_name(&app, before, n_before, name);
  }
  if (!kscreen_has_output(resolved) && app.n_outputs <= n_before) {
    fprintf(stderr, "KWin never advertised a new output for %s\n", name);
    return 1;
  }
  printf("READY name=%s requested=%s %dx%d\n", resolved, name, width, height);
  fflush(stdout);
  fflush(stderr);

  /* KWin 6.7 often leaves the new output connected-but-disabled until KScreen enables it.
   * Enabling publishes the wl_output Sunshine's kwin grab needs. HDMI stays on. */
  {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
      "kscreen-doctor output.%s.enable output.%s.position.1920,0 >/dev/null 2>&1",
      resolved, resolved);
    if (system(cmd) != 0) {
      fprintf(stderr, "kscreen-doctor enable %s failed; enable it manually\n", resolved);
    }
  }

  while (!app.stop && !app.closed) {
    if (dispatch_timeout(&app, 500) < 0) {
      break;
    }
  }

  fprintf(stderr, "releasing virtual output %s\n", resolved);
  zkde_screencast_stream_unstable_v1_close(app.stream);
  app.stream = NULL;
  zkde_screencast_unstable_v1_destroy(app.screencast);
  app.screencast = NULL;
  wl_display_flush(app.display);
  wl_display_disconnect(app.display);
  return 0;
}
