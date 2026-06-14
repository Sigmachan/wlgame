
#include "view.h"
#include "server.h"

#include <stdlib.h>
#include <wayland-protocols/content-type-v1-enum.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

void view_focus(struct wlgame_view *view, struct wlr_surface *surface) {
	if (!view) {
		return;
	}
	struct wlgame_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

struct wlgame_view *view_at(struct wlgame_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}
	*surface = scene_surface->surface;

	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static void view_check_game_mode(struct wlgame_view *view) {
	struct wlgame_server *server = view->server;
	struct wlr_surface *surface = view->xdg_toplevel->base->surface;

	enum wp_content_type_v1_type type =
		wlr_surface_get_content_type_v1(server->content_type_manager, surface);
	bool was_game = view->is_game;
	view->is_game = (type == WP_CONTENT_TYPE_V1_TYPE_GAME);

	if (view->is_game != was_game) {
		wlr_log(WLR_INFO, "[wlgame] view \"%s\" game_mode=%s",
			view->xdg_toplevel->title ? view->xdg_toplevel->title : "?",
			view->is_game ? "ON" : "OFF");
	}
}

static void view_map(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, map);
	view_check_game_mode(view);
	view_focus(view, view->xdg_toplevel->base->surface);
}

static void view_unmap(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, unmap);
	view->is_game = false;
}

static void view_commit(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, commit);
	view_check_game_mode(view);
}

static void view_destroy(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->link);
	free(view);
}

static void view_request_move(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_move);
	/* interactive move: TODO for stacking mode; not needed for game-only */
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_resize);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_maximize);
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel,
		view->xdg_toplevel->requested.maximized);
	wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_fullscreen);
	wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel,
		view->xdg_toplevel->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct wlgame_view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->xdg_toplevel = xdg_toplevel;
	view->scene_tree = wlr_scene_xdg_surface_create(server->layer_normal, xdg_toplevel->base);
	view->scene_tree->node.data = view;

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);

	wl_list_insert(&server->views, &view->link);
}

void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct wlr_xdg_surface *parent_xdg =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent_xdg) {
		return;
	}
	struct wlr_scene_tree *parent_tree = parent_xdg->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
}
