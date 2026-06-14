#pragma once


#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

struct wlgame_server;

struct wlgame_view {
	struct wl_list link;
	struct wlgame_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;

	bool is_game;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

void view_focus(struct wlgame_view *view, struct wlr_surface *surface);
struct wlgame_view *view_at(struct wlgame_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
