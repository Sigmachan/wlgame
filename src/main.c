
#include "server.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

static struct wlgame_server *g_server = NULL;

static void sigint_handler(int sig) {
	(void)sig;
	if (g_server) {
		wl_display_terminate(g_server->display);
	}
}

static void print_usage(const char *prog) {
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"wlgame — gaming-focused Wayland compositor\n"
		"\n"
		"Options:\n"
		"  -t, --tearing      Allow tearing for game surfaces (default: off)\n"
		"  -d, --debug        Enable debug logging\n"
		"  -h, --help         Show this help\n"
		"\n"
		"Protocols enabled:\n"
		"  wp_color_manager_v1       (HDR / wide gamut)\n"
		"  wp_tearing_control_v1     (game opt-in async flips)\n"
		"  wp_content_type_v1        (game surface detection)\n"
		"  wp_color_representation   (YUV / pixel format color)\n"
		"  wp_fractional_scale_v1    (FSR / NIS scaling foundation)\n"
		"  wp_alpha_modifier_v1\n"
		"  wp_cursor_shape_v1\n"
		"  linux-drm-syncobj-v1      (explicit GPU sync, NVIDIA fix)\n"
		"  linux-dmabuf-v1\n"
		"  wp_viewporter\n"
		"  wp_presentation_time\n"
		"  wlr_output_management_v1\n"
		"  xdg-shell v6\n"
		"  ext-xdg-decoration-v1\n"
		"  wlr-layer-shell-v1\n"
		"  ext-screencopy-v1\n"
		"  ext-foreign-toplevel-list-v1\n"
		"  xdg-activation-v1\n"
		"\n"
		"NVIDIA auto-detection:\n"
		"  Detects NVIDIA GPU via /sys/class/drm/card*/device/vendor.\n"
		"  When found: sets WLR_RENDERER=vulkan, GBM_BACKEND=nvidia-drm,\n"
		"  PROTON_ENABLE_NVAPI=1, DXVK_ENABLE_NVAPI=1, __GL_DLDSR_MULTIPLIER=2.25,\n"
		"  NVPRESENT_ENABLE_SMOOTH_MOTION=1 (RTX Smooth Motion).\n"
		"  linux-drm-syncobj-v1 (explicit sync) is always active when DRM available.\n",
		prog);
}

int main(int argc, char *argv[]) {
	bool tearing = false;
	bool debug = false;

	static const struct option longopts[] = {
		{ "tearing", no_argument, NULL, 't' },
		{ "debug",   no_argument, NULL, 'd' },
		{ "help",    no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "tdh", longopts, NULL)) != -1) {
		switch (c) {
		case 't': tearing = true; break;
		case 'd': debug = true; break;
		case 'h': print_usage(argv[0]); return 0;
		default:  print_usage(argv[0]); return 1;
		}
	}

	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

	struct wlgame_server server = {0};
	g_server = &server;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	server_init(&server, tearing);
	server_run(&server);
	server_fini(&server);

	return 0;
}
