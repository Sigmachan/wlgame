#include "launch.h"
#include "server.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

/* Fork+exec a child in its own session/process-group so a single SIGTERM to
 * the compositor doesn't race the kernel into delivering it to the child too,
 * and so we can signal the whole group on shutdown. Returns -1 on failure. */
static pid_t spawn(char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "[wlgame/launch] fork failed");
		return -1;
	}
	if (pid == 0) {
		setsid();
		/* Reset signal mask/dispositions inherited from the compositor. */
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execvp(argv[0], argv);
		wlr_log_errno(WLR_ERROR, "[wlgame/launch] exec '%s' failed", argv[0]);
		_exit(127);
	}
	return pid;
}

static int handle_sigchld(int sig, void *data) {
	struct wlgame_server *server = data;
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == server->child_pid) {
			server->child_exited = true;
			server->child_status = status;
			if (WIFEXITED(status)) {
				wlr_log(WLR_INFO, "[wlgame/launch] game exited code=%d — shutting down",
					WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				wlr_log(WLR_INFO, "[wlgame/launch] game killed by signal %d — shutting down",
					WTERMSIG(status));
			}
			wl_display_terminate(server->display);
		} else if (pid == server->mango_pid) {
			server->mango_pid = 0; /* overlay died; keep running */
		}
	}
	return 0;
}

void launch_init(struct wlgame_server *server) {
	if (!server->child_argv) {
		return; /* bare session — nothing to launch */
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	server->sigchld_source =
		wl_event_loop_add_signal(loop, SIGCHLD, handle_sigchld, server);

	/* Native-Wayland hints for the game and its toolkits. Children inherit
	 * the environment, so set it before the first fork. Opt-in only — many
	 * Proton titles still run better through XWayland. */
	if (server->prefer_wayland) {
		setenv("PROTON_ENABLE_WAYLAND", "1", 1);
		setenv("SDL_VIDEODRIVER", "wayland,x11", 1);
		setenv("GDK_BACKEND", "wayland,x11", 1);
		setenv("QT_QPA_PLATFORM", "wayland;xcb", 1);
		setenv("CLUTTER_BACKEND", "wayland", 1);
		setenv("MOZ_ENABLE_WAYLAND", "1", 1);
	}

	/* mangoapp is a layer-shell client that renders the MangoHud overlay. */
	if (server->mangoapp) {
		char *const mango[] = { "mangoapp", NULL };
		server->mango_pid = spawn(mango);
		if (server->mango_pid < 0) {
			wlr_log(WLR_ERROR,
				"[wlgame/launch] mangoapp not found — install mangohud");
		}
	}

	server->child_pid = spawn(server->child_argv);
	if (server->child_pid < 0) {
		wlr_log(WLR_ERROR, "[wlgame/launch] failed to spawn game — terminating");
		wl_display_terminate(server->display);
		return;
	}
	wlr_log(WLR_INFO, "[wlgame/launch] launched '%s' pid=%d",
		server->child_argv[0], server->child_pid);
}

void launch_finish(struct wlgame_server *server) {
	if (server->sigchld_source) {
		wl_event_source_remove(server->sigchld_source);
		server->sigchld_source = NULL;
	}
	/* Compositor is exiting first (Super+Q / SIGTERM): take the children with
	 * us. Signal the whole process group (negative pid) to catch wrappers
	 * like `bash -c` and Proton's umu/reaper. */
	if (server->child_pid > 0 && !server->child_exited) {
		kill(-server->child_pid, SIGTERM);
	}
	if (server->mango_pid > 0) {
		kill(-server->mango_pid, SIGTERM);
	}
}
