
#include "upscale.h"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/vulkan.h>
#include <wlr/util/log.h>

/* Extension functions loaded at init */
static PFN_vkGetMemoryFdKHR fn_vkGetMemoryFdKHR;

/* ── helpers ───────────────────────────────────────────────────────────── */

static VkFormat drm_to_vk(uint32_t drm) {
	switch (drm) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	default: return VK_FORMAT_B8G8R8A8_UNORM;
	}
}

static uint32_t find_memory_type(VkPhysicalDevice pd,
                                  uint32_t type_bits,
                                  VkMemoryPropertyFlags props) {
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(pd, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) &&
		    (mem.memoryTypes[i].propertyFlags & props) == props)
			return i;
	}
	return UINT32_MAX;
}

static uint8_t *load_spv(const char *dir, const char *name, size_t *out_size) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.comp.spv", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		wlr_log(WLR_ERROR, "[upscale] shader not found: %s", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*out_size = (size_t)ftell(f);
	rewind(f);
	uint8_t *buf = malloc(*out_size);
	fread(buf, 1, *out_size, f);
	fclose(f);
	return buf;
}

static VkShaderModule make_shader(VkDevice dev, const char *dir, const char *name) {
	size_t sz;
	uint8_t *code = load_spv(dir, name, &sz);
	if (!code) return VK_NULL_HANDLE;
	VkShaderModuleCreateInfo ci = {
		.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sz,
		.pCode    = (const uint32_t *)code,
	};
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(dev, &ci, NULL, &m);
	free(code);
	return m;
}

static VkPipeline make_compute(VkDevice dev, VkShaderModule sh, VkPipelineLayout layout) {
	VkComputePipelineCreateInfo ci = {
		.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage  = {
			.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage  = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = sh,
			.pName  = "main",
		},
		.layout = layout,
	};
	VkPipeline p = VK_NULL_HANDLE;
	vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
	return p;
}

/* ── image management ──────────────────────────────────────────────────── */

static void destroy_images(struct wlgame_upscale *up) {
	if (!up->device) return;
	if (up->output_dma_fd >= 0) { close(up->output_dma_fd); up->output_dma_fd = -1; }
	if (up->input_view)  { vkDestroyImageView(up->device, up->input_view,  NULL); up->input_view  = VK_NULL_HANDLE; }
	if (up->output_view) { vkDestroyImageView(up->device, up->output_view, NULL); up->output_view = VK_NULL_HANDLE; }
	if (up->input_image)  { vkDestroyImage(up->device, up->input_image,  NULL); up->input_image  = VK_NULL_HANDLE; }
	if (up->output_image) { vkDestroyImage(up->device, up->output_image, NULL); up->output_image = VK_NULL_HANDLE; }
	if (up->input_mem)  { vkFreeMemory(up->device, up->input_mem,  NULL); up->input_mem  = VK_NULL_HANDLE; }
	if (up->output_mem) { vkFreeMemory(up->device, up->output_mem, NULL); up->output_mem = VK_NULL_HANDLE; }
}

/* Import a DMA-BUF as a Vulkan sampled image (read-only).
 * Caller must NOT close dmabuf->fd[0] — we dup it inside. */
static bool import_dmabuf_image(struct wlgame_upscale *up,
                                 const struct wlr_dmabuf_attributes *dmabuf,
                                 VkFormat fmt) {
	VkSubresourceLayout plane_layout = {
		.offset   = (VkDeviceSize)dmabuf->offset[0],
		.size     = 0,
		.rowPitch = (VkDeviceSize)dmabuf->stride[0],
	};
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod_ci = {
		.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier           = dmabuf->modifier,
		.drmFormatModifierPlaneCount = (uint32_t)dmabuf->n_planes,
		.pPlaneLayouts               = &plane_layout,
	};
	VkExternalMemoryImageCreateInfo ext_ci = {
		.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext       = &mod_ci,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo img_ci = {
		.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext         = &ext_ci,
		.imageType     = VK_IMAGE_TYPE_2D,
		.format        = fmt,
		.extent        = { dmabuf->width, dmabuf->height, 1 },
		.mipLevels     = 1,
		.arrayLayers   = 1,
		.samples       = VK_SAMPLE_COUNT_1_BIT,
		.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage         = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(up->device, &img_ci, NULL, &up->input_image) != VK_SUCCESS)
		return false;

	/* Memory requirements */
	VkImageMemoryRequirementsInfo2 req_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
		.image = up->input_image,
	};
	VkMemoryDedicatedRequirements dedicated_req = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
	};
	VkMemoryRequirements2 mem_req = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
		.pNext = &dedicated_req,
	};
	vkGetImageMemoryRequirements2(up->device, &req_info, &mem_req);

	int fd_dup = dup(dmabuf->fd[0]);
	if (fd_dup < 0) { vkDestroyImage(up->device, up->input_image, NULL); up->input_image = VK_NULL_HANDLE; return false; }

	VkImportMemoryFdInfoKHR import_fd = {
		.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd         = fd_dup,
	};
	VkMemoryDedicatedAllocateInfo ded_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.pNext = &import_fd,
		.image = up->input_image,
	};
	uint32_t mem_type = find_memory_type(up->phys_device,
		mem_req.memoryRequirements.memoryTypeBits, 0);
	VkMemoryAllocateInfo alloc_info = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext           = &ded_alloc,
		.allocationSize  = mem_req.memoryRequirements.size,
		.memoryTypeIndex = mem_type,
	};
	if (vkAllocateMemory(up->device, &alloc_info, NULL, &up->input_mem) != VK_SUCCESS)
		return false;

	vkBindImageMemory(up->device, up->input_image, up->input_mem, 0);

	VkImageViewCreateInfo view_ci = {
		.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image            = up->input_image,
		.viewType         = VK_IMAGE_VIEW_TYPE_2D,
		.format           = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	return vkCreateImageView(up->device, &view_ci, NULL, &up->input_view) == VK_SUCCESS;
}

/* Allocate a LINEAR output VkImage and export its memory as a DMA-BUF fd. */
static bool create_output_image(struct wlgame_upscale *up,
                                 uint32_t w, uint32_t h, VkFormat fmt) {
	VkExternalMemoryImageCreateInfo ext_ci = {
		.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo img_ci = {
		.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext         = &ext_ci,
		.imageType     = VK_IMAGE_TYPE_2D,
		.format        = fmt,
		.extent        = { w, h, 1 },
		.mipLevels     = 1,
		.arrayLayers   = 1,
		.samples       = VK_SAMPLE_COUNT_1_BIT,
		.tiling        = VK_IMAGE_TILING_LINEAR,
		.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(up->device, &img_ci, NULL, &up->output_image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements mem_req;
	vkGetImageMemoryRequirements(up->device, up->output_image, &mem_req);

	VkExportMemoryAllocateInfo export_alloc = {
		.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	uint32_t mem_type = find_memory_type(up->phys_device,
		mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryAllocateInfo alloc_info = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext           = &export_alloc,
		.allocationSize  = mem_req.size,
		.memoryTypeIndex = mem_type,
	};
	if (vkAllocateMemory(up->device, &alloc_info, NULL, &up->output_mem) != VK_SUCCESS)
		return false;
	vkBindImageMemory(up->device, up->output_image, up->output_mem, 0);

	VkImageViewCreateInfo view_ci = {
		.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image            = up->output_image,
		.viewType         = VK_IMAGE_VIEW_TYPE_2D,
		.format           = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (vkCreateImageView(up->device, &view_ci, NULL, &up->output_view) != VK_SUCCESS)
		return false;

	/* Export output memory as DMA-BUF fd */
	VkMemoryGetFdInfoKHR get_fd_info = {
		.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory     = up->output_mem,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	if (!fn_vkGetMemoryFdKHR ||
	    fn_vkGetMemoryFdKHR(up->device, &get_fd_info, &up->output_dma_fd) != VK_SUCCESS) {
		up->output_dma_fd = -1;
		return false;
	}
	return true;
}

/* Update descriptor set to point at (possibly new) image views. */
static void update_descriptors(struct wlgame_upscale *up) {
	VkDescriptorImageInfo sampler_info = {
		.sampler     = up->sampler,
		.imageView   = up->input_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkDescriptorImageInfo storage_info = {
		.imageView   = up->output_view,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};
	VkWriteDescriptorSet writes[2] = {
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = up->desc_set,
			.dstBinding      = 0,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo      = &sampler_info,
		},
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = up->desc_set,
			.dstBinding      = 1,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo      = &storage_info,
		},
	};
	vkUpdateDescriptorSets(up->device, 2, writes, 0, NULL);
}

/* ── public API ────────────────────────────────────────────────────────── */

bool upscale_init(struct wlgame_upscale *up, struct wlr_renderer *renderer,
                  enum wlgame_upscale_mode mode, float sharpness,
                  const char *shader_dir) {
	*up = (struct wlgame_upscale){
		.mode          = mode,
		.sharpness     = sharpness,
		.shader_dir    = shader_dir,
		.output_dma_fd = -1,
	};

	if (mode == UPSCALE_NONE) return true;

	up->device      = wlr_vk_renderer_get_device(renderer);
	up->phys_device = wlr_vk_renderer_get_physical_device(renderer);
	up->queue_family = wlr_vk_renderer_get_queue_family(renderer);

	if (!up->device) {
		wlr_log(WLR_INFO, "[upscale] Vulkan renderer not active — upscaling disabled");
		up->mode = UPSCALE_NONE;
		return true;
	}

	vkGetDeviceQueue(up->device, up->queue_family, 0, &up->queue);

	fn_vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)
		vkGetDeviceProcAddr(up->device, "vkGetMemoryFdKHR");

	/* Command pool */
	VkCommandPoolCreateInfo pool_ci = {
		.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = up->queue_family,
	};
	vkCreateCommandPool(up->device, &pool_ci, NULL, &up->cmd_pool);

	VkCommandBufferAllocateInfo buf_ai = {
		.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool        = up->cmd_pool,
		.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	vkAllocateCommandBuffers(up->device, &buf_ai, &up->cmd_buf);

	VkFenceCreateInfo fence_ci = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	vkCreateFence(up->device, &fence_ci, NULL, &up->fence);

	/* Sampler — linear for NIS/FSR1, nearest for CAS */
	VkSamplerCreateInfo sampler_ci = {
		.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter        = VK_FILTER_LINEAR,
		.minFilter        = VK_FILTER_LINEAR,
		.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxAnisotropy    = 1.0f,
		.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.unnormalizedCoordinates = VK_FALSE,
	};
	vkCreateSampler(up->device, &sampler_ci, NULL, &up->sampler);

	/* Descriptor set layout: binding 0 = sampler, binding 1 = storage image */
	VkDescriptorSetLayoutBinding bindings[2] = {
		{
			.binding         = 0,
			.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		{
			.binding         = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
			.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
		},
	};
	VkDescriptorSetLayoutCreateInfo layout_ci = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings    = bindings,
	};
	vkCreateDescriptorSetLayout(up->device, &layout_ci, NULL, &up->desc_layout);

	/* Push constants: up to 5 floats (sharpness, iw, ih, ow, oh) */
	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset     = 0,
		.size       = sizeof(float) * 5,
	};
	VkPipelineLayoutCreateInfo pl_ci = {
		.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount         = 1,
		.pSetLayouts            = &up->desc_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges    = &push_range,
	};
	vkCreatePipelineLayout(up->device, &pl_ci, NULL, &up->pipeline_layout);

	/* Descriptor pool + set */
	VkDescriptorPoolSize pool_sizes[2] = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1 },
	};
	VkDescriptorPoolCreateInfo dpool_ci = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets       = 1,
		.poolSizeCount = 2,
		.pPoolSizes    = pool_sizes,
	};
	vkCreateDescriptorPool(up->device, &dpool_ci, NULL, &up->desc_pool);

	VkDescriptorSetAllocateInfo ds_ai = {
		.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool     = up->desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts        = &up->desc_layout,
	};
	vkAllocateDescriptorSets(up->device, &ds_ai, &up->desc_set);

	/* Load shader modules and create compute pipelines */
	VkShaderModule cas_sh    = make_shader(up->device, shader_dir, "cas");
	VkShaderModule nis_sh    = make_shader(up->device, shader_dir, "nis");
	VkShaderModule easu_sh   = make_shader(up->device, shader_dir, "fsr1_easu");
	VkShaderModule rcas_sh   = make_shader(up->device, shader_dir, "fsr1_rcas");

	if (cas_sh)  up->cas_pipeline          = make_compute(up->device, cas_sh,  up->pipeline_layout);
	if (nis_sh)  up->nis_pipeline          = make_compute(up->device, nis_sh,  up->pipeline_layout);
	if (easu_sh) up->fsr1_easu_pipeline    = make_compute(up->device, easu_sh, up->pipeline_layout);
	if (rcas_sh) up->fsr1_rcas_pipeline    = make_compute(up->device, rcas_sh, up->pipeline_layout);

	if (cas_sh)  vkDestroyShaderModule(up->device, cas_sh,  NULL);
	if (nis_sh)  vkDestroyShaderModule(up->device, nis_sh,  NULL);
	if (easu_sh) vkDestroyShaderModule(up->device, easu_sh, NULL);
	if (rcas_sh) vkDestroyShaderModule(up->device, rcas_sh, NULL);

	/* Validate we have the pipeline(s) we need */
	bool ok = true;
	switch (mode) {
	case UPSCALE_CAS:  ok = up->cas_pipeline  != VK_NULL_HANDLE; break;
	case UPSCALE_NIS:  ok = up->nis_pipeline  != VK_NULL_HANDLE; break;
	case UPSCALE_FSR1: ok = up->fsr1_easu_pipeline != VK_NULL_HANDLE &&
	                        up->fsr1_rcas_pipeline  != VK_NULL_HANDLE; break;
	default: break;
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "[upscale] failed to create pipeline for mode %s — upscaling disabled",
			upscale_mode_name(mode));
		up->mode = UPSCALE_NONE;
		return true;  /* non-fatal */
	}

	up->active = true;
	wlr_log(WLR_INFO, "[upscale] %s active (sharpness=%.2f)",
		upscale_mode_name(mode), sharpness);
	return true;
}

/* Custom wlr_buffer wrapping an exported DMA-BUF fd ──────────────────── */

struct vk_out_buffer {
	struct wlr_buffer           base;
	struct wlr_dmabuf_attributes dmabuf;
};

static void vk_out_buf_destroy(struct wlr_buffer *buf) {
	struct vk_out_buffer *vb = wl_container_of(buf, vb, base);
	/* fd is dup'd by caller — close our copy */
	for (int i = 0; i < vb->dmabuf.n_planes; i++)
		close(vb->dmabuf.fd[i]);
	free(vb);
}

static bool vk_out_buf_get_dmabuf(struct wlr_buffer *buf,
                                   struct wlr_dmabuf_attributes *out) {
	struct vk_out_buffer *vb = wl_container_of(buf, vb, base);
	*out = vb->dmabuf;
	return true;
}

static const struct wlr_buffer_impl vk_out_buf_impl = {
	.destroy    = vk_out_buf_destroy,
	.get_dmabuf = vk_out_buf_get_dmabuf,
};

static struct wlr_buffer *make_output_wlr_buffer(int fd, uint32_t w, uint32_t h,
                                                   uint32_t stride, uint32_t drm_fmt) {
	struct vk_out_buffer *vb = calloc(1, sizeof(*vb));
	if (!vb) return NULL;
	wlr_buffer_init(&vb->base, &vk_out_buf_impl, w, h);
	vb->dmabuf = (struct wlr_dmabuf_attributes){
		.width    = (int32_t)w,
		.height   = (int32_t)h,
		.format   = drm_fmt,
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.n_planes = 1,
		.fd       = { dup(fd), -1, -1, -1 },
		.stride   = { stride, 0, 0, 0 },
		.offset   = { 0, 0, 0, 0 },
	};
	if (vb->dmabuf.fd[0] < 0) { free(vb); return NULL; }
	return &vb->base;
}

/* Run a single compute pipeline and wait for completion. */
static void dispatch_compute(struct wlgame_upscale *up, VkPipeline pipeline,
                              float pc0, float pc1, float pc2, float pc3, float pc4,
                              uint32_t gx, uint32_t gy) {
	vkWaitForFences(up->device, 1, &up->fence, VK_TRUE, UINT64_MAX);
	vkResetFences(up->device, 1, &up->fence);
	vkResetCommandBuffer(up->cmd_buf, 0);

	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(up->cmd_buf, &begin);

	/* Transition input to SHADER_READ_ONLY */
	VkImageMemoryBarrier to_read = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask       = VK_ACCESS_NONE,
		.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.image               = up->input_image,
		.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	/* Transition output to GENERAL */
	VkImageMemoryBarrier to_general = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask       = VK_ACCESS_NONE,
		.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout           = VK_IMAGE_LAYOUT_GENERAL,
		.image               = up->output_image,
		.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageMemoryBarrier barriers_in[2] = { to_read, to_general };
	vkCmdPipelineBarrier(up->cmd_buf,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL, 2, barriers_in);

	vkCmdBindPipeline(up->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(up->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		up->pipeline_layout, 0, 1, &up->desc_set, 0, NULL);

	float pcs[5] = { pc0, pc1, pc2, pc3, pc4 };
	vkCmdPushConstants(up->cmd_buf, up->pipeline_layout,
		VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs), pcs);
	vkCmdDispatch(up->cmd_buf, (gx + 7) / 8, (gy + 7) / 8, 1);

	/* Barrier: compute write → present */
	VkImageMemoryBarrier out_barrier = {
		.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.image            = up->output_image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(up->cmd_buf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, NULL, 0, NULL, 1, &out_barrier);

	vkEndCommandBuffer(up->cmd_buf);

	VkSubmitInfo submit = {
		.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers    = &up->cmd_buf,
	};
	vkQueueSubmit(up->queue, 1, &submit, up->fence);
	vkWaitForFences(up->device, 1, &up->fence, VK_TRUE, UINT64_MAX);
}

void upscale_apply(struct wlgame_upscale *up, struct wlr_output_state *state,
                   uint32_t out_w, uint32_t out_h) {
	if (!up->active || up->mode == UPSCALE_NONE) return;
	if (!state->buffer) return;

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(state->buffer, &dmabuf)) return;

	VkFormat fmt = drm_to_vk(dmabuf.format);
	uint32_t in_w = (uint32_t)dmabuf.width, in_h = (uint32_t)dmabuf.height;

	/* Recreate images when size or format changes */
	bool need_rebuild = (in_w  != up->last_in_w  || in_h  != up->last_in_h  ||
	                     out_w != up->last_out_w  || out_h != up->last_out_h ||
	                     fmt   != up->last_vk_fmt);
	if (need_rebuild) {
		destroy_images(up);
		if (!import_dmabuf_image(up, &dmabuf, fmt)) {
			wlr_log(WLR_ERROR, "[upscale] failed to import input DMA-BUF");
			return;
		}
		if (!create_output_image(up, out_w, out_h, fmt)) {
			wlr_log(WLR_ERROR, "[upscale] failed to create output image");
			destroy_images(up);
			return;
		}
		up->last_in_w = in_w; up->last_in_h = in_h;
		up->last_out_w = out_w; up->last_out_h = out_h;
		up->last_vk_fmt = fmt;
		update_descriptors(up);
	} else {
		/* Re-import input (buffer changes each frame) */
		if (up->input_view)  vkDestroyImageView(up->device, up->input_view,  NULL);
		if (up->input_image) vkDestroyImage(up->device, up->input_image, NULL);
		if (up->input_mem)   vkFreeMemory(up->device, up->input_mem, NULL);
		up->input_view = VK_NULL_HANDLE; up->input_image = VK_NULL_HANDLE; up->input_mem = VK_NULL_HANDLE;
		if (!import_dmabuf_image(up, &dmabuf, fmt)) return;
		update_descriptors(up);
	}

	/* Run pipeline(s) */
	switch (up->mode) {
	case UPSCALE_CAS:
		/* pcs: sharpness, width, height */
		dispatch_compute(up, up->cas_pipeline,
			up->sharpness, (float)out_w, (float)out_h, 0, 0,
			out_w, out_h);
		break;
	case UPSCALE_NIS:
		/* pcs: sharpness, in_w, in_h, out_w, out_h */
		dispatch_compute(up, up->nis_pipeline,
			up->sharpness, (float)in_w, (float)in_h, (float)out_w, (float)out_h,
			out_w, out_h);
		break;
	case UPSCALE_FSR1: {
		/* EASU: in_w, in_h, out_w, out_h (no sharpness in easu) */
		dispatch_compute(up, up->fsr1_easu_pipeline,
			(float)in_w, (float)in_h, (float)out_w, (float)out_h, 0,
			out_w, out_h);
		/* RCAS: sharpness, width, height */
		dispatch_compute(up, up->fsr1_rcas_pipeline,
			up->sharpness, (float)out_w, (float)out_h, 0, 0,
			out_w, out_h);
		break;
	}
	default: return;
	}

	/* Get output row stride from VkSubresourceLayout */
	VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(up->device, up->output_image, &sub, &layout);

	struct wlr_buffer *new_buf = make_output_wlr_buffer(
		up->output_dma_fd, out_w, out_h,
		(uint32_t)layout.rowPitch, dmabuf.format);
	if (!new_buf) return;

	wlr_buffer_drop(state->buffer);
	state->buffer = new_buf;
}

void upscale_fini(struct wlgame_upscale *up) {
	if (!up->device) return;
	vkDeviceWaitIdle(up->device);
	destroy_images(up);
	if (up->desc_pool)          vkDestroyDescriptorPool(up->device,      up->desc_pool,        NULL);
	if (up->desc_layout)        vkDestroyDescriptorSetLayout(up->device, up->desc_layout,      NULL);
	if (up->pipeline_layout)    vkDestroyPipelineLayout(up->device,      up->pipeline_layout,  NULL);
	if (up->cas_pipeline)       vkDestroyPipeline(up->device,            up->cas_pipeline,     NULL);
	if (up->nis_pipeline)       vkDestroyPipeline(up->device,            up->nis_pipeline,     NULL);
	if (up->fsr1_easu_pipeline) vkDestroyPipeline(up->device,            up->fsr1_easu_pipeline, NULL);
	if (up->fsr1_rcas_pipeline) vkDestroyPipeline(up->device,            up->fsr1_rcas_pipeline, NULL);
	if (up->sampler)            vkDestroySampler(up->device,             up->sampler,          NULL);
	if (up->fence)              vkDestroyFence(up->device,               up->fence,            NULL);
	if (up->cmd_pool)           vkDestroyCommandPool(up->device,         up->cmd_pool,         NULL);
}

const char *upscale_mode_name(enum wlgame_upscale_mode mode) {
	switch (mode) {
	case UPSCALE_CAS:  return "CAS";
	case UPSCALE_NIS:  return "NIS";
	case UPSCALE_FSR1: return "FSR1";
	default:           return "none";
	}
}

enum wlgame_upscale_mode upscale_mode_from_str(const char *s) {
	if (!s) return UPSCALE_NONE;
	if (strcmp(s, "cas")  == 0) return UPSCALE_CAS;
	if (strcmp(s, "nis")  == 0) return UPSCALE_NIS;
	if (strcmp(s, "fsr1") == 0) return UPSCALE_FSR1;
	return UPSCALE_NONE;
}
