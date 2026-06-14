#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland/xwayland.h>

struct wlgame_server;

enum wlgame_view_type {
	WLGAME_VIEW_XDG,
	WLGAME_VIEW_XWAYLAND,
};

struct wlgame_view {
	struct wl_list link;
	struct wlgame_server *server;
	struct wlr_scene_tree *scene_tree;
	bool is_game;

	enum wlgame_view_type type;
	struct wlr_xdg_toplevel      *xdg_toplevel;   /* WLGAME_VIEW_XDG      */
	struct wlr_xwayland_surface  *xw_surface;      /* WLGAME_VIEW_XWAYLAND */

	/* Common listeners */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;

	/* XDG-only listeners */
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;

	/* XWayland-only listeners */
	struct wl_listener request_activate;
	struct wl_listener request_configure;
	struct wl_listener associate;
	struct wl_listener dissociate;
};

/* Returns the wlr_surface for either view type. */
struct wlr_surface *view_get_surface(struct wlgame_view *view);

void view_focus(struct wlgame_view *view, struct wlr_surface *surface);
struct wlgame_view *view_at(struct wlgame_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);

/* Called by server_new_xwayland_surface (xwayland.c) */
void server_new_xwayland_surface(struct wl_listener *listener, void *data);
