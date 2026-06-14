#include "view.h"
#include "server.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

static void xw_focus(struct wlgame_view *view) {
	struct wlgame_server *server = view->server;
	struct wlr_xwayland_surface *xw = view->xw_surface;

	/* Override-redirect windows (menus, tooltips, splashes, some launchers)
	 * only take keyboard focus when they explicitly ask for it — otherwise
	 * they would steal input from the game behind them. */
	if (xw->override_redirect &&
	    !wlr_xwayland_surface_override_redirect_wants_focus(xw)) {
		if (view->scene_tree)
			wlr_scene_node_raise_to_top(&view->scene_tree->node);
		return;
	}

	wlr_xwayland_surface_activate(xw, true);
	if (view->scene_tree)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);

	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);

	struct wlr_surface *surface = xw->surface;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (surface && kb) {
		wlr_seat_keyboard_notify_enter(server->seat, surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
}

static void xw_handle_map(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, map);
	struct wlgame_server *server = view->server;
	struct wlr_xwayland_surface *xw = view->xw_surface;

	/* Scene node is created lazily on map and destroyed on unmap, so the
	 * scene graph only ever references a live wlr_surface. */
	struct wlr_scene_tree *tree = wlr_scene_tree_create(server->layer_normal);
	wlr_scene_surface_create(tree, xw->surface);
	tree->node.data  = view;
	view->scene_tree = tree;
	wlr_scene_node_set_position(&tree->node, xw->x, xw->y);

	/* X11 clients in wlgame are games by default; override-redirect popups
	 * (menus/tooltips) are not. */
	view->is_game = !xw->override_redirect;

	xw_focus(view);
	wlr_log(WLR_INFO, "[wlgame/xw] mapped: pid=%d or=%d %dx%d title=%s",
		xw->pid, xw->override_redirect, xw->width, xw->height,
		xw->title ? xw->title : "?");
}

static void xw_handle_unmap(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, unmap);
	view->is_game = false;
	if (view->scene_tree) {
		wlr_scene_node_destroy(&view->scene_tree->node);
		view->scene_tree = NULL;
	}
}

/* `associate` fires once the inner wlr_surface becomes valid (wlroots ≥0.18).
 * Only here is it safe to wire map/unmap listeners. */
static void xw_handle_associate(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, associate);
	struct wlr_xwayland_surface *xw = view->xw_surface;

	view->map.notify = xw_handle_map;
	wl_signal_add(&xw->surface->events.map, &view->map);
	view->unmap.notify = xw_handle_unmap;
	wl_signal_add(&xw->surface->events.unmap, &view->unmap);
}

static void xw_handle_dissociate(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, dissociate);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
}

static void xw_handle_destroy(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, destroy);
	if (view->scene_tree) {
		wlr_scene_node_destroy(&view->scene_tree->node);
		view->scene_tree = NULL;
	}
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->link);
	free(view);
}

static void xw_handle_request_activate(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_activate);
	if (view->scene_tree)   /* only meaningful while mapped */
		xw_focus(view);
}

static void xw_handle_request_configure(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_configure);
	const struct wlr_xwayland_surface_configure_event *ev = data;
	wlr_xwayland_surface_configure(view->xw_surface,
		ev->x, ev->y, ev->width, ev->height);
	if (view->scene_tree)
		wlr_scene_node_set_position(&view->scene_tree->node, ev->x, ev->y);
}

static void xw_handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wlgame_view *view = wl_container_of(listener, view, request_fullscreen);
	wlr_xwayland_surface_set_fullscreen(view->xw_surface,
		view->xw_surface->fullscreen);
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
	view->scene_tree = NULL;          /* created on map */
	xw->data         = view;

	/* The inner wlr_surface is NULL until `associate` fires, so map/unmap
	 * listeners are wired in xw_handle_associate, NOT here. Touching
	 * xw->surface at new-surface time is a NULL deref. */
	view->associate.notify = xw_handle_associate;
	wl_signal_add(&xw->events.associate, &view->associate);
	view->dissociate.notify = xw_handle_dissociate;
	wl_signal_add(&xw->events.dissociate, &view->dissociate);

	view->destroy.notify = xw_handle_destroy;
	wl_signal_add(&xw->events.destroy, &view->destroy);
	view->request_activate.notify = xw_handle_request_activate;
	wl_signal_add(&xw->events.request_activate, &view->request_activate);
	view->request_configure.notify = xw_handle_request_configure;
	wl_signal_add(&xw->events.request_configure, &view->request_configure);
	view->request_fullscreen.notify = xw_handle_request_fullscreen;
	wl_signal_add(&xw->events.request_fullscreen, &view->request_fullscreen);

	wl_list_insert(&server->views, &view->link);
	wlr_log(WLR_DEBUG, "[wlgame/xw] new surface pid=%d", xw->pid);
}
