#pragma once

struct wlgame_server;

/* Spawn the wrapped game (and optional mangoapp overlay) once the compositor
 * socket is live. Wires a SIGCHLD handler into the event loop so the
 * compositor terminates when the game exits. No-op for a bare session. */
void launch_init(struct wlgame_server *server);

/* Tear down the SIGCHLD source and signal any surviving children so the
 * game does not outlive the compositor. */
void launch_finish(struct wlgame_server *server);
