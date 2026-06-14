#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_renderer.h>

enum wlgame_upscale_mode {
	UPSCALE_NONE = 0,
	UPSCALE_CAS,        /* AMD Contrast Adaptive Sharpening (sharpening, no resize) */
	UPSCALE_NIS,        /* NVIDIA Image Scaling (bicubic upscale + sharpening) */
	UPSCALE_FSR1,       /* AMD FSR 1.0 (EASU upscale + RCAS sharpening) */
};

struct wlgame_upscale {
	enum wlgame_upscale_mode mode;
	float sharpness;   /* [0.0, 1.0] */
	bool  active;      /* false when Vulkan renderer not in use */

	VkDevice         device;
	VkPhysicalDevice phys_device;
	uint32_t         queue_family;
	VkQueue          queue;

	VkCommandPool    cmd_pool;
	VkCommandBuffer  cmd_buf;
	VkFence          fence;
	VkSampler        sampler;

	/* Descriptor layout: set 0 binding 0 = combined image sampler,
	 *                      set 0 binding 1 = storage image */
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool      desc_pool;
	VkDescriptorSet       desc_set;
	VkPipelineLayout      pipeline_layout;

	/* One pipeline per mode */
	VkPipeline cas_pipeline;
	VkPipeline nis_pipeline;
	VkPipeline fsr1_easu_pipeline;
	VkPipeline fsr1_rcas_pipeline;

	/* Persistent images — recreated when frame size changes */
	VkImage        input_image,  output_image;
	VkDeviceMemory input_mem,    output_mem;
	VkImageView    input_view,   output_view;
	int            output_dma_fd;
	uint32_t       last_in_w, last_in_h;
	uint32_t       last_out_w, last_out_h;
	VkFormat       last_vk_fmt;

	const char *shader_dir;
};

/* Init: query Vulkan device from wlroots Vulkan renderer, load shaders, build pipelines.
 * Returns true even if Vulkan renderer not active (mode stays NONE, no-op on apply). */
bool upscale_init(struct wlgame_upscale *up, struct wlr_renderer *renderer,
                  enum wlgame_upscale_mode mode, float sharpness,
                  const char *shader_dir);

/* Upscale `in` (a composited scene buffer) to out_w×out_h. Returns a newly
 * created wlr_buffer that the caller owns: pass it to wlr_output_state_set_buffer()
 * then wlr_buffer_drop() once committed. Returns NULL if upscaling is inactive or
 * failed (caller should present `in` unchanged). */
struct wlr_buffer *upscale_run(struct wlgame_upscale *up, struct wlr_buffer *in,
                               uint32_t out_w, uint32_t out_h);

void upscale_fini(struct wlgame_upscale *up);

const char *upscale_mode_name(enum wlgame_upscale_mode mode);
enum wlgame_upscale_mode upscale_mode_from_str(const char *s);
