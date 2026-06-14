
#include "server.h"
#include "config.h"
#include "upscale.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <wlr/util/log.h>

static struct wlgame_server *g_server = NULL;

static void sigint_handler(int sig) {
	(void)sig;
	if (g_server) wl_display_terminate(g_server->display);
}

/* Parse "WxH" or "WxH@R" into a config. Returns false on malformed input. */
static bool parse_geometry(const char *s, int *w, int *h, int *rate) {
	int rr = 0;
	int n = sscanf(s, "%dx%d@%d", w, h, &rr);
	if (n < 2 || *w <= 0 || *h <= 0) return false;
	if (rate && n >= 3) *rate = rr;
	return true;
}

static void print_usage(const char *prog) {
	fprintf(stderr,
		"Usage: %s [options] [-- command [args...]]\n"
		"\n"
		"wlgame — gaming-focused Wayland compositor / per-game gamescope-style wrapper\n"
		"\n"
		"Run a single game and exit when it does:\n"
		"  %s -o 3840x2160@120 -F fsr1 -- %%command%%\n"
		"Nested inside your desktop (Wayland/X11 session):\n"
		"  %s -o 2560x1440 -f -m -- vkcube\n"
		"\n"
		"Output / display:\n"
		"  -o, --output WxH[@Hz]      Output mode (DRM) or window size (nested)\n"
		"  -r, --render WxH           Internal render resolution; the game renders\n"
		"                             here and is upscaled to the output (needs nis/fsr1)\n"
		"  -f, --fullscreen           Request a fullscreen window when nested\n"
		"      --fps <N>              Cap presentation/frame-callbacks to N Hz\n"
		"  -t, --tearing              Allow tearing (async page-flips) for games\n"
		"\n"
		"Scaling:\n"
		"  -F, --filter <mode>        Upscaler: none|cas|nis|fsr1 (default: auto)\n"
		"  -s, --scale <mode>         Alias of --filter\n"
		"      --sharpness <0.0-1.0>  Sharpening intensity (default: 0.8)\n"
		"      --shader-dir <path>    Override SPIR-V shader directory\n"
		"\n"
		"Game integration:\n"
		"  -m, --mangoapp             Auto-spawn the mangoapp performance overlay\n"
		"      --prefer-wayland       Hint Proton/SDL/Qt to use native Wayland\n"
		"\n"
		"Multi-GPU:\n"
		"  -g, --gpu <sel>            Render on this GPU, scan out on the display GPU\n"
		"                             (reverse-PRIME). sel: nvidia|amd|intel|discrete\n"
		"                             or a /dev/dri/renderD* path\n"
		"\n"
		"Misc:\n"
		"  -d, --debug                Enable debug logging\n"
		"  -h, --help                 Show this help\n"
		"\n"
		"Upscaling modes:\n"
		"  cas   AMD Contrast Adaptive Sharpening    (sharpening, any GPU)\n"
		"  nis   NVIDIA Image Scaling                (bicubic + sharpening)\n"
		"  fsr1  AMD FidelityFX Super Resolution 1.0 (edge-adaptive + RCAS)\n"
		"  none  Disabled        auto  CAS on AMD, NIS on NVIDIA\n"
		"\n"
		"When a `-- command` is given, wlgame launches it once the compositor is\n"
		"up, forwards WAYLAND_DISPLAY/DISPLAY, and exits with the game's exit code\n"
		"when it quits — drop it straight into Steam launch options.\n",
		prog, prog, prog);
}

int main(int argc, char *argv[]) {
	struct wlgame_config cfg = {
		.sharpness     = 0.8f,
		.upscale_mode  = UPSCALE_NONE,  /* NONE = auto-pick by GPU */
		.shader_dir    = getenv("WLGAME_SHADER_DIR"),
	};
	if (!cfg.shader_dir) cfg.shader_dir = WLGAME_SHADER_DIR;

	enum {
		OPT_FPS = 256, OPT_SHARPNESS, OPT_SHADER_DIR, OPT_PREFER_WAYLAND,
	};
	static const struct option longopts[] = {
		{ "output",         required_argument, NULL, 'o' },
		{ "render",         required_argument, NULL, 'r' },
		{ "fullscreen",     no_argument,       NULL, 'f' },
		{ "fps",            required_argument, NULL, OPT_FPS },
		{ "tearing",        no_argument,       NULL, 't' },
		{ "filter",         required_argument, NULL, 'F' },
		{ "scale",          required_argument, NULL, 's' },
		{ "sharpness",      required_argument, NULL, OPT_SHARPNESS },
		{ "shader-dir",     required_argument, NULL, OPT_SHADER_DIR },
		{ "mangoapp",       no_argument,       NULL, 'm' },
		{ "gpu",            required_argument, NULL, 'g' },
		{ "prefer-wayland", no_argument,       NULL, OPT_PREFER_WAYLAND },
		{ "debug",          no_argument,       NULL, 'd' },
		{ "help",           no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int c;
	/* leading '+' => stop at the first non-option so `--` ends our parsing */
	while ((c = getopt_long(argc, argv, "+o:r:fF:s:tmg:dh", longopts, NULL)) != -1) {
		switch (c) {
		case 'o':
			if (!parse_geometry(optarg, &cfg.output_width,
			                    &cfg.output_height, &cfg.output_rate)) {
				fprintf(stderr, "Invalid --output '%s' (expected WxH or WxH@Hz)\n", optarg);
				return 1;
			}
			break;
		case 'r':
			if (!parse_geometry(optarg, &cfg.render_width,
			                    &cfg.render_height, NULL)) {
				fprintf(stderr, "Invalid --render '%s' (expected WxH)\n", optarg);
				return 1;
			}
			break;
		case 'f': cfg.fullscreen = true; break;
		case OPT_FPS:
			cfg.fps_limit = atoi(optarg);
			if (cfg.fps_limit < 0) cfg.fps_limit = 0;
			break;
		case 't': cfg.tearing = true; break;
		case 'F':
		case 's':
			cfg.upscale_mode = upscale_mode_from_str(optarg);
			if (cfg.upscale_mode == UPSCALE_NONE &&
			    strcmp(optarg, "none") != 0 && strcmp(optarg, "auto") != 0) {
				fprintf(stderr, "Unknown filter '%s'. Use: none|cas|nis|fsr1|auto\n", optarg);
				return 1;
			}
			break;
		case OPT_SHARPNESS:    cfg.sharpness = strtof(optarg, NULL); break;
		case OPT_SHADER_DIR:   cfg.shader_dir = optarg; break;
		case 'm':              cfg.mangoapp = true; break;
		case 'g':              cfg.render_gpu = optarg; break;
		case OPT_PREFER_WAYLAND: cfg.prefer_wayland = true; break;
		case 'd':              cfg.debug = true; break;
		case 'h':              print_usage(argv[0]); return 0;
		default:               print_usage(argv[0]); return 1;
		}
	}

	/* Everything after the options (i.e. after `--`) is the game command. */
	if (optind < argc) {
		cfg.child_argv = &argv[optind];
	}

	wlr_log_init(cfg.debug ? WLR_DEBUG : WLR_INFO, NULL);

	struct wlgame_server server = {0};
	g_server = &server;

	signal(SIGINT,  sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGPIPE, SIG_IGN);

	server_init(&server, &cfg);
	server_run(&server);
	server_fini(&server);

	/* Propagate the wrapped game's exit status, gamescope-style. */
	if (server.child_exited) {
		if (WIFEXITED(server.child_status))  return WEXITSTATUS(server.child_status);
		if (WIFSIGNALED(server.child_status)) return 128 + WTERMSIG(server.child_status);
	}
	return 0;
}
