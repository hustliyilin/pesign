/*
 * Copyright 2012 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include "pesign.h"

static int should_exit = 0;

typedef struct {
	cms_context *cms;
	uid_t uid;
	gid_t gid;
	pid_t pid;
	int sd;
	int priority;
	char *errstr;
} context;

static void
send_response(context *ctx, struct pollfd *pollfd, int rc, char *errmsg)
{	
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	int msglen = errmsg ? strlen(errmsg) + 1 : 0;

	iov.iov_len = sizeof(pesignd_msghdr) + sizeof(pesignd_cmd_response)
			+ msglen;

	void *buffer = calloc(1, iov.iov_len);
	if (!buffer) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: could not allocate memory: %m");
		exit(1);
	}

	iov.iov_base = buffer;

	pesignd_msghdr *pm = buffer;
	pesignd_cmd_response *resp = (pesignd_cmd_response *)((uint8_t *)pm +
					offsetof(pesignd_msghdr, size) +
					sizeof (pm->size));

	pm->version = PESIGND_VERSION;
	pm->command = CMD_RESPONSE;
	pm->size = sizeof(resp->rc) + msglen;

	memset(&msg, '\0', sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	resp->rc = rc;
	if (errmsg)
		memcpy(resp->errmsg, errmsg, msglen);

	n = sendmsg(pollfd->fd, &msg, 0);
	if (n < 0)
		ctx->cms->log(ctx->cms, ctx->priority|LOG_WARNING,
			"pesignd: could not send response to client: %m");

	free(buffer);
}

static void
handle_kill_daemon(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	exit(0);
}

static void
handle_unlock_token(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	char *buffer = malloc(size);

	if (!buffer) {
oom:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	pesignd_string *tn = (pesignd_string *)buffer;
	if (n < sizeof(tn->size)) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: possible exploit attempt. exiting.");
		exit(1);
	}
	n -= sizeof(tn->size);
	if (n < tn->size)
		goto malformed;
	n -= tn->size;

	if (tn->value[tn->size - 1] != '\0')
		goto malformed;

	pesignd_string *tp = pesignd_string_next(tn);
	if (n < sizeof(tp->size))
		goto malformed;
	n -= sizeof(tp->size);
	if (n < tp->size)
		goto malformed;
	n -= tp->size;

	if (tn->value[tn->size - 1] != '\0')
		goto malformed;

	if (n != 0)
		goto malformed;

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
		"pesignd: unlocking token \"%s\"", tn->value);

	/* authenticating with nss frees these ... best API ever. */
	ctx->cms->tokenname = strdup((char *)tn->value);
	if (!ctx->cms->tokenname)
		goto oom;

	char *pin = strdup((char *)tp->value);
	if (!pin)
		goto oom;

	cms_set_pw_callback(ctx->cms, get_password_passthrough);
	cms_set_pw_data(ctx->cms, pin);

	int rc = unlock_nss_token(ctx->cms);

	cms_set_pw_callback(ctx->cms, get_password_fail);
	cms_set_pw_data(ctx->cms, NULL);

	if (rc == 0)
		ctx->cms->log(ctx->cms, LOG_NOTICE, "pesignd: Authentication "
			"succeeded for token \"%s\"", tn->value);

	send_response(ctx, pollfd, rc, ctx->errstr);
	free(buffer);
}

static void
socket_get_fd(context *ctx, int sd, int *fd)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[2];

	size_t controllen = CMSG_SPACE(sizeof(int));
	struct cmsghdr *cm = malloc(controllen);
	if (!cm) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));
	iov.iov_base = buf;
	iov.iov_len = 2;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cm;
	msg.msg_controllen = controllen;

	ssize_t n;
	n = recvmsg(sd, &msg, MSG_WAITALL);
	if (n < 0) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: possible exploit attempt. exiting.");
		exit(1);
	}

	struct cmsghdr *cme = CMSG_FIRSTHDR(&msg);

	if (cme == NULL)
		goto malformed;

	if (cme->cmsg_level != SOL_SOCKET)
		goto malformed;

	if (cme->cmsg_type != SCM_RIGHTS)
		goto malformed;

	*fd = *((int *)CMSG_DATA(cme));

	free(cm);
}

static void
handle_sign_attached(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	char *buffer = malloc(size);

	if (!buffer) {
oom:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	pesignd_string *tn = (pesignd_string *)buffer;
	if (n < sizeof(tn->size)) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: possible exploit attempt. exiting.");
		exit(1);
	}

	n -= sizeof(tn->size);
	if (n < tn->size)
		goto malformed;
	n -= tn->size;

	/* authenticating with nss frees these ... best API ever. */
	ctx->cms->tokenname = strdup((char *)tn->value);
	if (!ctx->cms->tokenname)
		goto oom;

	if (n < sizeof(tn->size))
		goto malformed;
	pesignd_string *cn = pesignd_string_next(tn);
	n -= sizeof(cn->size);
	if (n < cn->size)
		goto malformed;

	ctx->cms->certname = strdup((char *)cn->value);
	if (!ctx->cms->certname)
		goto oom;

	n -= cn->size;
	if (n != 0)
		goto malformed;

	int infd=-1;
	socket_get_fd(ctx, pollfd->fd, &infd);

	int outfd=-1;
	socket_get_fd(ctx, pollfd->fd, &outfd);

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
		"pesignd: attempting to sign with key \"%s:%s\"",
		tn->value, cn->value);
	free(buffer);

	int rc = find_certificate(ctx->cms);
	if (rc < 0) {
		goto finish;
	}

finish:
	close(infd);
	close(outfd);

	send_response(ctx, pollfd, rc, ctx->errstr);
}

static void
handle_sign_detached(context *ctx, struct pollfd *pollfd, socklen_t size)
{

}

static void
__attribute__((noreturn))
handle_invalid_input(pesignd_cmd cmd, context *ctx, struct pollfd *pollfd,
			socklen_t size)
{
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: got unexpected command 0x%x", cmd);
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: possible exploit attempt. exiting.");
		exit(1);
}

typedef void (*cmd_handler)(context *ctx, struct pollfd *pollfd,
				socklen_t size);

typedef struct {
	pesignd_cmd cmd;
	cmd_handler func;
} cmd_table_t;

cmd_table_t cmd_table[] = {
		{ CMD_KILL_DAEMON, handle_kill_daemon },
		{ CMD_UNLOCK_TOKEN, handle_unlock_token },
		{ CMD_SIGN_ATTACHED, handle_sign_attached },
		{ CMD_SIGN_DETACHED, handle_sign_detached },
		{ CMD_RESPONSE, NULL },
		{ CMD_LIST_END, NULL }
	};

static int
handle_event(context *ctx, struct pollfd *pollfd)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	pesignd_msghdr pm;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = &pm;
	iov.iov_len = sizeof(pm);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	char control[1024];
	msg.msg_controllen = 1024;
	msg.msg_control = control;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);
	if (n < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_WARNING,
			"pesignd: recvmsg failed: %m");
		return n;
	}

	if (pm.version != PESIGND_VERSION) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: got version %d, expected version %d",
			pm.version, PESIGND_VERSION);
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: possible exploit attempt. exiting.");
		exit(1);
	}

	for (int i = 0; cmd_table[i].cmd != CMD_LIST_END; i++) {
		if (cmd_table[i].cmd == pm.command) {
			if (cmd_table[i].func == NULL)
				handle_invalid_input(pm.command, ctx, pollfd,
							pm.size);
			cmd_table[i].func(ctx, pollfd, pm.size);
			return 0;
		}
	}

	handle_invalid_input(pm.command, ctx, pollfd, pm.size);
	return 0;
}

static int
handle_events(context *ctx)
{
	int rc;
	int nsockets = 1;

	struct pollfd *pollfds = calloc(1, sizeof(struct pollfd));

	if (!pollfds) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: could not allocate memory: %m");
		exit(1);
	}

	pollfds[0].fd = ctx->sd;
	pollfds[0].events = POLLIN|POLLPRI|POLLHUP;

	while (1) {
		rc = ppoll(pollfds, nsockets, NULL, NULL);
		if (should_exit != 0)
			exit(0);
		if (rc < 0) {
			ctx->cms->log(ctx->cms, ctx->priority|LOG_WARNING,
				"pesignd: ppoll: %m");
			continue;
		}

		if (pollfds[0].revents & POLLIN) {
			nsockets++;
			struct pollfd *newpollfds = realloc(pollfds,
				nsockets * sizeof(struct pollfd));

			if (!newpollfds) {
				ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
					"pesignd: could not allocate memory: "
					"%m");
				exit(1);
			}
			pollfds = newpollfds;

			struct sockaddr_un remote;
			socklen_t len = sizeof(remote);
			pollfds[nsockets-1].fd = accept(pollfds[0].fd, &remote,
							&len);
			pollfds[nsockets-1].events = POLLIN|POLLPRI|POLLHUP;
			pollfds[nsockets-1].revents = pollfds[0].revents;
		}
		for (int i = 1; i < nsockets; i++) {
		new_poll_result:
			if (pollfds[i].revents & (POLLHUP|POLLNVAL)) {
				close(pollfds[i].fd);
				if (i == nsockets-1) {
					nsockets--;
					continue;
				}
				for (int j = i; j < nsockets - 1; j++) {
					pollfds[j].fd = pollfds[j+1].fd;
					pollfds[j].events =
						pollfds[j].events;
					pollfds[j].revents =
						pollfds[j].revents;
				}
				nsockets--;
				goto new_poll_result;
			}

			if (pollfds[i].revents & (POLLIN|POLLPRI))
				handle_event(ctx, &pollfds[i]);
		}
	}
	return 0;
}

static int
get_uid_and_gid(context *ctx, char **homedir)
{
	struct passwd *passwd;

	passwd = getpwnam("pesign");

	if (!passwd)
		return -1;

	ctx->uid = passwd->pw_uid;
	ctx->gid = passwd->pw_gid;

	if (ctx->uid == 0 || ctx->gid == 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: cowardly refusing to start with uid = %d "
			"and gid = %d", ctx->uid, ctx->gid);
		errno = EINVAL;
		return -1;
	}

	*homedir = passwd->pw_dir;

	return 0;
}

static void
quit_handler(int signal)
{
	should_exit = 1;
}

static void
announce_exit(int status, void *private)
{
	context *ctx = (context *)private;
	if (ctx->sd >= 0) {
		close(ctx->sd);
		unlink(SOCKPATH);
	}
	if (ctx->errstr)
		ctx->cms->log(ctx->cms,
			ctx->priority | (status == 0 ? LOG_NOTICE : LOG_ERR),
			ctx->errstr);

	ctx->cms->log(ctx->cms,
		ctx->priority | (status == 0 ? LOG_NOTICE : LOG_ERR),
		"pesignd exiting (pid %d)", getpid());
}

static int
set_up_socket(context *ctx)
{
	int sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sd < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to create socket: %m");
		exit(1);
	}

	int one = 1;
	int rc = setsockopt(sd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to set socket options: %m");
		exit(1);
	}

	struct sockaddr_un addr_un = {
		.sun_family = AF_UNIX,
		.sun_path = SOCKPATH,
	};

	rc = bind(sd, &addr_un, sizeof(addr_un));
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to bind to \"%s\": %m",
			addr_un.sun_path);
		exit(1);
	}
	rc = chmod(SOCKPATH, 0660);
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: could not set permissions for \"%s\": %m",
			SOCKPATH);
		exit(1);
	}

	rc = listen(sd, 5);
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"pesignd: unable to listen on socket: %m");
		exit(1);
	}


	ctx->sd = sd;
	return 0;
}

static void
check_socket(context *ctx)
{
	errno = 0;
	int rc = access(SOCKPATH, R_OK);
	if (rc == 0) {
		struct sockaddr_un addr_un = {
			.sun_family = AF_UNIX,
			.sun_path = SOCKPATH,
		};

		int sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (sd < 0) {
			fprintf(stderr, "pesignd: unable to create socket: "
				"%m");
			exit(1);
		}

		socklen_t len = strlen(addr_un.sun_path) +
				sizeof(addr_un.sun_family);

		rc = connect(sd, (struct sockaddr *)&addr_un, len);
		if (rc < 0) {
			unlink(SOCKPATH);
			return;
		}

		struct sockaddr_un remote;
		socklen_t size = sizeof(remote);
		rc = getpeername(sd, &remote, &size);
		if (rc < 0) {
			return;
		} else {
			fprintf(stderr, "pesignd: already running");
			exit(1);
		}
	} else {
		/* It could be something other than EEXIST, but it really
		 * doesn't matter since the daemon isn't running.  Blindly
		 * remove it. */
		unlink(SOCKPATH);
	}
}

static int
__attribute__ ((format (printf, 3, 4)))
daemon_logger(cms_context *cms, int priority, char *fmt, ...)
{
	context *ctx = (context *)cms->log_priv;
	va_list ap;
	int rc = 0;

	if (ctx->errstr)
		xfree(ctx->errstr);

	va_start(ap, fmt);
	if (priority & LOG_ERR) {
		va_list aq;

		va_copy(aq, ap);
		rc = vasprintf(&ctx->errstr, fmt, aq);
		va_end(aq);
	}

	vsyslog(ctx->priority | priority, fmt, ap);
	va_end(ap);
	return rc;
}

int
daemonize(cms_context *cms_ctx, int do_fork)
{
	int rc = 0;
	context ctx = { 
		.cms = cms_ctx,
		.priority = do_fork ? LOG_PID
				    : LOG_PID|LOG_PERROR,
	};

	ctx.cms = cms_ctx;
	ctx.cms->log_priv = &ctx;
	ctx.sd = -1;

	if (getuid() != 0) {
		fprintf(stderr, "pesignd must be started as root");
		exit(1);
	}
		
	check_socket(&ctx);

	openlog("pesignd", LOG_PID, LOG_DAEMON);


	if (do_fork) {
		pid_t pid;

		if ((pid = fork()))
			return 0;
	}
	ctx.pid = getpid();
	ctx.cms->log(ctx.cms, ctx.priority|LOG_NOTICE,
		"pesignd starting (pid %d)", ctx.pid);
	daemon_logger(ctx.cms, ctx.priority|LOG_NOTICE,
		"pesignd starting (pid %d)", ctx.pid);

	rc = on_exit(announce_exit, &ctx);
	if (rc < 0) {
		ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
			"pesignd: could not register exit handler: %m");
		exit(1);
	}

	int fd = open("/dev/zero", O_RDONLY);
	close(STDIN_FILENO);
	rc = dup2(fd, STDIN_FILENO);
	if (rc < 0) {
		ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
			"pesignd: could not set up standard input: %m");
		exit(1);
	}
	close(fd);

	fd = open("/dev/null", O_WRONLY);
	close(STDOUT_FILENO);
	rc = dup2(fd, STDOUT_FILENO);
	if (rc < 0) {
		ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
			"pesignd: could not set up standard output: %m");
		exit(1);
	}

	close(STDERR_FILENO);
	rc = dup2(fd, STDERR_FILENO);
	if (rc < 0) {
		ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
			"pesignd: could not set up standard error: %m");
		exit(1);
	}
	close(fd);

	prctl(PR_SET_NAME, "pesignd", 0, 0, 0);

	setsid();

	if (do_fork) {
		signal(SIGTTOU, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);
		signal(SIGTSTP, SIG_IGN);
		signal(SIGQUIT, quit_handler);
		signal(SIGINT, quit_handler);
		signal(SIGTERM, quit_handler);
	}

	char *homedir = NULL;

	rc = get_uid_and_gid(&ctx, &homedir);
	if (rc < 0) {
		ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
			"pesignd: could not get group and user information "
			"for pesign: %m");
		exit(1);
	}

	chdir(homedir ? homedir : "/");

	if (getuid() == 0) {
		/* process is running as root, drop privileges */
		if (setgid(ctx.gid) != 0) {
			ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
				"pesignd: unable to drop group privileges: %m");
			exit(1);
		}
		if (setuid(ctx.uid) != 0) {
			ctx.cms->log(ctx.cms, ctx.priority|LOG_ERR,
				"pesignd: unable to drop user privileges: %m");
			exit(1);
		}
	}

	set_up_socket(&ctx);

	cms_set_pw_callback(ctx.cms, get_password_fail);
	cms_set_pw_data(ctx.cms, NULL);
	ctx.cms->log = daemon_logger;

	rc = handle_events(&ctx);
	return rc;
}
