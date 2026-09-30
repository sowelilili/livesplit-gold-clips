#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "lsclient.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool ls_net_init(void)
{
	WSADATA data;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void ls_net_cleanup(void)
{
	WSACleanup();
}

void ls_conn_init(struct ls_conn *c, int timeout_ms)
{
	memset(c, 0, sizeof(*c));
	c->sock = LS_NO_SOCKET;
	c->timeout_ms = timeout_ms;
}

bool ls_connected(const struct ls_conn *c)
{
	return c->sock != LS_NO_SOCKET;
}

void ls_close(struct ls_conn *c)
{
	if (c->sock != LS_NO_SOCKET) {
		closesocket((SOCKET)c->sock);
		c->sock = LS_NO_SOCKET;
	}
	c->rx_len = 0;
}

static void set_error(struct ls_conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_error(struct ls_conn *c, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, args);
	va_end(args);
}

/* Waits until the socket is readable (or writable); false on timeout. */
static bool wait_socket(SOCKET s, bool for_write, ULONGLONG deadline)
{
	ULONGLONG now = GetTickCount64();
	fd_set fds, err_fds;
	struct timeval tv;
	ULONGLONG left;

	if (now >= deadline)
		return false;
	left = deadline - now;
	tv.tv_sec = (long)(left / 1000);
	tv.tv_usec = (long)((left % 1000) * 1000);
	FD_ZERO(&fds);
	FD_ZERO(&err_fds);
	FD_SET(s, &fds);
	FD_SET(s, &err_fds);
	return select(0, for_write ? NULL : &fds, for_write ? &fds : NULL, &err_fds, &tv) > 0;
}

bool ls_connect(struct ls_conn *c, const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *res = NULL, *ai;
	char port_text[16];
	int r;

	ls_close(c);
	c->error[0] = '\0';
	snprintf(port_text, sizeof(port_text), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	r = getaddrinfo(host, port_text, &hints, &res);
	if (r != 0) {
		set_error(c, "cannot resolve the host name (error %d)", r);
		return false;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		u_long nonblocking = 1;
		bool ok = false;

		if (s == INVALID_SOCKET) {
			set_error(c, "socket() failed (error %d)", WSAGetLastError());
			continue;
		}
		ioctlsocket(s, FIONBIO, &nonblocking);
		if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
			ok = true;
		} else if (WSAGetLastError() == WSAEWOULDBLOCK) {
			fd_set wfds, efds;
			struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
			int n;

			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			FD_SET(s, &wfds);
			FD_SET(s, &efds);
			n = select(0, NULL, &wfds, &efds, &tv);
			if (n > 0 && FD_ISSET(s, &wfds)) {
				ok = true;
			} else if (n == 0) {
				set_error(c, "no answer (is LiveSplit's TCP server started?)");
			} else {
				int err = 0, len = sizeof(err);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
				if (err == WSAECONNREFUSED)
					set_error(c, "connection refused (is LiveSplit's TCP server started?)");
				else
					set_error(c, "connect failed (error %d)", err);
			}
		} else {
			set_error(c, "connect failed (error %d)", WSAGetLastError());
		}

		if (ok) {
			BOOL nodelay = TRUE;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
			c->sock = (uintptr_t)s;
			break;
		}
		closesocket(s);
	}
	freeaddrinfo(res);
	return c->sock != LS_NO_SOCKET;
}

bool ls_query(struct ls_conn *c, const char *cmd, char *out, size_t out_size)
{
	SOCKET s = (SOCKET)c->sock;
	char line[512];
	int len, sent = 0;
	ULONGLONG deadline;

	if (out_size)
		out[0] = '\0';
	if (c->sock == LS_NO_SOCKET) {
		set_error(c, "not connected");
		return false;
	}
	len = snprintf(line, sizeof(line), "%s\r\n", cmd);
	if (len < 0 || (size_t)len >= sizeof(line)) {
		set_error(c, "command too long");
		return false;
	}

	/* Responses come strictly one per command; anything left over belongs
	 * to nothing we asked for. */
	c->rx_len = 0;
	deadline = GetTickCount64() + (ULONGLONG)c->timeout_ms;

	while (sent < len) {
		int r = send(s, line + sent, len - sent, 0);
		if (r > 0) {
			sent += r;
		} else if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
			if (!wait_socket(s, true, deadline)) {
				set_error(c, "timed out sending a command");
				return false;
			}
		} else {
			set_error(c, "send failed (error %d)", WSAGetLastError());
			return false;
		}
	}

	for (;;) {
		char *nl = memchr(c->rx, '\n', c->rx_len);
		int r;

		if (nl) {
			size_t line_len = (size_t)(nl - c->rx);
			size_t n = line_len;
			if (n > 0 && c->rx[n - 1] == '\r')
				n--;
			if (out_size) {
				if (n >= out_size)
					n = out_size - 1;
				memcpy(out, c->rx, n);
				out[n] = '\0';
			}
			c->rx_len -= line_len + 1;
			memmove(c->rx, nl + 1, c->rx_len);
			return true;
		}
		if (c->rx_len == sizeof(c->rx)) {
			/* keep the start of an overlong line, drop the rest */
			c->rx_len = sizeof(c->rx) / 2;
		}

		r = recv(s, c->rx + c->rx_len, (int)(sizeof(c->rx) - c->rx_len), 0);
		if (r > 0) {
			c->rx_len += (size_t)r;
		} else if (r == 0) {
			set_error(c, "LiveSplit closed the connection");
			return false;
		} else if (WSAGetLastError() == WSAEWOULDBLOCK) {
			if (!wait_socket(s, false, deadline)) {
				set_error(c, "no response to \"%s\" within %d ms", cmd, c->timeout_ms);
				return false;
			}
		} else {
			int err = WSAGetLastError();
			if (err == WSAECONNRESET || err == WSAECONNABORTED)
				set_error(c, "LiveSplit closed the connection");
			else
				set_error(c, "receive failed (error %d)", err);
			return false;
		}
	}
}
