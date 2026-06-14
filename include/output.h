#pragma once


#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct wlgame_server;

struct wlgame_output {
	struct wl_list link;
	struct wlgame_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;

	bool has_game_surface;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

void output_new(struct wl_listener *listener, void *data);
