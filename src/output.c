
#include "output.h"
#include "server.h"
#include "view.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-protocols/content-type-v1-enum.h>
#include <wayland-protocols/tearing-control-v1-enum.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static bool output_has_game_surface(struct wlgame_output *output) {
	struct wlgame_server *server = output->server;
	struct wlgame_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		struct wlr_surface *surface = view->xdg_toplevel->base->surface;

		enum wp_tearing_control_v1_presentation_hint hint =
			wlr_tearing_control_manager_v1_surface_hint_from_surface(
				server->tearing_manager, surface);
		if (hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC) {
			return true;
		}

		enum wp_content_type_v1_type type =
			wlr_surface_get_content_type_v1(server->content_type_manager, surface);
		if (type == WP_CONTENT_TYPE_V1_TYPE_GAME) {
			return true;
		}
	}
	return false;
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct wlgame_output *output = wl_container_of(listener, output, frame);
	struct wlgame_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		server->scene, wlr_output);
	if (!scene_output) {
		return;
	}

	bool want_tearing = server->allow_tearing && output_has_game_surface(output);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	if (want_tearing) {
		state.tearing_page_flip = true;
	}

	wlr_scene_output_build_state(scene_output, &state, NULL);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct wlgame_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct wlgame_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void setup_output_color(struct wlgame_output *output) {
	struct wlgame_server *server = output->server;
	if (!server->color_manager) {
		return;
	}

	/* Advertise sRGB as preferred — games will set HDR via color-management-v1 */
	struct wlr_image_description_v1_data pref = {
		.tf_named = 0, /* let wlroots pick default */
		.primaries_named = 0,
	};
	wlr_color_manager_v1_set_surface_preferred_image_description(
		server->color_manager, NULL, &pref);
}

void output_new(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct wlgame_output *output = calloc(1, sizeof(*output));
	output->server = server;
	output->wlr_output = wlr_output;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, output->scene_output);

	setup_output_color(output);

	wlr_log(WLR_INFO, "[wlgame] output: %s %dx%d tearing=%s",
		wlr_output->name,
		wlr_output->width, wlr_output->height,
		(wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) ? "vrr" : "off");
}
