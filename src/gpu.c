#include "gpu.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VENDOR_NVIDIA 0x10de
#define VENDOR_AMD    0x1002
#define VENDOR_INTEL  0x8086

static unsigned int read_vendor(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	unsigned int v = 0;
	fscanf(f, "%x", &v);
	fclose(f);
	return v;
}

struct wlgame_gpu_info gpu_detect_and_apply(void) {
	struct wlgame_gpu_info info = {0};

	glob_t g;
	if (glob("/sys/class/drm/card*/device/vendor", 0, NULL, &g) != 0)
		return info;

	for (size_t i = 0; i < g.gl_pathc; i++) {
		unsigned int v = read_vendor(g.gl_pathv[i]);
		switch (v) {
		case VENDOR_NVIDIA: info.nvidia = true; break;
		case VENDOR_AMD:    info.amd    = true; break;
		case VENDOR_INTEL:  info.intel  = true; break;
		}
	}
	globfree(&g);

	// Priority: NVIDIA > AMD > Intel
	if (info.nvidia)      info.vendor = GPU_VENDOR_NVIDIA;
	else if (info.amd)    info.vendor = GPU_VENDOR_AMD;
	else if (info.intel)  info.vendor = GPU_VENDOR_INTEL;

	/* ── NVIDIA ─────────────────────────────────────────────────────── */
	if (info.nvidia) {
		// Vulkan renderer (required for explicit sync + DMA-BUF)
		setenv("WLR_RENDERER",              "vulkan",    0);
		setenv("GBM_BACKEND",               "nvidia-drm",0);
		setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia",    0);
		setenv("__GL_SYNC_TO_VBLANK",       "0",         0);

		// Proton / DXVK: enable NVAPI so games see DLSS, RTX, latency SDK
		setenv("PROTON_ENABLE_NVAPI",       "1",         0);
		setenv("DXVK_ENABLE_NVAPI",         "1",         0);

		// NGX updater lets Proton pull DLSS/DLSS-G/RayReconstruction updates
		setenv("PROTON_ENABLE_NGX_UPDATER", "1",         0);

		// DLDSR: 2.25× virtual resolution downscaled to display (driver-side)
		setenv("__GL_DLDSR_MULTIPLIER",     "2.25",      0);

		// RTX Smooth Motion (driver frame interpolation, Blackwell+)
		setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", "1",    0);

		// VA-API via nvidia-vaapi-driver
		setenv("LIBVA_DRIVER_NAME",         "nvidia",    0);
		setenv("NVD_BACKEND",               "direct",    0);
	}

	/* ── AMD ─────────────────────────────────────────────────────────── */
	if (info.amd) {
		// Prefer Mesa RADV over AMDVLK — better gaming + open-source
		setenv("AMD_VULKAN_ICD",  "RADV",  0);

		// RADV perf features:
		//   sam   = Smart Access Memory / ReBAR
		//   ngg   = Next Generation Geometry (mesh shaders path)
		//   nggc  = NGG culling pass
		//   rebar = ReBAR explicit enable
		setenv("RADV_PERFTEST", "sam,ngg,nggc,rebar", 0);

		// Async pipeline compilation — reduces stutters on first shader encounter
		setenv("DXVK_ASYNC",                "1", 0);
		setenv("PROTON_RADV_ENABLE_ASYNC_PIPELINE", "1", 0);

		// Expose OpenGL 4.6 / GLSL 460 to apps that check
		setenv("MESA_GL_VERSION_OVERRIDE",   "4.6",  0);
		setenv("MESA_GLSL_VERSION_OVERRIDE", "460",  0);

		// VA-API via Mesa
		setenv("LIBVA_DRIVER_NAME", "radeonsi", 0);
	}

	/* ── Intel ───────────────────────────────────────────────────────── */
	if (info.intel) {
		setenv("LIBVA_DRIVER_NAME", "iHD", 0);
		setenv("WLR_RENDERER",      "vulkan", 0);
	}

	return info;
}

void gpu_print_info(const struct wlgame_gpu_info *info) {
	const char *name = "none";
	switch (info->vendor) {
	case GPU_VENDOR_NVIDIA: name = "NVIDIA"; break;
	case GPU_VENDOR_AMD:    name = "AMD";    break;
	case GPU_VENDOR_INTEL:  name = "Intel";  break;
	default:                name = "unknown";break;
	}
	fprintf(stderr, "[wlgame] GPU: %s%s%s%s\n", name,
		info->nvidia ? " (NVAPI+DLSS+RTX-SM env set)" : "",
		info->amd    ? " (RADV+ReBAR+async-pipeline env set)" : "",
		info->intel  ? " (iHD env set)" : "");
}
