
#include "server.h"
#include "output.h"
#include "gpu.h"
#include "upscale.h"
#include "launch.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-protocols/color-management-v1-enum.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/backend/headless.h>
#include <wlr/util/log.h>

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
void server_new_xwayland_surface(struct wl_listener *listener, void *data);
void server_new_layer_surface(struct wl_listener *listener, void *data);

/* Detect a nested (windowed) backend, and capture the concrete sub-backend so
 * we can create its output. wlr_backend_autocreate returns a multi-backend, so
 * the is_wl/is_x11/is_headless checks must run over its children. */
static void find_nested_iter(struct wlr_backend *backend, void *data) {
	struct wlgame_server *s = data;
	if (wlr_backend_is_wl(backend) || wlr_backend_is_x11(backend) ||
	    wlr_backend_is_headless(backend)) {
		s->nested = true;
		if (!s->nested_backend) s->nested_backend = backend;
	}
}

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

	/* wlroots 0.21 only implements the `parametric` feature so far and asserts
	 * the rest (set_primaries / set_luminances / set_mastering /
	 * extended_target_volume) must be false. Parametric image descriptions
	 * still expose PQ / BT.2020 / DCI-P3 via the advertised TF + primaries enum
	 * lists — which is exactly what HDR games signal. */
	struct wlr_color_manager_v1_options opts = {
		.features = {
			.parametric = true,
		},
		.render_intents      = intents,
		.render_intents_len  = 3,
		.transfer_functions  = tfs,
		.transfer_functions_len = tf_len,
		.primaries           = primaries,
		.primaries_len       = primaries_len,
	};

	server->color_manager = wlr_color_manager_v1_create(server->display, 1, &opts);
	free(tfs);
	free(primaries);
	wlr_log(WLR_INFO, "[wlgame] color-management-v1: %zu TFs, %zu primaries",
		tf_len, primaries_len);
}

void server_init(struct wlgame_server *server, const struct wlgame_config *cfg) {
	enum wlgame_upscale_mode upscale_mode = cfg->upscale_mode;
	float sharpness        = cfg->sharpness;
	const char *shader_dir = cfg->shader_dir;

	server->allow_tearing  = cfg->tearing;
	server->output_width   = cfg->output_width;
	server->output_height  = cfg->output_height;
	server->output_rate    = cfg->output_rate;
	server->render_width   = cfg->render_width;
	server->render_height  = cfg->render_height;
	server->fps_limit      = cfg->fps_limit;
	server->fullscreen     = cfg->fullscreen;
	server->mangoapp       = cfg->mangoapp;
	server->prefer_wayland = cfg->prefer_wayland;
	server->child_argv     = cfg->child_argv;

	/* GPU detection sets WLR_RENDERER=vulkan etc. before backend creation */
	struct wlgame_gpu_info gpu = gpu_detect_and_apply();
	server->nvidia = gpu.nvidia;
	server->amd    = gpu.amd;
	gpu_print_info(&gpu);

	server->display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);

	struct wlr_session *session = NULL;
	server->backend = wlr_backend_autocreate(loop, &session);
	assert(server->backend);

	/* A nested backend (running inside another Wayland/X11 session) renders to
	 * a window; a DRM backend owns the display. This changes how outputs are
	 * sized and created. */
	server->nested = false;
	server->nested_backend = NULL;
	if (wlr_backend_is_multi(server->backend)) {
		wlr_multi_for_each_backend(server->backend, find_nested_iter, server);
	} else {
		find_nested_iter(server->backend, server);
	}
	wlr_log(WLR_INFO, "[wlgame] backend: %s", server->nested ? "nested" : "DRM");

	/* Internal-res scaling: a headless "game" output at render_w×render_h that
	 * clients render into, upscaled to the real output each frame. */
	server->scaled = (server->render_width > 0 && server->render_height > 0);
	if (server->scaled) {
		server->headless = wlr_headless_backend_create(loop);
		if (!server->headless) {
			wlr_log(WLR_ERROR, "[wlgame] headless backend failed — disabling scaling");
			server->scaled = false;
		}
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	assert(server->renderer);
	wlr_renderer_init_wl_display(server->renderer, server->display);

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	assert(server->allocator);

	/* Scene graph — 5 layers, Z-ordered */
	server->scene          = wlr_scene_create();
	server->layer_background = wlr_scene_tree_create(&server->scene->tree);
	server->layer_bottom     = wlr_scene_tree_create(&server->scene->tree);
	server->layer_normal     = wlr_scene_tree_create(&server->scene->tree);
	server->layer_top        = wlr_scene_tree_create(&server->scene->tree);
	server->layer_overlay    = wlr_scene_tree_create(&server->scene->tree);

	server->output_layout = wlr_output_layout_create(server->display);
	server->scene_layout  = wlr_scene_attach_output_layout(server->scene, server->output_layout);

	/* Core */
	server->compositor = wlr_compositor_create(server->display, 6, server->renderer);
	wlr_subcompositor_create(server->display);
	wlr_data_device_manager_create(server->display);
	wlr_shm_create_with_renderer(server->display, 2, server->renderer);

	/* XDG shell v6 */
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	/* ── Staging protocols ─────────────────────────────────────────────── */

	/* Tearing: game surfaces opt in to async page flips */
	server->tearing_manager = wlr_tearing_control_manager_v1_create(server->display, 1);

	/* Content type: games self-identify for tearing + upscale decisions */
	server->content_type_manager = wlr_content_type_manager_v1_create(server->display, 1);

	/* HDR / wide-gamut (PQ, BT.2020, DCI-P3, mastering metadata) */
	setup_color_manager(server);
	server->color_repr_manager =
		wlr_color_representation_manager_v1_create_with_renderer(server->display, 1, server->renderer);

	/* Scaling */
	server->fractional_scale_manager =
		wlr_fractional_scale_manager_v1_create(server->display, 1);
	server->alpha_modifier = wlr_alpha_modifier_v1_create(server->display);

	/* Cursor */
	server->cursor_shape_manager =
		wlr_cursor_shape_manager_v1_create(server->display, 1);

	/* Explicit GPU sync — critical on NVIDIA (fixes async Vulkan driver corruption) */
	int drm_fd = wlr_backend_get_drm_fd(server->backend);
	if (drm_fd >= 0) {
		server->syncobj_manager =
			wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd);
		wlr_log(WLR_INFO, "[wlgame] linux-drm-syncobj-v1: active (drm_fd=%d)", drm_fd);
	}

	/* ── Standard protocols ─────────────────────────────────────────────── */
	server->linux_dmabuf =
		wlr_linux_dmabuf_v1_create_with_renderer(server->display, 5, server->renderer);
	server->viewporter       = wlr_viewporter_create(server->display);
	server->presentation     = wlr_presentation_create(server->display, server->backend, 2);
	server->output_manager   = wlr_output_manager_v1_create(server->display);
	server->xdg_decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->display, 1);
	server->idle_notifier    = wlr_idle_notifier_v1_create(server->display);
	server->gamma_control_manager = wlr_gamma_control_manager_v1_create(server->display);
	server->single_pixel_manager  = wlr_single_pixel_buffer_manager_v1_create(server->display);
	server->foreign_toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
	server->xdg_activation = wlr_xdg_activation_v1_create(server->display);
	server->data_control_manager =
		wlr_data_control_manager_v1_create(server->display);

	/* ── Screencopy / capture (for OBS, game overlays, screenshots) ────── */
	server->image_copy_capture_manager =
		wlr_ext_image_copy_capture_manager_v1_create(server->display, 1);
	server->output_image_capture =
		wlr_ext_output_image_capture_source_manager_v1_create(server->display, 1);

	/* ── Security / session ─────────────────────────────────────────────── */
	server->security_context_manager =
		wlr_security_context_manager_v1_create(server->display);
	server->session_lock_manager = wlr_session_lock_manager_v1_create(server->display);

	/* ── Workspace management ───────────────────────────────────────────── */
	server->workspace_manager = wlr_ext_workspace_manager_v1_create(server->display, 1);

	/* ── Layer shell (MangoHud, Steam overlay, game HUDs) ───────────────── */
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
	wlr_log(WLR_INFO, "[wlgame] layer-shell-v1: active");

	/* ── Outputs ─────────────────────────────────────────────────────────── */
	wl_list_init(&server->outputs);
	server->new_output.notify = output_new;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	wl_list_init(&server->views);

	/* ── Seat + cursor ──────────────────────────────────────────────────── */
	server->seat    = wlr_seat_create(server->display, "seat0");
	server->cursor  = wlr_cursor_create();
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
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->seat_request_set_selection);

	/* ── XWayland (X11 legacy game compat) ─────────────────────────────── */
#ifdef HAVE_XWAYLAND
	server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
	if (server->xwayland) {
		server->new_xwayland_surface.notify = server_new_xwayland_surface;
		wl_signal_add(&server->xwayland->events.new_surface,
			&server->new_xwayland_surface);
		wlr_xwayland_set_seat(server->xwayland, server->seat);
		setenv("DISPLAY", server->xwayland->display_name, true);
		wlr_log(WLR_INFO, "[wlgame] XWayland: active on %s",
			server->xwayland->display_name);
	} else {
		wlr_log(WLR_ERROR, "[wlgame] XWayland: failed to start");
	}
#else
	wlr_log(WLR_INFO, "[wlgame] XWayland: disabled at build time");
#endif

	/* ── Upscaling pipeline ─────────────────────────────────────────────── */
	/* Auto-select mode if user didn't specify */
	if (upscale_mode == UPSCALE_NONE && (gpu.nvidia || gpu.amd)) {
		upscale_mode = gpu.nvidia ? UPSCALE_NIS : UPSCALE_CAS;
		wlr_log(WLR_INFO, "[wlgame] upscale: auto-selected %s for %s GPU",
			upscale_mode_name(upscale_mode), gpu.nvidia ? "NVIDIA" : "AMD");
	}
	/* Scaling needs a *resizing* filter; CAS and none only sharpen at 1:1. */
	if (server->scaled && (upscale_mode == UPSCALE_NONE || upscale_mode == UPSCALE_CAS)) {
		upscale_mode = gpu.nvidia ? UPSCALE_NIS : UPSCALE_FSR1;
		wlr_log(WLR_INFO, "[wlgame] scaled mode needs a resizing filter — using %s",
			upscale_mode_name(upscale_mode));
	}
	upscale_init(&server->upscale, server->renderer, upscale_mode, sharpness, shader_dir);
}

void server_run(struct wlgame_server *server) {
	const char *socket = wl_display_add_socket_auto(server->display);
	assert(socket);
	setenv("WAYLAND_DISPLAY", socket, true);

	wlr_log(WLR_INFO, "[wlgame] socket: %s | NVIDIA=%s AMD=%s | tearing=%s | upscale=%s | fps=%s",
		socket,
		server->nvidia ? "yes" : "no",
		server->amd    ? "yes" : "no",
		server->allow_tearing ? "on" : "off",
		upscale_mode_name(server->upscale.mode),
		server->fps_limit ? "capped" : "vsync");

	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "[wlgame] failed to start backend");
		exit(1);
	}

	/* Nested backends don't auto-create an output — make our window now so
	 * the game has a surface to render into. output_new() sizes it. */
	if (server->nested && server->nested_backend) {
		int W = server->output_width  > 0 ? server->output_width  : 1920;
		int H = server->output_height > 0 ? server->output_height : 1080;
		if (wlr_backend_is_wl(server->nested_backend)) {
			wlr_wl_output_create(server->nested_backend);
		} else if (wlr_backend_is_x11(server->nested_backend)) {
			wlr_x11_output_create(server->nested_backend);
		} else if (wlr_backend_is_headless(server->nested_backend)) {
			if (wl_list_empty(&server->outputs)) {
				wlr_headless_add_output(server->nested_backend, W, H);
			}
		}
		if (wl_list_empty(&server->outputs)) {
			wlr_log(WLR_ERROR, "[wlgame] nested backend produced no output");
		}
	}

	/* Internal-res virtual output: clients see and render at render_w×render_h;
	 * the real output presents an upscaled copy (see present_scaled in output.c).
	 * The game output goes in the layout (→ exposed as the only wl_output); the
	 * real output stays out of the layout so clients can't pick it. */
	if (server->scaled && server->headless) {
		if (!wlr_backend_start(server->headless)) {
			wlr_log(WLR_ERROR, "[wlgame] failed to start headless backend");
		} else {
			server->game_output = wlr_headless_add_output(server->headless,
				server->render_width, server->render_height);
			wlr_output_init_render(server->game_output, server->allocator, server->renderer);

			struct wlr_output_state gst;
			wlr_output_state_init(&gst);
			wlr_output_state_set_enabled(&gst, true);
			wlr_output_state_set_custom_mode(&gst,
				server->render_width, server->render_height, 0);
			wlr_output_commit_state(server->game_output, &gst);
			wlr_output_state_finish(&gst);

			struct wlr_output_layout_output *glo =
				wlr_output_layout_add_auto(server->output_layout, server->game_output);
			server->game_scene_output =
				wlr_scene_output_create(server->scene, server->game_output);
			wlr_scene_output_layout_add_output(server->scene_layout, glo,
				server->game_scene_output);

			wlr_log(WLR_INFO, "[wlgame] internal render %dx%d → upscale to output (%s)",
				server->render_width, server->render_height,
				upscale_mode_name(server->upscale.mode));
		}
	}

	/* Spawn the wrapped game (if any) now that the socket is live. */
	launch_init(server);

	fprintf(stderr, "[wlgame] running on %s\n", socket);
	wl_display_run(server->display);
}

void server_fini(struct wlgame_server *server) {
	/* Stop the game before tearing down the graphics stack. */
	launch_finish(server);

	/* Detach every global listener before destroying the objects they hang
	 * off — wlr_cursor_destroy (and friends) assert their signals are empty. */
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->new_xdg_toplevel.link);
	wl_list_remove(&server->new_xdg_popup.link);
	wl_list_remove(&server->new_layer_surface.link);
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->seat_request_cursor.link);
	wl_list_remove(&server->seat_request_set_selection.link);

#ifdef HAVE_XWAYLAND
	if (server->xwayland) {
		/* Detach our new_surface listener — wlr_xwayland_destroy asserts the
		 * signal has no remaining listeners. */
		wl_list_remove(&server->new_xwayland_surface.link);
		wlr_xwayland_destroy(server->xwayland);
		server->xwayland = NULL;
	}
#endif

	upscale_fini(&server->upscale);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);
	if (server->headless) {
		wlr_backend_destroy(server->headless);
		server->headless = NULL;
	}
	wlr_backend_destroy(server->backend);
	wl_display_destroy(server->display);
}
