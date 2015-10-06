#ifndef _SPIN_EMU_H_
#define _SPIN_EMU_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <netinet/in.h>
#include <assert.h>

/* suppress warning */
#undef NULL
#include "spin1_api.h"


//  Making topology defined by SDP command packet (e.g. in ybug this is `p2pc x y`)
extern int NUM_CHIPS_X;
extern int NUM_CHIPS_Y;
extern int NUM_CHIPS;

// spinnaker.h defines NUM_LINKS
#define NUM_ROUTES 1024

// NOTE: ybug, the visualiser, and this emulator might be on the same machine
#define SPIN_EMU_PORT 17893

//for simulations that require 1 millisecond ticks, slow the emulation by config variable
extern int SLOW_DOWN_TIMER;

extern char *spin_emu_tmpdir;
extern int spin_emu_ether_fd;
extern int spin_emu_chip;
extern int spin_emu_core;
// Initialised variable which gets set by the P2PC command from ybug
extern int initalised;



// FD for sockets to each core process
extern int **spin_emu_socket_dispatch;
extern int **spin_emu_socket_core;
#define SPIN_EMU_SOCK 5
extern int **spin_emu_running;
extern int **spin_emu_child_pid;
extern int **channel_targets;

struct spin_emu_route_t {
	uint key;
	uint mask;
	uint route;
};
extern struct spin_emu_route_t **spin_emu_routes;

struct spin_emu_route_packet
{
	int index;
	struct spin_emu_route_t entry;
};
	
struct spin_emu_mc_t
{
	uint key;
	uint payload;
	int has_payload;
};

#define SPIN_EMU_MC_QUEUE_SIZE 1024
struct spin_emu_mc_queue
{
	int pos;
	int len;
	struct spin_emu_mc_t queue[SPIN_EMU_MC_QUEUE_SIZE];
};

extern sdp_msg_t *spin_emu_ether_sdp_queue;
extern sdp_msg_t *spin_emu_sdp_queue;

extern void *spin_emu_dtcm_freeptr;

void spin_emu_dispatcher(int num, ...);

extern pthread_t spin_emu_controller_thread;
extern pthread_t spin_emu_worker_thread;

void bind_ethernet_channel();
void spin_emu_controller();
void spin_emu_send_rr_ping(int chip, int core);
void spin_emu_send_mc(int fd, struct spin_emu_mc_t *mc);
void spin_emu_send_route(int fd, uint entry, uint key, uint mark, uint route);
void spin_emu_die(const char *msg, ...);
void spin_emu_die_errno(const char *msg, ...);
#define die spin_emu_die
#define die_errno spin_emu_die_errno
void *xmalloc(size_t len);
void *xzalloc(size_t len);
int prefixcmp(const char *str, const char *prefix);

enum {
	SPIN_EMU_PACKET_MC = 0x42,
	SPIN_EMU_PACKET_SDP,
	SPIN_EMU_PACKET_ROUTE,
	SPIN_EMU_PACKET_STARTDATA,
	SPIN_EMU_PACKET_RR_PING,
	SPIN_EMU_PACKET_RR_PONG,
	SPIN_EMU_PACKET_FINISHED,
	SPIN_EMU_PACKET_WRITE,
	SPIN_EMU_PACKET_READ,
	SPIN_EMU_PACKET_VER,
	SPIN_EMU_PACKET_AS
};

#define TMPDIR_LEN 256

struct startdata_packet
{
	int chip;
	int core;
	char tmpdir[TMPDIR_LEN];
};


static inline int chip_id(int x, int y)
{
	assert(0 <= x && x < NUM_CHIPS_X); 
	assert(0 <= y && y < NUM_CHIPS_Y);
	return (x*NUM_CHIPS_Y) + y;
	// CP modified this function to go beyond 2x2
}

static inline int chip_my_x(int id)
{
	assert(0 <= id && id < NUM_CHIPS);
	return id/NUM_CHIPS_Y;
	// CP modified this function to go beyond 2x2 and to unambiguously define function
}

static inline int chip_my_y(int id)
{
	assert(0 <= id && id < NUM_CHIPS);
	return id%NUM_CHIPS_Y;
	// CP modified this function to go beyond 2x2 and to unambiguously define function	
}

extern int debug_mc_dispatch;
extern int debug_sdp_dispatch;
extern int debug_events;
extern int debug_startup;
extern int debug_exec;

extern int round_robin_scheduling;

extern void spin_emu_spawn_cores();
extern void spin_emu_setup_channels();

extern void spin_emu_mmap_core_mem(int chip, int core);
extern void spin_emu_munmap_core_mem();
extern void spin_emu_msync_core_mem();


#endif /* _SPIN_EMU_H_ */
