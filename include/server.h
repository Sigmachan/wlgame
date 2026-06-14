#pragma once

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
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

struct wlgame_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layer_background;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_normal;
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *layer_overlay;

	struct wlr_output_layout *output_layout;
	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;

	/* staging protocols */
	struct wlr_tearing_control_manager_v1 *tearing_manager;
	struct wlr_content_type_manager_v1 *content_type_manager;
	struct wlr_color_manager_v1 *color_manager;
	struct wlr_color_representation_manager_v1 *color_repr_manager;
	struct wlr_alpha_modifier_v1 *alpha_modifier;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;
	struct wlr_linux_drm_syncobj_manager_v1 *syncobj_manager;

	/* standard protocols */
	struct wlr_linux_dmabuf_v1 *linux_dmabuf;
	struct wlr_viewporter *viewporter;
	struct wlr_presentation *presentation;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager;
	struct wlr_screencopy_manager_v1 *screencopy_manager;
	struct wlr_export_dmabuf_manager_v1 *export_dmabuf_manager;
	struct wlr_single_pixel_buffer_manager_v1 *single_pixel_manager;
	struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
	struct wlr_xdg_activation_v1 *xdg_activation;

	struct wl_list outputs;
	struct wl_list views;

	bool nvidia;
	bool allow_tearing;

	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
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

void server_init(struct wlgame_server *server, bool allow_tearing);
void server_run(struct wlgame_server *server);
void server_fini(struct wlgame_server *server);
