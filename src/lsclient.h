/*
 * Client for LiveSplit's TCP server: one persistent connection, one command
 * at a time ("cmd\r\n" out, one line back), every wait bounded by a timeout.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ls_conn {
	uintptr_t sock; /* SOCKET; LS_NO_SOCKET when closed */
	int timeout_ms; /* per command */
	size_t rx_len;
	char rx[8192];
	char error[256]; /* why the last call failed */
};

#define LS_NO_SOCKET (~(uintptr_t)0)

/* WSAStartup/WSACleanup (reference counted by Winsock). */
bool ls_net_init(void);
void ls_net_cleanup(void);

void ls_conn_init(struct ls_conn *c, int timeout_ms);
bool ls_connect(struct ls_conn *c, const char *host, int port, int timeout_ms);
void ls_close(struct ls_conn *c);
bool ls_connected(const struct ls_conn *c);

/* Sends cmd and reads one response line (without "\r\n") into out. A long
 * line is truncated to fit. Returns false on failure or timeout, after which
 * the connection should be closed. */
bool ls_query(struct ls_conn *c, const char *cmd, char *out, size_t out_size);
