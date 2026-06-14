#include "view.h"
#include "server.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static void xw_focus(struct wlgame_view *view) {
	struct wlgame_server *server = view->server;
	wlr_xwayland_surface_activate(view->xw_surface, true);
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
}

static void xw_handle_map(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, map);
	view->is_game = true; /* X11 clients in wlgame are games by default */
	xw_focus(view);
	wlr_log(WLR_INFO, "[wlgame/xw] mapped: pid=%d title=%s",
		view->xw_surface->pid,
		view->xw_surface->title ? view->xw_surface->title : "?");
}

static void xw_handle_unmap(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, unmap);
	view->is_game = false;
}

static void xw_handle_destroy(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->link);
	free(view);
}

static void xw_handle_request_activate(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_activate);
	xw_focus(view);
}

static void xw_handle_request_configure(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_configure);
	const struct wlr_xwayland_surface_configure_event *ev = data;
	wlr_xwayland_surface_configure(view->xw_surface,
		ev->x, ev->y, ev->width, ev->height);
}

void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct wlgame_server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xw = data;

	struct wlgame_view *view = calloc(1, sizeof(*view));
	if (!view) return;

	view->server     = server;
	view->type       = WLGAME_VIEW_XWAYLAND;
	view->xw_surface = xw;
	xw->data         = view;

	/* Wrap in a scene tree so view_at() can find it via tree->node.data */
	struct wlr_scene_tree *tree =
		wlr_scene_tree_create(server->layer_normal);
	wlr_scene_surface_create(tree, xw->surface);
	view->scene_tree       = tree;
	tree->node.data        = view;

	view->map.notify = xw_handle_map;
	wl_signal_add(&xw->surface->events.map, &view->map);

	view->unmap.notify = xw_handle_unmap;
	wl_signal_add(&xw->surface->events.unmap, &view->unmap);

	view->destroy.notify = xw_handle_destroy;
	wl_signal_add(&xw->events.destroy, &view->destroy);

	view->request_activate.notify = xw_handle_request_activate;
	wl_signal_add(&xw->events.request_activate, &view->request_activate);

	view->request_configure.notify = xw_handle_request_configure;
	wl_signal_add(&xw->events.request_configure, &view->request_configure);

	wl_list_insert(&server->views, &view->link);
	wlr_log(WLR_DEBUG, "[wlgame/xw] new surface pid=%d", xw->pid);
}
