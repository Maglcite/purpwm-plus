/* Process entry point; signal handlers only publish flags to the main loop. */
#include "host_backend.h"
#include "config.h"

#include <errno.h>

volatile sig_atomic_t purp_signal_screenshot;
volatile sig_atomic_t purp_signal_lock;
volatile sig_atomic_t purp_signal_quit;

static void handle_signal(int signo) {
    if (signo == SIGUSR1) purp_signal_screenshot = 1;
    else if (signo == SIGUSR2) purp_signal_lock = 1;
    else purp_signal_quit = 1;
}

static bool install_signals(void) {
    struct sigaction action = { .sa_handler = handle_signal, .sa_flags = SA_RESTART };
    sigemptyset(&action.sa_mask);
    const int signals[] = { SIGUSR1, SIGUSR2, SIGINT, SIGTERM };
    for (size_t i = 0; i < PURP_ARRAY_LEN(signals); ++i) {
        if (sigaction(signals[i], &action, NULL) < 0) return false;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL) == 0;
}

static void print_usage(void) {
    printf("purpwm+ %s — dwm-inspired Wayland compositor\n\n"
           "Usage: purpwm+ [OPTIONS]\n\n"
           "Options:\n"
           "  --drm                 Drive connected displays directly with DRM/KMS\n"
           "  --winit, --nested      Run in a host Wayland window\n"
           "  --headless             Run offscreen, without a host display or input\n"
           "  --standalone           Fullscreen session; select DRM without a host display\n"
           "  --fullscreen           Alias of --standalone\n"
           "  --spawn-terminal       Launch a terminal after startup\n"
           "  --no-autostart          Skip session helpers and configured autostart commands\n"
           "  -h, --help             Show this help\n\n"
           "Environment:\n"
           "  PURPWM_AUTOSTART=0 disables autostart; PURPWM_STANDALONE enables standalone.\n"
           "  PURPWM_VIRTUAL_MONITORS=name:WxH[@HZ][:SCALE],... defines virtual outputs.\n"
           "  PURPWM_DRM_DEVICE, PURPWM_RESOLUTION=WxH, PURPWM_REFRESH_RATE=HZ select DRM.\n"
           "  PURPWM_SCALE overrides the default display scale.\n\n"
           "Keybindings (Mod = Super/Logo or Alt):\n"
           "  Mod + h                 Show/hide keybinding help (Esc/click closes)\n"
           "  Tap Mod / Mod + p       Toggle Spotlight launcher\n"
           "  Mod + Return            Open terminal\n"
           "  Mod + w / Shift + w     Wi-Fi manager / toggle Wi-Fi\n"
           "  Mod + Shift/Ctrl + b    Bluetooth manager / toggle Bluetooth\n"
           "  Mod + s / Mod + `       Toggle scratchpad terminal\n"
           "  Mod + Shift/Ctrl + l    Lock screen (also SIGUSR2)\n"
           "  Mod + Arrows            Move window / swap tiled windows\n"
           "  Mod + Shift + Arrows    Glide window edge / tiled split\n"
           "  Mod + j / k             Focus next/previous window\n"
           "  Mod + [ / ] (or l)      Decrease/increase master width\n"
           "  Mod + i / d             Increase/decrease master count\n"
           "  Mod + t / m / f         Tile / monocle / floating layout\n"
           "  Mod + u / g             Centered master / grid layout\n"
           "  Mod + Ctrl + Space      Cycle layouts\n"
           "  Mod + Shift + f         Toggle true fullscreen (also Mod + F11)\n"
           "  Mod + o                 Toggle opaque focused window\n"
           "  Mod + = / -             Increase/decrease gaps\n"
           "  Mod + Shift + g         Toggle gaps\n"
           "  Mod + Ctrl + 0          Reset gaps\n"
           "  Mod + Ctrl + = / -      Increase/decrease saved UI scale\n"
           "  Mod + Shift + Space     Toggle floating window\n"
           "  Mod + b                 Toggle status bar\n"
           "  Mod + 1..9              View workspace tag\n"
           "  Mod + Shift + 1..9      Move window to tag\n"
           "  Mod + Escape            Open system monitor\n"
           "  Print                   Screenshot + clipboard (also SIGUSR1)\n"
           "  Shift/Mod + Print       Screenshot focused window\n"
           "  Mod + Left/Right-drag   Move/resize window\n"
           "  Volume pill             Click mute; scroll volume; right-click mixer\n"
           "  Mod + q                 Close window\n"
           "  Mod + Shift + q         Quit compositor (also SIGINT/SIGTERM)\n",
           PURPWM_VERSION);
}

int main(int argc, char **argv) {
    const char *autostart = getenv(PURP_ENV_AUTOSTART);
    purp_options_t opts = {
        .backend = PURP_BACKEND_AUTO,
        .standalone = getenv(PURP_ENV_STANDALONE) != NULL,
        .autostart = !autostart || strcmp(autostart, "0") != 0,
    };
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            print_usage();
            return EXIT_SUCCESS;
        }
        if (!strcmp(arg, "--drm")) opts.backend = PURP_BACKEND_DRM;
        else if (!strcmp(arg, "--winit") || !strcmp(arg, "--nested"))
            opts.backend = PURP_BACKEND_NESTED;
        else if (!strcmp(arg, "--headless")) opts.backend = PURP_BACKEND_HEADLESS;
        else if (!strcmp(arg, "--standalone") || !strcmp(arg, "--fullscreen"))
            opts.standalone = true;
        else if (!strcmp(arg, "--spawn-terminal")) opts.spawn_terminal = true;
        else if (!strcmp(arg, "--no-autostart")) opts.autostart = false;
    }
    if (!install_signals()) {
        PURP_ERROR("Cannot install signal handlers: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    if (opts.backend == PURP_BACKEND_AUTO)
        opts.backend = opts.standalone && !getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")
                     ? PURP_BACKEND_DRM : PURP_BACKEND_NESTED;
    switch (opts.backend) {
    case PURP_BACKEND_DRM: return purp_backend_drm_run(&opts);
    case PURP_BACKEND_HEADLESS: return purp_backend_headless_run(&opts);
    case PURP_BACKEND_AUTO:
    case PURP_BACKEND_NESTED: return purp_backend_nested_run(&opts);
    }
    return EXIT_FAILURE;
}
