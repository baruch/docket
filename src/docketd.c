#include "docket.h"
#include "tar.h"
#include "special_arg.h"
#include "dev_list.h"

#include "wire.h"
#include "wire_fd.h"
#include "wire_net.h"
#include "wire_io.h"
#include "wire_pool.h"
#include "wire_stack.h"
#include "wire_lock.h"
#include "wire_log.h"
#include "macros.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <time.h>
#include <assert.h>
#include <stdarg.h>
#include <signal.h>

#define MAX_ARGS 20

static wire_thread_t wire_main;
static wire_t task_accept;
static wire_pool_t docket_pool;
static wire_pool_t exec_pool;

typedef struct docket_state {
	wire_net_t write_net;
	wire_wait_t wait;
	wire_lock_t write_lock;
	int remaining;
	int auto_close;
	char prefix[128];
	char *line;
	unsigned log_len;
	char log[512*1024];
} docket_state_t;

static void set_nonblock(int fd)
{
	int ret = fcntl(fd, F_GETFL);
	if (ret < 0)
		return;

	fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

static void set_reuse(int fd)
{
	int so_reuseaddr = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));
}

static int socket_setup(unsigned short port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		wire_log(WLOG_ERR, "Failed to create socket: %m");
		return -1;
	}

	set_nonblock(fd);
	set_reuse(fd);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret < 0) {
		wire_log(WLOG_ERR, "Failed to bind to socket on port %hd: %m", port);
		wio_close(fd);
		return -1;
	}

	ret = listen(fd, 100);
	if (ret < 0) {
		wire_log(WLOG_ERR, "Failed to listen to port %hd: %m", port);
		wio_close(fd);
		return -1;
	}

	return fd;
}

static void docket_log(docket_state_t *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void docket_log(docket_state_t *state, const char *fmt, ...)
{
	va_list ap;
	int written;
	size_t space = sizeof(state->log) - state->log_len;

	va_start(ap, fmt);
	written = vsnprintf(state->log + state->log_len, space, fmt, ap);
	va_end(ap);

	if (written > 0 && written <= space) {
		state->log_len += written;
		if (state->log_len < sizeof(state->log)) {
			state->log[state->log_len++] = '\n';
		}
	}

	//TODO: wire_logv(WLOG_INFO, fmt, ap);
}

static void remaining_dec(docket_state_t *state)
{
	state->remaining--;

	// We are the last one standing, close the connection
	if (state->auto_close && state->remaining == 0) {
		wire_wait_resume(&state->wait);
	}
}

static void send_buf(docket_state_t *state, const char *buf, unsigned buf_len)
{
	size_t sent;
	wire_net_write(&state->write_net, buf, buf_len, &sent);
}

static unsigned send_buf_zeros(docket_state_t *state, char *buf, unsigned buf_size, unsigned sendbytes)
{
	unsigned sent = 0;

	memset(buf, 0, buf_size);

	while (sent < sendbytes) {
		unsigned remaining = sendbytes - sent;
		unsigned tosend = remaining < buf_size ? remaining : buf_size;
		send_buf(state, buf, tosend);
		sent += tosend;
	}

	return sent;
}

static void send_tar_pad(docket_state_t *state, char *buf, unsigned buf_size, unsigned filesize)
{
	filesize %= 512;

	if (filesize == 0)
		return;

	filesize = 512 - filesize; // Pad to 512 bytes
	send_buf_zeros(state, buf, buf_size, filesize);
}

static void send_tar_header(docket_state_t *state, const char *dir, char *filename, int file_size)
{
	struct tar hdr;
	size_t sent;

	tar_set_header(&hdr, state->prefix, dir, filename, file_size, time(NULL));
	wire_net_write(&state->write_net, &hdr, sizeof(hdr), &sent);
}

static void send_all(docket_state_t *state, char *dir, char *filename, char *buf, int buf_len, size_t buf_sz)
{
	wire_lock_take(&state->write_lock);
	send_tar_header(state, dir, filename, buf_len);
	send_buf(state, buf, buf_len);
	send_tar_pad(state, buf, buf_sz, buf_len);
	wire_lock_release(&state->write_lock);
}

static void send_log_file(docket_state_t *state)
{
	send_all(state, ".", "docket.log", state->log, state->log_len, sizeof(state->log));
}

static void render_filename(char *filename, size_t buflen, char **cmd, const char *suffix)
{
	size_t len = 0;
	int cmd_idx = 0;
	int cmd_offset = 0;
	char *slash;

	slash = strrchr(cmd[0], '/');
	if (slash)
		cmd_offset = slash - cmd[0] + 1;

	while (len < buflen-4 && cmd_idx < MAX_ARGS && cmd[cmd_idx]) {
		switch (cmd[cmd_idx][cmd_offset]) {
			case 0:
				// Skip to the next command
				filename[len] = '_';
				cmd_offset = 0;
				cmd_idx++;
				break;
			case '/':
			case ' ':
				// Translate slash and space to underscore to avoid opening a subdirectory
				filename[len] = '_';
				cmd_offset++;
				break;
			default:
				filename[len] = cmd[cmd_idx][cmd_offset];
				cmd_offset++;
				break;
		}

		len++;
	}

	len--; // Backtrack on the last underscore
	for (cmd_offset = 0; suffix[cmd_offset] != 0 && len < buflen-1; cmd_offset++) {
		filename[len++] = suffix[cmd_offset];
	}

	filename[len] = 0;
}

static void flatten_filename(char *filename, size_t buflen, char *orig_filename)
{
	char *cmd[3] = {"", orig_filename, NULL};
	render_filename(filename, buflen, cmd, "");
}

static void file_collector(docket_state_t *state, char *dir, char *filename)
{
	int fd;
	int ret;
	struct stat stbuf;
	int nrcvd;
	char buf[900*1024];
	char flat_filename[128];

	docket_log(state, "Collect file %s", filename);

	fd = wio_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		// TODO: Log error
		docket_log(state, "Failed to open file %s: %m", filename);
		return;
	}

	ret = wio_fstat(fd, &stbuf);
	if (ret < 0) {
		// TODO: Log error
		docket_log(state, "Failed to fstat file %s: %m", filename);
		wio_close(fd);
		return;
	}

	if (!S_ISREG(stbuf.st_mode)) {
		docket_log(state, "File %s is not a regular file", filename);
		wio_close(fd);
		return;
	}

	nrcvd = wio_read(fd, buf, sizeof(buf));
	if (nrcvd < 0) {
		// TODO: Log error
		docket_log(state, "Failed to read file %s: %m\n", filename);
		wio_close(fd);
		return;
	}

	flatten_filename(flat_filename, sizeof(flat_filename), filename);

	if (stbuf.st_size == 0) {
		// Read a proc/sysfs file, unknown size, assume fitting into a fixed buffer in one read
		send_all(state, dir, flat_filename, buf, nrcvd, sizeof(buf));
	} else {
		// Read a regular file, known file in advance, requires more than one read
		int nsent = 0;
		unsigned size = stbuf.st_size;
		wire_lock_take(&state->write_lock);

		if (nrcvd < sizeof(buf)) {
			// It's possible the file size is smaller than one buffer, in which
			// case adjust the size, this is mostly relevant for sysfs files
			size = nrcvd;
		}
		send_tar_header(state, dir, flat_filename, size);

		send_buf(state, buf, nrcvd);
		nsent += nrcvd;
		while (nsent < size) {
			nrcvd = wio_read(fd, buf, sizeof(buf));
			if (nrcvd <= 0) {
				nsent += send_buf_zeros(state, buf, sizeof(buf), size - nsent);
			} else {
				send_buf(state, buf, nrcvd);
				nsent += nrcvd;
			}
		}
		send_tar_pad(state, buf, sizeof(buf), size);

		wire_lock_release(&state->write_lock);
	}

	wio_close(fd);
}

static void glob_collector(docket_state_t *state, char *dir, char *pattern)
{
	int ret;
	glob_t globbuf;
	int i;

	memset(&globbuf, 0, sizeof(globbuf));
	ret = wio_glob(pattern, GLOB_NOSORT, NULL, &globbuf);
	if (ret != 0) {
		docket_log(state, "Glob for pattern %s failed with error %d", pattern, ret);
		return;
	}

	for (i = 0; i < globbuf.gl_pathc; i++) {
		// TODO: In the future we can spawn more wires here to do all the files in parallel
		file_collector(state, dir, globbuf.gl_pathv[i]);
	}

	wio_globfree(&globbuf);
}

struct tree_args {
	docket_state_t *state;
	char *dir;
	char *basepath;
	char *name;
};

static void task_tree_collector_file(void *arg)
{
	struct tree_args *tree_args = arg;
	docket_state_t *state = tree_args->state;
	char dir[128];
	char basepath[128];

	strcpy(dir, tree_args->dir);
	snprintf(basepath, sizeof(basepath), "%s/%s", tree_args->basepath, tree_args->name);
	// At this stage we are clear to reschedule

	docket_log(state, "Tree collector for file %s", basepath);
	file_collector(state, dir, basepath);
}

static void tree_collector(docket_state_t *state, char *dir, char *basepath)
{
	DIR *dirent;
	struct dirent *entry;
	struct tree_args tree_args;
	char new_basepath[128];

	dirent = wio_opendir(basepath);
	if (!dirent) {
		docket_log(state, "Failed to open directory %s: %d (%m)", basepath, errno);
		return;
	}

	docket_log(state, "Tree collector for %s", basepath);

	tree_args.state = state;
	tree_args.dir = dir;
	tree_args.basepath = basepath;

	errno = 0;
	while ( (entry = wio_readdir(dirent)) != NULL ) {
		switch (entry->d_type) {
			case DT_DIR:
				if (entry->d_name[0] != '.') {
					snprintf(new_basepath, sizeof(new_basepath), "%s/%s", basepath, entry->d_name);
					tree_collector(state, dir, new_basepath);
				}
				break;

			case DT_REG:
				tree_args.name = entry->d_name;
				wire_pool_alloc_block(&exec_pool, "tree collector file", task_tree_collector_file, &tree_args);
				wire_yield(); // Let it copy the arguments
				break;

			default:
				docket_log(state, "Not collecting file %s in %s since it is not a file or directory", entry->d_name, basepath);
				break;
		}
		errno = 0;
	}

	wio_closedir(dirent);
}

//////
struct fd_collector_args {
	docket_state_t *state;
	int fd;
	pid_t pid;
	char dir[128];
	char filename[128];
};

static void task_fd_collector(void *arg)
{
	struct fd_collector_args args;
	char buf[900*1024];
	unsigned buf_len = 0;
	size_t nrcvd;
	int ret;
	wire_net_t net;

	// Copy the args
	memcpy(&args, arg, sizeof(args));

	// Prepare to read the data
	set_nonblock(args.fd);
	wire_net_init(&net, args.fd);
	wire_timeout_reset(&net.tout, 120 * 1000); // 120 seconds

	// Read all the data
	do {
		ret = wire_net_read_any(&net, buf+buf_len, sizeof(buf) - buf_len, &nrcvd);
		if (ret >= 0)
			buf_len += nrcvd;
	} while (ret >= 0 && nrcvd > 0 && buf_len < sizeof(buf));

	if (ret < 0 && errno != ENODATA) {
		docket_log(args.state, "Failed to read from process pipe %s: %d (%m)", args.filename, errno);
	}

	wire_net_close(&net);
	wio_kill(args.pid, 9);

	if (buf_len > 0)
		send_all(args.state, args.dir, args.filename, buf, buf_len, sizeof(buf));
	else
		docket_log(args.state, "Collected from fd size zero, not emitting file %s", args.filename);

	remaining_dec(args.state);
}

static void exec_collector_spawn_one(docket_state_t *state, char *dir, char **cmd)
{
	int out_fd;
	int err_fd;
	pid_t pid;

	pid = wio_spawn(cmd, NULL, &out_fd, &err_fd);
	if (pid < 0) {
		docket_log(state, "Failed to spawn command %s %s %s %s %s %s, errno=%d (%m)",
				cmd[0], cmd[1] ?  : "", cmd[2] ? : "", cmd[3] ? : "", cmd[4] ? : "", cmd[5] ? "..." : "",
				errno);
		return;
	}

	state->remaining += 2;

	struct fd_collector_args out_args;
	out_args.state = state;
	out_args.pid = pid;
	strncpy(out_args.dir, dir, sizeof(out_args.dir));
	out_args.dir[sizeof(out_args.dir)-1] = 0;
	out_args.fd = out_fd;
	render_filename(out_args.filename, sizeof(out_args.filename), cmd, ".out");
	wire_pool_alloc_block(&exec_pool, "fd processor", task_fd_collector, &out_args);

	struct fd_collector_args err_args;
	err_args.state = state;
	err_args.pid = pid;
	strncpy(err_args.dir, dir, sizeof(err_args.dir));
	err_args.dir[sizeof(err_args.dir)-1] = 0;
	err_args.fd = err_fd;
	render_filename(err_args.filename, sizeof(err_args.filename), cmd, ".err");
	wire_pool_alloc_block(&exec_pool, "fd processor", task_fd_collector, &err_args);

	// Let the collector wires grab their arguments from out stack
	wire_yield();
}

static void exec_collector(docket_state_t *state, char *dir, char **cmd)
{
	char ritems[32][32];
	char *items[32];
	int num_items = 0;
	char *param = NULL;
	int i;
	int special_idx;

	for (i = 0; i < 32; i++)
		items[i] = ritems[i];

	for (special_idx = 1; cmd[special_idx] != NULL; special_idx++) {
		num_items = special_arg_match(cmd[special_idx], items, ARRAY_SIZE(items), 32);
		// Only one special argument is supported
		if (num_items)
			break;
	}

	if (num_items == 0) {
		exec_collector_spawn_one(state, dir, cmd);
	} else {
		param = cmd[special_idx];
		for (i = 0; i < num_items; i++) {
			cmd[special_idx] = items[i];
			docket_log(state, "Collecting exec with parameter %s value %s", param, items[i]);
			exec_collector_spawn_one(state, dir, cmd);
		}
	}
}

static void task_line_process(void *arg)
{
	docket_state_t *state = arg;
	char *saveptr = NULL;
	char line[128];
	char *args[MAX_ARGS];
	int num_args = 0;

	// Copy the line locally to avoid it getting overrun
	strncpy(line, state->line, sizeof(line));
	line[sizeof(line)-1] = 0;
	state->line = NULL;

	// Break up the line into the different arguments, seperated by the vertical line '|'
	args[num_args] = strtok_r(line, "|", &saveptr);
	while (args[num_args] != NULL && num_args < MAX_ARGS) {
		args[++num_args] = strtok_r(NULL, "|", &saveptr);
	}

	if (num_args == 0)
		goto Exit;

	if (strcmp(args[0], "FILE") == 0) {
		if (num_args >= 3)
			file_collector(state, args[1], args[2]);
		else
			docket_log(state, "Not enough arguments to FILE collector, got %d args", num_args);
	} else if (strcmp(args[0], "GLOB") == 0) {
		if (num_args >= 3)
			glob_collector(state, args[1], args[2]);
		else
			docket_log(state, "Not enough arguments to GLOB collector, got %d args", num_args);
	} else if (strcmp(args[0], "TREE") == 0) {
		if (num_args >= 3)
			tree_collector(state, args[1], args[2]);
		else
			docket_log(state, "Not enough arguments to TREE collector, got %d args", num_args);
	} else if (strcmp(args[0], "EXEC") == 0) {
		if (num_args >= 3)
			exec_collector(state, args[1], &args[2]);
		else
			docket_log(state, "Not enough arguments to EXEC collector, got %d args", num_args);
	} else if (strcmp(args[0], "PREFIX") == 0) {
		if (num_args >= 2) {
			strncpy(state->prefix, args[1], sizeof(state->prefix));
			state->prefix[sizeof(state->prefix)-1] = 0;
		} else {
			docket_log(state, "Not enough arguments to PREFIX collector, got %d args", num_args);
		}
	} else {
		docket_log(state, "Unknown collector requested '%s'", args[0]);
	}

Exit:
	remaining_dec(state);
}

static int launch_collectors(docket_state_t *state, char *buf, size_t buf_len, size_t *processed)
{
	size_t proc = 0;
	char *newline;
	char *line = buf;
	int eof_rcvd = 0;

	while ( (newline = memchr(line, '\n', buf_len - proc)) != NULL ) {
		proc = newline - buf + 1;
		*newline = 0;

		// If the en
		if (strcmp(line, "EOF") == 0) {
			eof_rcvd = 1;
			break;
		}

		// Skip empty lines and comments
		if (line[0] != 0 && line[0] != '#') {
			state->remaining++;
			state->line = line;
			wire_pool_alloc_block(&docket_pool, "line processor", task_line_process, state);
			wire_yield(); // Wait for the wire to copy the line to itself
			assert(state->line == NULL);
		}

		line = newline+1;
	}

	*processed = proc;
	return eof_rcvd;
}

static void task_docket_run(void *arg)
{
	int fd = (long int)arg;
	int ret;
	wire_net_t net;
	char buf[32*1024];
	size_t rcvd = 0;
	int eof_rcvd = 0;
	docket_state_t state;

	set_nonblock(fd);
	wire_net_init(&net, fd);
	wire_timeout_reset(&net.tout, 120*1000);

	// Setup the write side of the socket
	wire_net_init(&state.write_net, dup(fd));
	wire_wait_init(&state.wait);
	wire_lock_init(&state.write_lock);
	state.remaining = 0;
	state.auto_close = 0;
	state.log_len = 0;

	// Do the reads
	do {
		// Receive new data
		size_t nrcvd = 0;
		ret = wire_net_read_any(&net, buf+rcvd, sizeof(buf)-rcvd, &nrcvd);
		if (ret < 0)
			break;
		rcvd += nrcvd;

		// Process as much data as possible
		eof_rcvd = launch_collectors(&state, buf, rcvd, &nrcvd);

		// Move data to beginning of buffer for next cycle
		memmove(buf, buf+nrcvd, rcvd - nrcvd);
		rcvd -= nrcvd;
	} while (!eof_rcvd);

	// Close the read side of things
	shutdown(fd, SHUT_RD);
	wire_net_close(&net);

	// If we got the full list of data, we wait to send it all
	if (eof_rcvd) {
		if (state.remaining == 0) {
			// Nothing left to wait for, we close the write fd
		} else {
			state.auto_close = 1;
			wire_wait_single(&state.wait);
		}
		docket_log(&state, "Docket collection done");
		send_log_file(&state);
		wire_net_close(&state.write_net);
	}

	wire_log(WLOG_INFO, "Collection for fd %d is done", fd);
}

static void task_accept_run(void *arg)
{
	UNUSED(arg);
	struct sockaddr sa;
	socklen_t salen;
	int ret;
	char host[32];
	char serv[32];

	wire_log(WLOG_INFO, "docketd starting up");

	int fd = socket_setup(DOCKET_PORT);
	if (fd < 0) {
		wire_log(WLOG_FATAL, "docketd failed to bind to socket, bailing out.");
		return;
	}

	wire_fd_state_t fd_state;
	wire_fd_mode_init(&fd_state, fd);

	while (1) {
		wire_fd_mode_read(&fd_state);
		wire_fd_wait(&fd_state);

		memset(&sa, 0, sizeof(sa));
		salen = sizeof(sa);
		int new_fd = accept(fd, &sa, &salen);
		if (new_fd >= 0) {
			ret = wio_getnameinfo(&sa, salen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST|NI_NUMERICSERV);
			if (ret == 0) {
				wire_log(WLOG_INFO, "New connection: fd=%d origin=%s:%s", fd, host, serv);
			} else {
				wire_log(WLOG_INFO, "New connection: fd=%d (failed to resolve address, error=%d %s)\n", new_fd, ret, gai_strerror(ret));
			}

			wire_t *task = wire_pool_alloc_block(&docket_pool, "docket", task_docket_run, (void*)(long int)new_fd);
			if (!task) {
				wire_log(WLOG_ERR, "Docket is busy, sorry\n");
				wio_close(new_fd);
			}
		} else {
			if (errno != EINTR && errno != EAGAIN) {
				wire_log(WLOG_FATAL, "Error accepting from listening socket: %m");
				break;
			}
		}
	}
}

int main()
{
	signal(SIGCHLD, SIG_IGN);

	wire_thread_init(&wire_main);
	wire_fd_init();
	wire_io_init(8);
	wire_log_init_stdout();
	wire_pool_init(&docket_pool, NULL, 32, 1024*1024);
	wire_pool_init(&exec_pool, NULL, 32, 1024*1024);
	wire_init(&task_accept, "accept", task_accept_run, NULL, WIRE_STACK_ALLOC(4096));
	dev_list_init();
	wire_thread_run();
	return 0;
}
