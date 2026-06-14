#pragma once

#include "upscale.h"

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>

/* ── Staging Wayland protocols ─────────────────────────────────────────── */
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>

/* ── Standard gaming-useful protocols ─────────────────────────────────── */
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_data_control_v1.h>

/* ── Screencopy / capture (Valve-adjacent: OBS, game overlays) ─────────── */
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>

/* ── Security / session ──────────────────────────────────────────────────── */
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>

/* ── Workspace management ─────────────────────────────────────────────── */
#include <wlr/types/wlr_ext_workspace_v1.h>

/* ── XWayland (legacy X11 game compat) ──────────────────────────────────── */
#include <wlr/xwayland/xwayland.h>

/* ── Layer shell (MangoHud, Steam overlay, game HUDs) ───────────────────── */
#include <wlr/types/wlr_layer_shell_v1.h>

struct wlgame_server {
	struct wl_display        *display;
	struct wlr_backend       *backend;
	struct wlr_renderer      *renderer;
	struct wlr_allocator     *allocator;

	struct wlr_scene             *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layer_background;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_normal;
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *layer_overlay;

	struct wlr_output_layout   *output_layout;
	struct wlr_compositor      *compositor;
	struct wlr_xdg_shell       *xdg_shell;
	struct wlr_seat            *seat;
	struct wlr_cursor          *cursor;
	struct wlr_xcursor_manager *cursor_mgr;

	/* ── Staging protocols ─────────────────────────────────────────────── */
	struct wlr_tearing_control_manager_v1    *tearing_manager;
	struct wlr_content_type_manager_v1       *content_type_manager;
	struct wlr_color_manager_v1             *color_manager;
	struct wlr_color_representation_manager_v1 *color_repr_manager;
	struct wlr_alpha_modifier_v1            *alpha_modifier;
	struct wlr_cursor_shape_manager_v1       *cursor_shape_manager;
	struct wlr_fractional_scale_manager_v1   *fractional_scale_manager;
	struct wlr_linux_drm_syncobj_manager_v1  *syncobj_manager;

	/* ── Standard protocols ─────────────────────────────────────────────── */
	struct wlr_linux_dmabuf_v1              *linux_dmabuf;
	struct wlr_viewporter                   *viewporter;
	struct wlr_presentation                 *presentation;
	struct wlr_output_manager_v1            *output_manager;
	struct wlr_xdg_decoration_manager_v1    *xdg_decoration_manager;
	struct wlr_idle_notifier_v1             *idle_notifier;
	struct wlr_gamma_control_manager_v1      *gamma_control_manager;
	struct wlr_single_pixel_buffer_manager_v1 *single_pixel_manager;
	struct wlr_ext_foreign_toplevel_list_v1  *foreign_toplevel_list;
	struct wlr_xdg_activation_v1             *xdg_activation;
	struct wlr_data_control_manager_v1       *data_control_manager;

	/* ── Screencopy / capture ──────────────────────────────────────────── */
	struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture_manager;
	struct wlr_ext_output_image_capture_source_manager_v1 *output_image_capture;

	/* ── Security / session ─────────────────────────────────────────────── */
	struct wlr_security_context_manager_v1 *security_context_manager;
	struct wlr_session_lock_manager_v1     *session_lock_manager;

	/* ── Workspace ──────────────────────────────────────────────────────── */
	struct wlr_ext_workspace_manager_v1    *workspace_manager;

	/* ── XWayland ───────────────────────────────────────────────────────── */
	struct wlr_xwayland                    *xwayland;

	/* ── Layer shell ────────────────────────────────────────────────────── */
	struct wlr_layer_shell_v1              *layer_shell;

	struct wl_list outputs;
	struct wl_list views;

	/* GPU info */
	bool nvidia;
	bool amd;

	/* Post-process upscaling (FSR1 / NIS / CAS) */
	struct wlgame_upscale upscale;

	bool allow_tearing;

	/* Listeners */
	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_xwayland_surface;
	struct wl_listener new_layer_surface;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener new_input;
	struct wl_listener seat_request_cursor;
	struct wl_listener seat_request_set_selection;
	struct wl_listener tearing_new_object;
};

void server_init(struct wlgame_server *server, bool allow_tearing,
                 enum wlgame_upscale_mode upscale_mode, float sharpness,
                 const char *shader_dir);
void server_run(struct wlgame_server *server);
void server_fini(struct wlgame_server *server);
