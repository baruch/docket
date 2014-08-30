#include "docket.h"
#include "tar.h"

#include "wire.h"
#include "wire_pool.h"
#include "wire_io.h"
#include "wire_lock.h"
#include "wire_net.h"
#include "wire_log.h"
#include "wire_stack.h"

#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <memory.h>

static wire_thread_t wire_main;
static wire_pool_t docket_pool;
static wire_lock_t out_lock;
static wire_net_t out_net;
static wire_t stdin_wire;

static int docket_send_collection(wire_net_t *net, const char *ip, const char *name, const char *listfile)
{
	size_t nrcvd;
	size_t nsent;
	int res = -1;
	int ret;
	int fd;
	char buf[48*1024];

	ret = snprintf(buf, sizeof(buf), "PREFIX|%s\n", name);
	nrcvd = ret;
	ret = wire_net_write(net, buf, nrcvd, &nsent);
	if (ret < 0 || nrcvd != nsent) {
		wire_log(WLOG_ERR, "Not all data sent for prefix ret=%d nsent=%u nrcvd=%u", ret, nsent, nrcvd);
		return -1;
	}

	fd = wio_open(listfile, O_RDONLY, 0);
	if (fd < 0)
		return -1;

	while (1) {
		ret = wio_read(fd, buf, sizeof(buf));
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			else
				break;
		}

		if (ret == 0) {
			// EOF
			res = 0;
			break;
		}

		nrcvd = ret;
		ret = wire_net_write(net, buf, nrcvd, &nsent);
		if (ret < 0 || nsent != nrcvd) {
			wire_log(WLOG_ERR, "Not all data sent ret=%d nsent=%u nrcvd=%u", ret, nsent, nrcvd);
			break;
		}
	}

	wio_close(fd);
	shutdown(net->fd_state.fd, SHUT_WR);
	return res;
}

static int docket_collect_tar(wire_net_t *net, const char *ip)
{
	int ret;
	int i;
	size_t nrcvd;
	size_t nsent;
	char buf[48*1024];
	unsigned file_len = 0;
	struct tar *tar;

	wire_timeout_reset(&net->tout, 120*1000);

	ret = wire_net_read_full(net, buf, 512, &nrcvd);
	if (ret < 0 || nrcvd != 512) {
		wire_log(WLOG_ERR, "Error receiving tar header ret=%d errno=%d (%m)", ret, errno);
		return -1;
	}

	tar = (struct tar *)buf;

	for (i = 0; i < sizeof(tar->filesize) && tar->filesize[i]; i++) {
		unsigned digit = tar->filesize[i] - '0';
		file_len *= 8;
		file_len += digit;
	}

	wire_log(WLOG_DEBUG, "tar header for %s file size %s decimal %u", tar->filename, tar->filesize, file_len);

	if (file_len % 512 != 0)
		file_len += 512 - (file_len % 512);

	wire_log(WLOG_DEBUG, "rounded tar file size %u", file_len);

	wire_lock_take(&out_lock);

	ret = wire_net_write(&out_net, buf, 512, &nsent);
	if (ret < 0 || nsent != 512) {
		wire_log(WLOG_FATAL, "Error writing tar data, it will get mixed up, aborting.");
		wire_fd_wait_msec(100);
		abort();
	}

	while (file_len > 0) {
		wire_timeout_reset(&net->tout, 120*1000);

		size_t toread = file_len > sizeof(buf) ? sizeof(buf) : file_len;
		ret = wire_net_read_full(net, buf, toread, &nrcvd);
		if ((ret < 0 && errno != ENODATA) || nrcvd != toread) {
			// TODO: Can improve things by filling up with zeroes as needed, at least other sources will succeed to be collected
			wire_log(WLOG_FATAL, "Error reading buffer data, it will get mixed up, aborting. ret=%d nrcvd=%u toread=%u errno=%d (%m)", ret, nrcvd, toread, errno);
			wire_fd_wait_msec(100);
			abort();
		}

		ret = wire_net_write(&out_net, buf, toread, &nsent);
		if (ret < 0 || nsent != toread) {
			wire_log(WLOG_FATAL, "Error writing buffer data, it will get mixed up, aborting.");
			wire_fd_wait_msec(100);
			abort();
		}

		file_len -= toread;
	}

	wire_lock_release(&out_lock);
	return 0;
}

static void docket_collect_stream(wire_net_t *net, const char *ip)
{
	while (docket_collect_tar(net, ip) == 0) {
		// Let other sources write their output too for fairness
		wire_yield();
	}
}

static void docket_collect(void *arg)
{
	char *line = arg;
	char *ip;
	char *name;
	char *listfile;
	char *saveptr;
	int ret;
	wire_net_t net;

	ip = strtok_r(line, " \t", &saveptr);
	if (!ip) {
		wire_log(WLOG_ERR, "No IP on the line: '%s'", line);
		return;
	}
	name = strtok_r(NULL, " \t", &saveptr);
	if (!name) {
		wire_log(WLOG_ERR, "No Name on the line");
		return;
	}
	listfile = strtok_r(NULL, " \t", &saveptr);
	if (!listfile) {
		wire_log(WLOG_ERR, "No list file on the line");
		return;
	}

	ip = strdup(ip);
	name = strdup(name);
	listfile = strdup(listfile);

	wire_log(WLOG_INFO, "Connecting to %s", ip);

	ret = wire_net_init_tcp_connected(&net, ip, "7000", 10*1000, NULL, NULL);
	if (ret == 0) {
		ret = docket_send_collection(&net, ip, name, listfile);
		if (ret == 0) {
			wire_log(WLOG_INFO, "Waiting for data from %s", ip);
			docket_collect_stream(&net, ip);
		} else {
			wire_log(WLOG_ERR, "Error writing orders to %s", ip);
		}
		wire_net_close(&net);
	} else {
		wire_log(WLOG_ERR, "Error connecting to %s: %d (%m)", ip, errno);
	}

	wire_log(WLOG_INFO, "Connection to %s finished", ip);

	free(ip);
	free(name);
	free(listfile);
}

static size_t process_stdin(char *buf, size_t buf_len)
{
	char *eol;
	char *line;
	size_t processed = 0;

	while (processed < buf_len) {
		line = buf + processed;
		eol = memchr(line, '\n', buf_len - processed);
		if (!eol)
			break;

		*eol = 0;
		processed = eol - buf + 1;

		if (line[0] == '#')
			continue;

		wire_pool_alloc_block(&docket_pool, "collect", docket_collect, line);
	}
	// Let all the new wires copy their argument from our stack
	wire_yield();

	return processed;
}

static void stdin_processor(void *arg)
{
	UNUSED(arg);

	wire_net_t net;
	size_t nrcvd;
	size_t buf_len;
	size_t processed;
	int ret;
	char buf[32*1024];

	wire_log(WLOG_INFO, "stdin processing starting");

	wire_net_init(&net, 0);

	buf_len = 0;
	do {
		ret = wire_net_read_any(&net, buf + buf_len, sizeof(buf) - buf_len, &nrcvd);
		if (ret >= 0) {
			buf_len += nrcvd;
			processed = process_stdin(buf, buf_len);
			if (processed != buf_len) {
				// move data to start of buffer
				size_t remaining = buf_len - processed;
				memmove(buf, buf + processed, remaining);
				buf_len = remaining;
			}
		}
	} while (ret >= 0);

	wire_net_close(&net);

	process_stdin(buf, buf_len);
	wire_log(WLOG_INFO, "stdin processing done");
}

int main()
{
	signal(SIGPIPE, SIG_IGN);

	wire_thread_init(&wire_main);
	wire_fd_init();
	wire_io_init(2);
	wire_log_init_stderr();
	wire_pool_init(&docket_pool, NULL, 64, 64*1024);
	wire_lock_init(&out_lock);
	wire_net_init(&out_net, 1);
	wire_init(&stdin_wire, "stdin processor", stdin_processor, NULL, WIRE_STACK_ALLOC(64*1024));
	wire_thread_run();
	return 0;
}
