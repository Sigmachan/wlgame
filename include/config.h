#pragma once

#include <stdbool.h>
#include "upscale.h"

/* Parsed command-line configuration. Built once in main(), then copied into
 * the server. Treat as immutable after server_init(). */
struct wlgame_config {
	bool  tearing;        /* allow async page-flips for game surfaces        */
	bool  debug;          /* WLR_DEBUG log level                             */
	bool  fullscreen;     /* nested: request a fullscreen window             */
	bool  mangoapp;       /* auto-spawn the mangoapp performance overlay     */
	bool  prefer_wayland; /* opt games into native Wayland (Proton/SDL/Qt)   */

	int   output_width;   /* physical/window width   (0 = auto/preferred)    */
	int   output_height;  /* physical/window height  (0 = auto/preferred)    */
	int   output_rate;    /* refresh in Hz           (0 = preferred)         */
	int   render_width;   /* nested internal render width  (0 = = output)    */
	int   render_height;  /* nested internal render height (0 = = output)    */
	int   fps_limit;      /* present/frame-callback cap in Hz (0 = unlimited) */

	enum wlgame_upscale_mode upscale_mode;
	float sharpness;      /* [0.0, 1.0]                                      */
	const char *shader_dir;

	/* Render GPU when it differs from the display GPU (reverse-PRIME): render
	 * on the discrete card, scan out on the iGPU that has the monitor.
	 * Accepts "nvidia"/"amd"/"intel"/"discrete" or a /dev/dri/renderD* path. */
	const char *render_gpu;

	/* Game command following `--` on the CLI. NULL = bare session (no child,
	 * compositor runs until Super+Q / SIGTERM). */
	char *const *child_argv;
};
