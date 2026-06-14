#pragma once
#include <stdbool.h>

enum wlgame_gpu_vendor {
	GPU_VENDOR_NONE   = 0,
	GPU_VENDOR_NVIDIA = 1,
	GPU_VENDOR_AMD    = 2,
	GPU_VENDOR_INTEL  = 3,
};

struct wlgame_gpu_info {
	enum wlgame_gpu_vendor vendor;
	bool nvidia;
	bool amd;
	bool intel;
};

/* Detect primary GPU vendor via /sys/class/drm. Sets env vars for found GPU.
 * Call before creating the wlr_backend — env vars must be set before DRM opens. */
struct wlgame_gpu_info gpu_detect_and_apply(void);
void gpu_print_info(const struct wlgame_gpu_info *info);

/* Resolve a render-GPU selector ("nvidia"/"amd"/"intel"/"discrete" or an
 * explicit /dev/dri/renderD* path) to a render-node path. Returns a malloc'd
 * string the caller must free, or NULL if no match. */
char *gpu_resolve_render_node(const char *want);
