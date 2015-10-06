#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include "spin_emu.h"

void spin_emu_send_mc(int fd, struct spin_emu_mc_t *mc)
{
	size_t len = mc->has_payload ? 8 : 4;
	char buf[10];
	buf[0] = SPIN_EMU_PACKET_MC;
	memcpy(buf+1, mc, len);
	if (send(fd, buf, len+1, MSG_DONTWAIT) < 0)
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			die_errno("send() for mc");
}


void spin_emu_send_route(int fd, uint entry, uint key, uint mark, uint route)
{

	char buf[1+sizeof(struct spin_emu_route_packet)];
	struct spin_emu_route_packet *p = (struct spin_emu_route_packet *) (buf+1);
	memset(buf, 0, sizeof(buf));
	buf[0] = SPIN_EMU_PACKET_ROUTE;
	p->index = entry;
	p->entry.key = key;
	p->entry.mask = mark;
	p->entry.route = route;
	if (send(fd, buf, 1+sizeof(struct spin_emu_route_packet), 0) < 0)
		die_errno("send() failed for spin1_set_mc_table_entry");
}

void spin_emu_die(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	fputs("fatal: ", stderr);
	vfprintf(stderr, msg, args);
	fputc('\n', stderr);
	exit(1);
	va_end(args);
}

void spin_emu_die_errno(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	fputs("fatal: ", stderr);
	vfprintf(stderr, msg, args);
	fprintf(stderr, ": %s\n", strerror(errno));
	exit(1);
	va_end(args);
}

void *xmalloc(size_t len)
{
	void *ret = malloc(len);
	if (!ret)
		die("out of memory");
	return ret;
}

void *xzalloc(size_t len) 
{
	void *ret = xmalloc(len);
	memset(ret, '\0', len);
	return ret;
}

int prefixcmp(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix));
}
