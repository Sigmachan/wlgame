#include "server.h"
#include "output.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static struct wlr_scene_tree *layer_to_scene_tree(struct wlgame_server *server,
		enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->layer_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return server->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return server->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return server->layer_overlay;
	default:                                    return server->layer_top;
	}
}

void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct wlgame_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	/* Assign the first available output if client left it unset */
	if (!layer_surface->output) {
		struct wlgame_output *out;
		wl_list_for_each(out, &server->outputs, link) {
			layer_surface->output = out->wlr_output;
			break;
		}
		if (!layer_surface->output) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
	}

	struct wlr_scene_tree *parent =
		layer_to_scene_tree(server, layer_surface->pending.layer);

	struct wlr_scene_layer_surface_v1 *scene_layer =
		wlr_scene_layer_surface_v1_create(parent, layer_surface);
	if (!scene_layer) {
		return;
	}

	/* Configure with full output area */
	struct wlr_box full_area = {0};
	wlr_output_effective_resolution(layer_surface->output,
		&full_area.width, &full_area.height);
	struct wlr_box usable = full_area;
	wlr_scene_layer_surface_v1_configure(scene_layer, &full_area, &usable);

	wlr_log(WLR_INFO, "[wlgame/layer] new surface ns=%s layer=%d %dx%d",
		layer_surface->namespace ? layer_surface->namespace : "?",
		(int)layer_surface->pending.layer,
		full_area.width, full_area.height);
}
