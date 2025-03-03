#include <fcntl.h>
#include <unistd.h>
#include "internal/internal.h"
#include "internal/eventing/epoll_kqueue.h"

#include <string>

static char usio_ibuf[65536];
static std::string usio_obuf;
static us_internal_callback_t *usio_icb = 0, *usio_ocb = 0;
static void (*usio_read_cb)(std::string_view) = 0;
static void (*usio_close_cb)() = 0;
static int fdin = 0, fdout = 1;

static us_internal_callback_t *usio__init_fd_cb(int fd, us_loop_t *loop, void (*cb)(us_internal_callback_t *)) {
	int flags;
	if((flags = fcntl(fd, F_GETFL)) == -1
			|| fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1
			|| fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
		return 0;
	us_internal_callback_t *ret_cb;
	ret_cb = (us_internal_callback_t *)us_create_poll(loop, 0, sizeof(us_internal_callback_t) - sizeof(us_poll_t));
	us_poll_init((us_poll_t *)ret_cb, fd, POLL_TYPE_CALLBACK);
	ret_cb->loop = loop;
	ret_cb->cb_expects_the_loop = 0;
	ret_cb->leave_poll_ready = 1;
	ret_cb->cb = cb;
	return ret_cb;
}

static void usio__free_cb(us_internal_callback_t **pcb) {
	us_poll_stop((us_poll_t *)*pcb, (*pcb)->loop);
	us_poll_free((us_poll_t *)*pcb, (*pcb)->loop);
	*pcb = 0;
}

#define READ_EXIT (n == 0)
#ifdef WSCAT_TTY
#include <sys/ioctl.h>
#if defined(__OpenBSD__) || defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#undef READ_EXIT
#define READ_EXIT (n < 0 && errno == EIO)
#endif

static int pty;
int pty_spawn(const char *args[], uint16_t rows, uint16_t cols) {
	int pid, flags;
	struct winsize size = {rows, cols, 0, 0};
	pid = forkpty(&pty, 0, 0, &size);
	if (pid < 0) return -1;
	else if (pid == 0) {
		setsid();
		int ret = execvp(args[0], (char * const *)args);
		if (ret < 0) {
			perror("execvp failed\n");
			_exit(-errno);
		}
	}
	return ((flags = fcntl(pty, F_GETFL)) == -1
			|| fcntl(pty, F_SETFL, flags | O_NONBLOCK) == -1
			|| fcntl(pty, F_SETFD, FD_CLOEXEC) == -1
			|| (fdin = dup(pty)) < 0 || (fdout = dup(pty)) < 0 );
}

void pty_resize(uint16_t rows, uint16_t cols) {
	struct winsize size = {rows, cols, 0, 0};
	ioctl(pty, TIOCSWINSZ, &size);
}
#endif // WSCAT_TTY

void usio_run(us_loop_t *loop, void (*rcb)(std::string_view) = 0, void (*ccb)() = 0) {
	usio_icb = usio__init_fd_cb(fdin, loop, [](us_internal_callback_t *) {
		ssize_t n = read(fdin, usio_ibuf, sizeof(usio_ibuf));
		if(n > 0 && usio_read_cb) usio_read_cb(std::string_view(usio_ibuf, (size_t)n));
		else if(READ_EXIT) {
			usio__free_cb(&usio_icb);
			usio__free_cb(&usio_ocb);
			if(usio_close_cb) usio_close_cb();
		}
	});
	usio_read_cb = rcb;
	usio_close_cb = ccb;
	us_poll_start((us_poll_t *)usio_icb, loop, LIBUS_SOCKET_READABLE);
	usio_ocb = usio__init_fd_cb(fdout, loop, [](us_internal_callback_t *cb) {
		ssize_t n = write(fdout, usio_obuf.data(), usio_obuf.length());
		if(n > 0) {
			if((size_t)n == usio_obuf.length()) {
				usio_obuf.clear();
				us_poll_stop((us_poll_t *)cb, cb->loop);
			} else usio_obuf.erase(0, (size_t)n);
		}
	});
}

void usio_write(std::string_view msg) {
	if(usio_obuf.empty()) {
		ssize_t n = write(fdout, msg.data(), msg.length());
		if(n>0) {
			if((size_t)n == msg.length()) return;
			else msg.remove_prefix((size_t)n);
		}
		us_poll_start((us_poll_t *)usio_ocb, usio_ocb->loop, LIBUS_SOCKET_WRITABLE);
	}
	usio_obuf.append(msg);
}

#include "App.h"

struct _Empty {};
us_listen_socket_t *server = 0;
std::vector<uWS::WebSocket<false, true, _Empty> *> clients;

int main(int argc, char **argv) {
	const char *path = "/", *host = "localhost";
	int port = 7681, i = 1;
	for(; i < argc && argv[i][0] == '-' && argv[i][1] && !argv[i][2]; ++i) {
		if(argv[i][1]=='p' && ++i < argc) port = atoi(argv[i]);
		else if(argv[i][1]=='i' && ++i < argc) host = argv[i];
		else if(argv[i][1]=='u' && ++i < argc) path = argv[i];
		else break;
	}
#ifdef WSCAT_TTY
	const char *args[] = {getenv("SHELL"), 0, 0, 0};
	if(i < argc) { args[1] = "-c"; args[2] = argv[i]; }
	if(pty_spawn(args, 24, 80)) return 1;
#endif // WSCAT_TTY
	auto app = uWS::App().ws<_Empty>(path, {
		.compression = uWS::CompressOptions(uWS::SHARED_COMPRESSOR|uWS::SHARED_DECOMPRESSOR),
		.maxPayloadLength = 100 * 1024 * 1024,
		.maxBackpressure = 100 * 1024 * 1024,
		.open = [](auto *ws) {
			clients.push_back(ws);
		},
		.message = [](auto *ws, std::string_view msg, uWS::OpCode op) {
			if(ws == clients.front()) {
		       		if(op == 1) usio_write(msg);
#ifndef WSCAT_TTY
				else std::cerr << msg << "\n";
#else
				else if(op == 2 && msg.length() >= 4)
					pty_resize(*(uint16_t *)msg.data(), *(uint16_t *)(msg.data()+2));
#endif // WSCAT_TTY
			}
		},
		.close = [](auto *ws, int, std::string_view) {
			if(server) {
				auto r = std::find(clients.begin(), clients.end(), ws);
				if(r != clients.end()) clients.erase(r);
			}
		}
	});
	if(port > 0) app.listen(host, port, [](auto *s) { server = s; });
	else app.listen([](auto *s) { server = s; }, host);
	usio_run((us_loop_t *)uWS::Loop::get(), [](std::string_view buf) {
			if(!clients.empty()) clients.front()->send(buf);
		}, []() {
			if(server) {
				us_listen_socket_close(false, server);
				server = 0;
			}
			for(auto *ws: clients) ws->close();
			clients.clear();
		});
	app.run();
}
