#include "docket.h"
#include "tar.h"

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

#define MAX_ARGS 20

static wire_thread_t wire_main;
static wire_t task_accept;
static wire_pool_t docket_pool;

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

static void send_all(docket_state_t *state, char *dir, char *filename, char *buf, int buf_len)
{
	wire_lock_take(&state->write_lock);
	send_tar_header(state, dir, filename, buf_len);
	send_buf(state, buf, buf_len);
	send_tar_pad(state, buf, buf_len, buf_len);
	wire_lock_release(&state->write_lock);
}

static void send_log_file(docket_state_t *state)
{
	send_all(state, ".", "docket.log", state->log, state->log_len);
}

static void file_collector(docket_state_t *state, char *dir, char *filename)
{
	int fd;
	int ret;
	struct stat stbuf;
	char buf[48*1024];
	int nrcvd;

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

	if (stbuf.st_size == 0) {
		// Read a proc/sysfs file, unknown size, assume fitting into a fixed buffer in one read
		send_all(state, dir, filename, buf, nrcvd);
	} else {
		// Read a regular file, known file in advance, requires more than one read
		int nsent = 0;
		wire_lock_take(&state->write_lock);
		send_tar_header(state, dir, filename, stbuf.st_size);

		send_buf(state, buf, nrcvd);
		nsent += nrcvd;
		while (nsent < stbuf.st_size) {
			nrcvd = wio_read(fd, buf, sizeof(buf));
			if (nrcvd <= 0) {
				nsent += send_buf_zeros(state, buf, sizeof(buf), stbuf.st_size - nsent);
			} else {
				send_buf(state, buf, nrcvd);
				nsent += nrcvd;
			}
		}
		send_tar_pad(state, buf, sizeof(buf), stbuf.st_size);

		wire_lock_release(&state->write_lock);
	}

	wio_close(fd);
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
	state->remaining--;

	// We are the last one standing, close the connection
	if (state->auto_close && state->remaining == 0) {
		wire_wait_resume(&state->wait);
	}
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

		state->remaining++;
		state->line = line;
		wire_pool_alloc_block(&docket_pool, "line processor", task_line_process, state);
		wire_yield(); // Wait for the wire to copy the line to itself
		assert(state->line == NULL);
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
	wire_thread_init(&wire_main);
	wire_fd_init();
	wire_io_init(8);
	wire_log_init_stdout();
	wire_pool_init(&docket_pool, NULL, 32, 1024*1024);
	wire_init(&task_accept, "accept", task_accept_run, NULL, WIRE_STACK_ALLOC(4096));
	wire_thread_run();
	return 0;
}
