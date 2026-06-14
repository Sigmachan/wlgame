
#include "output.h"
#include "server.h"
#include "upscale.h"
#include "view.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-protocols/content-type-v1-enum.h>
#include <wayland-protocols/tearing-control-v1-enum.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static int64_t now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static bool output_has_game_surface(struct wlgame_output *output) {
	struct wlgame_server *server = output->server;
	struct wlgame_view *view;
	wl_list_for_each(view, &server->views, link) {
		struct wlr_surface *surface = view_get_surface(view);
		if (!surface || !surface->mapped) {
			continue;
		}

		enum wp_tearing_control_v1_presentation_hint hint =
			wlr_tearing_control_manager_v1_surface_hint_from_surface(
				server->tearing_manager, surface);
		if (hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC) {
			return true;
		}

		enum wp_content_type_v1_type type =
			wlr_surface_get_content_type_v1(server->content_type_manager, surface);
		if (type == WP_CONTENT_TYPE_V1_TYPE_GAME || view->is_game) {
			return true;
		}
	}
	return false;
}

/* Scaled mode: render the scene at the internal resolution into the headless
 * game output, FSR/NIS-upscale that buffer to this real output's size, and
 * present it. Driven by the real output's frame (vblank) clock. */
static void present_scaled(struct wlgame_output *output) {
	struct wlgame_server *server = output->server;
	struct wlr_output *real = output->wlr_output;
	struct wlr_scene_output *game_so = server->game_scene_output;
	if (!game_so) {
		wlr_output_schedule_frame(real);
		return;
	}

	/* fps cap: throttle the game by skipping presents + frame callbacks. */
	if (server->fps_limit > 0) {
		int64_t now = now_ns();
		int64_t interval = 1000000000LL / server->fps_limit;
		if (output->last_present_ns != 0 &&
		    now - output->last_present_ns < interval - interval / 16) {
			wlr_output_schedule_frame(real);
			return;
		}
		output->last_present_ns = now;
	}

	/* Render the scene at internal resolution into the game output's buffer. */
	struct wlr_output_state gs;
	wlr_output_state_init(&gs);
	if (!wlr_scene_output_build_state(game_so, &gs, NULL) || !gs.buffer) {
		/* Nothing damaged this frame — keep the clock alive. */
		wlr_output_state_finish(&gs);
		wlr_output_schedule_frame(real);
		return;
	}

	/* Upscale the internal-res frame to the real output size. */
	struct wlr_buffer *up_buf = upscale_run(&server->upscale, gs.buffer,
		(uint32_t)real->width, (uint32_t)real->height);
	wlr_output_state_finish(&gs);   /* release the build buffer */

	if (!up_buf) {
		wlr_output_schedule_frame(real);
		return;
	}

	/* Present the upscaled buffer on the real output. */
	struct wlr_output_state rs;
	wlr_output_state_init(&rs);
	wlr_output_state_set_enabled(&rs, true);
	wlr_output_state_set_buffer(&rs, up_buf);
	if (!wlr_output_commit_state(real, &rs)) {
		wlr_log(WLR_ERROR, "[wlgame] scaled present failed on %s", real->name);
	}
	wlr_output_state_finish(&rs);
	wlr_buffer_drop(up_buf);

	if (!output->scanout_logged) {
		output->scanout_logged = true;
		wlr_log(WLR_INFO, "[wlgame] scaled present active: %dx%d → %dx%d via %s",
			server->render_width, server->render_height,
			real->width, real->height, upscale_mode_name(server->upscale.mode));
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(game_so, &now);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct wlgame_output *output = wl_container_of(listener, output, frame);
	struct wlgame_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	if (server->scaled) {
		present_scaled(output);
		return;
	}

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		server->scene, wlr_output);
	if (!scene_output) {
		return;
	}

	/* fps cap: throttle to the target by skipping vblanks. Returning without
	 * sending frame-done also paces the client (the game) down to the cap. */
	if (server->fps_limit > 0) {
		int64_t now = now_ns();
		int64_t interval = 1000000000LL / server->fps_limit;
		if (output->last_present_ns != 0 &&
		    now - output->last_present_ns < interval - interval / 16) {
			wlr_output_schedule_frame(wlr_output);
			return;
		}
		output->last_present_ns = now;
	}

	bool want_game = output_has_game_surface(output);

	/* When a fullscreen game owns the screen and no post-process upscale is
	 * active, wlr_scene can scan its buffer out directly (zero-copy). */
	if (want_game && server->upscale.mode == UPSCALE_NONE && !output->scanout_logged) {
		wlr_log(WLR_INFO, "[wlgame] direct scanout eligible on %s", wlr_output->name);
		output->scanout_logged = true;
	}
	bool want_tearing = server->allow_tearing && want_game;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	if (want_tearing) {
		state.tearing_page_flip = true;
	}

	/* VRR: auto-enable adaptive sync when a game surface is active */
	if (wlr_output->adaptive_sync_supported) {
		wlr_output_state_set_adaptive_sync_enabled(&state, want_game);
	}

	wlr_scene_output_build_state(scene_output, &state, NULL);

	/* Post-process upscaling (FSR1 / NIS / CAS). At native res this is a
	 * sharpening pass; it returns a new owned buffer or NULL when inactive. */
	struct wlr_buffer *up_buf = upscale_run(&server->upscale, state.buffer,
		(uint32_t)wlr_output->width, (uint32_t)wlr_output->height);

	if (up_buf) {
		wlr_output_state_finish(&state);   /* release the scene buffer */
		struct wlr_output_state us;
		wlr_output_state_init(&us);
		wlr_output_state_set_enabled(&us, true);
		if (want_tearing) {
			us.tearing_page_flip = true;
		}
		if (wlr_output->adaptive_sync_supported) {
			wlr_output_state_set_adaptive_sync_enabled(&us, want_game);
		}
		wlr_output_state_set_buffer(&us, up_buf);
		wlr_output_commit_state(wlr_output, &us);
		wlr_output_state_finish(&us);
		wlr_buffer_drop(up_buf);
	} else {
		wlr_output_commit_state(wlr_output, &state);
		wlr_output_state_finish(&state);
	}

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

	int W = server->output_width, H = server->output_height, R = server->output_rate;
	if (W > 0 && H > 0 && server->nested) {
		/* Nested window: any size is fine, set it directly. Rate in mHz. */
		wlr_output_state_set_custom_mode(&state, W, H, R > 0 ? R * 1000 : 0);
	} else if (W > 0 && H > 0) {
		/* DRM: pick the listed mode matching WxH (closest refresh to R). */
		struct wlr_output_mode *best = NULL, *m;
		wl_list_for_each(m, &wlr_output->modes, link) {
			if (m->width != W || m->height != H) continue;
			if (!best || (R > 0 &&
			    abs(m->refresh - R * 1000) < abs(best->refresh - R * 1000))) {
				best = m;
			}
		}
		if (best) {
			wlr_output_state_set_mode(&state, best);
		} else {
			wlr_log(WLR_ERROR, "[wlgame] no %dx%d mode on %s — using preferred",
				W, H, wlr_output->name);
			struct wlr_output_mode *pref = wlr_output_preferred_mode(wlr_output);
			if (pref) wlr_output_state_set_mode(&state, pref);
		}
	} else {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		if (mode) wlr_output_state_set_mode(&state, mode);
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

	/* In scaled mode the real output is NOT part of the scene/layout — it's a
	 * dumb presenter for the upscaled game-output buffer, and must not be
	 * exposed as a wl_output (clients render into the internal output instead). */
	if (!server->scaled) {
		struct wlr_output_layout_output *layout_output =
			wlr_output_layout_add_auto(server->output_layout, wlr_output);
		output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
		wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
			output->scene_output);
	}

	setup_output_color(output);

	wlr_log(WLR_INFO, "[wlgame] output: %s %dx%d tearing=%s",
		wlr_output->name,
		wlr_output->width, wlr_output->height,
		(wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) ? "vrr" : "off");
}
