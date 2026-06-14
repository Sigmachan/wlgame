
#include "server.h"
#include "output.h"
#include "nvidia.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-protocols/color-management-v1-enum.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/backend.h>
#include <wlr/util/log.h>

/* Forward declarations for handlers defined in other files */
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void server_new_xdg_popup(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);
void server_seat_request_cursor(struct wl_listener *listener, void *data);
void server_seat_request_set_selection(struct wl_listener *listener, void *data);
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_cursor_frame(struct wl_listener *listener, void *data);

static void setup_color_manager(struct wlgame_server *server) {
	size_t tf_len, primaries_len;
	enum wp_color_manager_v1_transfer_function *tfs =
		wlr_color_manager_v1_transfer_function_list_from_renderer(server->renderer, &tf_len);
	enum wp_color_manager_v1_primaries *primaries =
		wlr_color_manager_v1_primaries_list_from_renderer(server->renderer, &primaries_len);

	static const enum wp_color_manager_v1_render_intent intents[] = {
		WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
		WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE,
		WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE,
	};

	struct wlr_color_manager_v1_options opts = {
		.features = {
			.parametric = true,
			.set_primaries = true,
			.set_luminances = true,
			.set_mastering_display_primaries = true,
			.extended_target_volume = true,
		},
		.render_intents = intents,
		.render_intents_len = 3,
		.transfer_functions = tfs,
		.transfer_functions_len = tf_len,
		.primaries = primaries,
		.primaries_len = primaries_len,
	};

	server->color_manager = wlr_color_manager_v1_create(server->display, 1, &opts);
	free(tfs);
	free(primaries);

	wlr_log(WLR_INFO, "[wlgame] color-management-v1: %zu TFs, %zu primaries sets advertised",
		tf_len, primaries_len);
}

static void setup_color_representation(struct wlgame_server *server) {
	server->color_repr_manager =
		wlr_color_representation_manager_v1_create_with_renderer(server->display, 1, server->renderer);
}

void server_init(struct wlgame_server *server, bool allow_tearing) {
	server->allow_tearing = allow_tearing;

	nvidia_apply_env();
	server->nvidia = nvidia_detect();
	nvidia_print_info();

	server->display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);

	struct wlr_session *session = NULL;
	server->backend = wlr_backend_autocreate(loop, &session);
	assert(server->backend);

	server->renderer = wlr_renderer_autocreate(server->backend);
	assert(server->renderer);
	wlr_renderer_init_wl_display(server->renderer, server->display);

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	assert(server->allocator);

	/* Scene graph — layered for proper z-ordering */
	server->scene = wlr_scene_create();
	server->layer_background = wlr_scene_tree_create(&server->scene->tree);
	server->layer_bottom     = wlr_scene_tree_create(&server->scene->tree);
	server->layer_normal     = wlr_scene_tree_create(&server->scene->tree);
	server->layer_top        = wlr_scene_tree_create(&server->scene->tree);
	server->layer_overlay    = wlr_scene_tree_create(&server->scene->tree);

	server->output_layout = wlr_output_layout_create(server->display);
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

	/* Core protocols */
	server->compositor = wlr_compositor_create(server->display, 6, server->renderer);
	wlr_subcompositor_create(server->display);
	wlr_data_device_manager_create(server->display);
	wlr_shm_create_with_renderer(server->display, 2, server->renderer);

	/* XDG shell */
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	/* --- Staging protocols --- */

	/* Tearing control: games opt-in to async flips */
	server->tearing_manager = wlr_tearing_control_manager_v1_create(server->display, 1);

	/* Content type: clients self-identify as game/video/photo */
	server->content_type_manager = wlr_content_type_manager_v1_create(server->display, 1);

	/* Color management: full HDR/wide-gamut pipeline */
	setup_color_manager(server);
	setup_color_representation(server);

	/* Fractional scaling */
	server->fractional_scale_manager =
		wlr_fractional_scale_manager_v1_create(server->display, 1);

	/* Alpha modifier */
	server->alpha_modifier = wlr_alpha_modifier_v1_create(server->display);

	/* Cursor shape (no cursor surface required from clients) */
	server->cursor_shape_manager =
		wlr_cursor_shape_manager_v1_create(server->display, 1);

	/* Explicit GPU sync — critical on NVIDIA (fixes corruption with async drivers) */
	int drm_fd = wlr_backend_get_drm_fd(server->backend);
	if (drm_fd >= 0) {
		server->syncobj_manager =
			wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd);
		wlr_log(WLR_INFO, "[wlgame] linux-drm-syncobj-v1: active (drm_fd=%d)", drm_fd);
	}

	/* --- Standard gaming-useful protocols --- */

	server->linux_dmabuf =
		wlr_linux_dmabuf_v1_create_with_renderer(server->display, 5, server->renderer);
	server->viewporter = wlr_viewporter_create(server->display);
	server->presentation = wlr_presentation_create(server->display, server->backend, 2);
	server->output_manager = wlr_output_manager_v1_create(server->display);
	server->xdg_decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->display, 1);
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	server->gamma_control_manager = wlr_gamma_control_manager_v1_create(server->display);
	server->screencopy_manager = wlr_screencopy_manager_v1_create(server->display);
	server->export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server->display);
	server->single_pixel_manager = wlr_single_pixel_buffer_manager_v1_create(server->display);
	server->foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
	server->xdg_activation = wlr_xdg_activation_v1_create(server->display);

	/* Output */
	wl_list_init(&server->outputs);
	server->new_output.notify = output_new;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	/* Views */
	wl_list_init(&server->views);

	/* Seat + cursor */
	server->seat = wlr_seat_create(server->display, "seat0");
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->seat_request_cursor.notify = server_seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->seat_request_cursor);
	server->seat_request_set_selection.notify = server_seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->seat_request_set_selection);
}

void server_run(struct wlgame_server *server) {
	const char *socket = wl_display_add_socket_auto(server->display);
	assert(socket);
	setenv("WAYLAND_DISPLAY", socket, true);

	wlr_log(WLR_INFO, "[wlgame] Wayland socket: %s", socket);
	wlr_log(WLR_INFO, "[wlgame] NVIDIA: %s", server->nvidia ? "yes" : "no");
	wlr_log(WLR_INFO, "[wlgame] Tearing: %s", server->allow_tearing ? "allowed" : "vsync-only");

	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "[wlgame] failed to start backend");
		exit(1);
	}

	fprintf(stderr, "[wlgame] running on %s\n", socket);
	wl_display_run(server->display);
}

void server_fini(struct wlgame_server *server) {
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);
	wlr_backend_destroy(server->backend);
	wl_display_destroy(server->display);
}
