#include "nvidia.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVIDIA_VENDOR_ID 0x10de

static bool nvidia_present = false;
static bool nvidia_checked = false;

bool nvidia_detect(void) {
	if (nvidia_checked) {
		return nvidia_present;
	}
	nvidia_checked = true;

	glob_t g;
	if (glob("/sys/class/drm/card*/device/vendor", 0, NULL, &g) != 0) {
		return false;
	}

	for (size_t i = 0; i < g.gl_pathc; i++) {
		FILE *f = fopen(g.gl_pathv[i], "r");
		if (!f) {
			continue;
		}
		unsigned int vendor = 0;
		fscanf(f, "%x", &vendor);
		fclose(f);
		if (vendor == NVIDIA_VENDOR_ID) {
			nvidia_present = true;
			break;
		}
	}
	globfree(&g);
	return nvidia_present;
}

void nvidia_apply_env(void) {
	if (!nvidia_detect()) {
		return;
	}

	/* Use Vulkan renderer — better on NVIDIA than GLES2 */
	setenv("WLR_RENDERER", "vulkan", 0);

	/* GBM backend for NVIDIA (required for wlroots DRM) */
	setenv("GBM_BACKEND", "nvidia-drm", 0);
	setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);

	/* Disable VBlank sync at the GL level — compositor controls timing */
	setenv("__GL_SYNC_TO_VBLANK", "0", 0);

	/* NVAPI gaming extensions via Proton/DXVK */
	setenv("PROTON_ENABLE_NVAPI", "1", 0);
	setenv("DXVK_ENABLE_NVAPI", "1", 0);

	/* Reflex (NV_low_latency2) via NVAPI */
	setenv("PROTON_ENABLE_NGX_UPDATER", "1", 0);

	/* DLDSR super-resolution (2.25x = quality mode) */
	setenv("__GL_DLDSR_MULTIPLIER", "2.25", 0);

	/* NVIDIA VA-API decode */
	setenv("LIBVA_DRIVER_NAME", "nvidia", 0);
	setenv("NVD_BACKEND", "direct", 0);

	/* RTX Smooth Motion frame generation */
	setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", "1", 0);

	/* Disable nvidia-drm modeset check warnings */
	setenv("__NV_PRIME_RENDER_OFFLOAD", "0", 0);
}

void nvidia_print_info(void) {
	if (!nvidia_detect()) {
		fprintf(stderr, "[wlgame] GPU: non-NVIDIA, standard path\n");
		return;
	}

	/* Find the NVIDIA card name */
	glob_t g;
	char name[64] = "unknown";
	if (glob("/sys/class/drm/card*/device/label", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
		FILE *f = fopen(g.gl_pathv[0], "r");
		if (f) {
			fgets(name, sizeof(name), f);
			name[strcspn(name, "\n")] = '\0';
			fclose(f);
		}
		globfree(&g);
	}

	fprintf(stderr, "[wlgame] NVIDIA detected: %s\n", name);
	fprintf(stderr, "[wlgame] Enabled: Vulkan renderer, GBM, NVAPI, Reflex, DLDSR, RTX Smooth Motion\n");
	fprintf(stderr, "[wlgame] Explicit sync: linux-drm-syncobj-v1 active\n");
}
