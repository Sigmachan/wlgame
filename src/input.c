
#include "server.h"
#include "view.h"

#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_data_device.h>

struct wlgame_keyboard {
	struct wl_list link;
	struct wlgame_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct wlgame_keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct wlgame_keyboard *kb = wl_container_of(listener, kb, key);
	struct wlgame_server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	/* Alt+F4 closes focused window; Super+Q quits */
	if ((mods & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_F4) {
				struct wlgame_view *view;
				wl_list_for_each(view, &server->views, link) {
					if (view->xdg_toplevel->base->surface->mapped) {
						wlr_xdg_toplevel_send_close(view->xdg_toplevel);
						handled = true;
						break;
					}
				}
			}
		}
	}
	if ((mods & WLR_MODIFIER_LOGO) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_q || syms[i] == XKB_KEY_Q) {
				wl_display_terminate(server->display);
				handled = true;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct wlgame_keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

static void server_new_keyboard(struct wlgame_server *server, struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct wlgame_keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = server;
	kb->wlr_keyboard = wlr_keyboard;

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	kb->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);
	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
}

static void server_new_pointer(struct wlgame_server *server, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (wlr_seat_get_keyboard(server->seat)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

void server_seat_request_cursor(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, seat_request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

void server_seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, seat_request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlgame_view *view = view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!view) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlgame_view *view = view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!view) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlgame_view *view = view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		return;
	}
	view_focus(view, surface);
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct wlgame_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}
