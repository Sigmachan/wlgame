
#include "server.h"
#include "upscale.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

static struct wlgame_server *g_server = NULL;

static void sigint_handler(int sig) {
	(void)sig;
	if (g_server) wl_display_terminate(g_server->display);
}

static void print_usage(const char *prog) {
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"wlgame — gaming-focused Wayland compositor\n"
		"\n"
		"Options:\n"
		"  -t, --tearing              Allow tearing for game surfaces\n"
		"  -s, --scale <mode>         Post-process upscaler: none|cas|nis|fsr1 (default: auto)\n"
		"      --sharpness <0.0-1.0>  Sharpening intensity (default: 0.8)\n"
		"      --shader-dir <path>    Override SPIR-V shader directory\n"
		"  -d, --debug                Enable debug logging\n"
		"  -h, --help                 Show this help\n"
		"\n"
		"Upscaling modes:\n"
		"  cas   AMD Contrast Adaptive Sharpening    (sharpening, any GPU)\n"
		"  nis   NVIDIA Image Scaling                (bicubic + sharpening, any GPU)\n"
		"  fsr1  AMD FidelityFX Super Resolution 1.0 (edge-adaptive + RCAS)\n"
		"  none  Disabled\n"
		"  auto  CAS on AMD, NIS on NVIDIA (default when GPU detected)\n"
		"\n"
		"Staging Wayland protocols enabled:\n"
		"  wp_color_manager_v1           HDR / BT.2020 / PQ / DCI-P3\n"
		"  wp_tearing_control_v1         Async page flips for games\n"
		"  wp_content_type_v1            Game surface detection\n"
		"  wp_color_representation_v1    YUV / pixel format color space\n"
		"  wp_fractional_scale_v1        Sub-pixel scaling\n"
		"  wp_alpha_modifier_v1\n"
		"  wp_cursor_shape_v1\n"
		"  linux-drm-syncobj-v1          Explicit GPU sync (NVIDIA fix)\n"
		"  ext-image-copy-capture-v1     Screencopy (OBS, overlays)\n"
		"  ext-session-lock-v1           Screen lock\n"
		"  ext-workspace-v1              Workspace management\n"
		"  xdg-security-context-v1       Flatpak sandbox\n"
		"\n"
		"Auto GPU detection:\n"
		"  NVIDIA: sets WLR_RENDERER=vulkan, PROTON_ENABLE_NVAPI=1, DXVK_ENABLE_NVAPI=1,\n"
		"          PROTON_ENABLE_NGX_UPDATER=1 (DLSS), __GL_DLDSR_MULTIPLIER=2.25,\n"
		"          NVPRESENT_ENABLE_SMOOTH_MOTION=1 (RTX Smooth Motion)\n"
		"  AMD:    sets AMD_VULKAN_ICD=RADV, RADV_PERFTEST=sam,ngg,nggc,rebar,\n"
		"          DXVK_ASYNC=1, PROTON_RADV_ENABLE_ASYNC_PIPELINE=1\n",
		prog);
}

int main(int argc, char *argv[]) {
	bool tearing   = false;
	bool debug     = false;
	float sharpness = 0.8f;
	enum wlgame_upscale_mode upscale_mode = UPSCALE_NONE;  /* NONE = auto */
	const char *shader_dir = getenv("WLGAME_SHADER_DIR");
	if (!shader_dir) shader_dir = WLGAME_SHADER_DIR;

	static const struct option longopts[] = {
		{ "tearing",    no_argument,       NULL, 't' },
		{ "scale",      required_argument, NULL, 's' },
		{ "sharpness",  required_argument, NULL, 'S' },
		{ "shader-dir", required_argument, NULL, 'D' },
		{ "debug",      no_argument,       NULL, 'd' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "ts:dh", longopts, NULL)) != -1) {
		switch (c) {
		case 't': tearing = true; break;
		case 's':
			upscale_mode = upscale_mode_from_str(optarg);
			if (upscale_mode == UPSCALE_NONE && strcmp(optarg, "none") != 0 &&
			    strcmp(optarg, "auto") != 0) {
				fprintf(stderr, "Unknown scale mode '%s'. Use: none|cas|nis|fsr1|auto\n", optarg);
				return 1;
			}
			break;
		case 'S': sharpness = strtof(optarg, NULL); break;
		case 'D': shader_dir = optarg; break;
		case 'd': debug = true; break;
		case 'h': print_usage(argv[0]); return 0;
		default:  print_usage(argv[0]); return 1;
		}
	}

	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

	struct wlgame_server server = {0};
	g_server = &server;

	signal(SIGINT,  sigint_handler);
	signal(SIGTERM, sigint_handler);

	server_init(&server, tearing, upscale_mode, sharpness, shader_dir);
	server_run(&server);
	server_fini(&server);

	return 0;
}
