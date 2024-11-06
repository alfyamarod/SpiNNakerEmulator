#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>


#include "spin_emu.h"
#include "scamp_emu.h"

static inline int max(int a, int b)
{
	return a > b ? a : b;
}

const size_t recvbufsize = 4096;
long sendersip;
uint sendersport;
ushort senderseq;

int NUM_CHIPS_X;
int NUM_CHIPS_Y;
int NUM_CHIPS;
int **spin_emu_socket_dispatch;
int **spin_emu_socket_core;
int **spin_emu_running;
int **channel_targets;

int spin_emu_running_count=0;

struct spin_emu_mc_queue **mc_queue;
struct spin_emu_route_t **spin_emu_routes;
sdp_msg_t ***sdp_queue;
sdp_msg_t ***sdp_queue_tail;

char spin_emu_tmpdir_template[] = "/tmp/spin_emu.XXXXXX";

static int mc_pending;
int initialised = 0;
uint floodfill; // memory address where floodfill starts putting data (Important: this means only one flood fill at a time)

iptag_t iptags[TAG_TABLE_SIZE];
iptag_t *tag_table = iptags;
uint tag_tto = 9;	// 2.56s = 10ms * (1 << (9-1))

static void queue_mc(struct spin_emu_mc_queue *q,
		     uint key, uint payload, int has_payload)
{
	int idx;
	if (q->len >= SPIN_EMU_MC_QUEUE_SIZE)
		return;
	idx = (q->pos + q->len) % SPIN_EMU_MC_QUEUE_SIZE;

	q->queue[idx].key = key;
	q->queue[idx].payload = payload;
	q->queue[idx].has_payload = has_payload;

	q->len++;
	mc_pending = 1;
}

static void route_mc(int chip, struct spin_emu_mc_t mc)
{
	int i,j;
	if (debug_mc_dispatch)
	{
		fprintf(stderr, "DISPATCHER sending MC packet on behalf of [%d] %8x %8x\n", chip, mc.key, mc.payload);
	}
	for (i = 0; i < NUM_ROUTES; i++)
	{

		struct spin_emu_route_t *r = &spin_emu_routes[chip][i];

		/**
		 * A little extra debugging for routing (uncommenting this will produce a lot of output)

		struct spin_emu_route_t *a = &spin_emu_routes[0][i];
		struct spin_emu_route_t *b = &spin_emu_routes[1][i];
		struct spin_emu_route_t *c = &spin_emu_routes[2][i];
		struct spin_emu_route_t *d = &spin_emu_routes[3][i];

		fprintf(stderr, "%d) %8x:%8x:%8x", i, a->key, a->mask, a->route);
		fprintf(stderr, "\t%8x:%8x:%8x", b->key, b->mask, b->route);
		fprintf(stderr, "\t%8x:%8x:%8x", c->key, c->mask, c->route);
		fprintf(stderr, "\t%8x:%8x:%8x\n", d->key, d->mask, d->route);

		*/

		if ((r->key) != (mc.key & r->mask))
		{
			continue;	// no match for this entry
		}

		if (debug_mc_dispatch)
			fprintf(stderr, "DISPATCHER [%d] routing mc %8x %8x %8x route=%8x\n", chip, mc.key, r->mask, (mc.key & r->mask),r->route);


		for (j = 0; j < NUM_LINKS; j++)
		{
			if (!(r->route & (1<<j)))
				continue;
			int t = channel_targets[chip][j];
			if (t < 0)
				continue;
			if (debug_mc_dispatch)
				fprintf(stderr, "\tover link %d to chip %d (r->route=%08x)\n", j, t,r->route);

			queue_mc(&mc_queue[t][0], mc.key,
				 mc.payload, mc.has_payload);
		}
		for (j = 0; j < NUM_CPUS; j++) {
			if (!(r->route & (1<<(j+NUM_LINKS))))
				continue;

			if (debug_mc_dispatch)
			{
				fprintf(stderr, "\tinternally to core %d\n", j);
			}
			spin_emu_send_mc(spin_emu_socket_dispatch[chip][j], &mc);
		}
		return;
	}
}

static void route_one_mc_queue(int chip, struct spin_emu_mc_queue *q)
{
	while (q->len) {
		if (debug_mc_dispatch > 1)
			fprintf(stderr, "DISPATCHER [%d] flushing mc %d\n", chip, q->len);
		route_mc(chip, q->queue[q->pos % SPIN_EMU_MC_QUEUE_SIZE]);
		q->pos++;
		q->len--;
	}
}

static void flush_one_mc_queue(int chip, int core, int fd)
{
	struct spin_emu_mc_queue *q = &mc_queue[chip][core];
	while (q->len)
	{
		struct spin_emu_mc_t *mc = &q->queue[q->pos % SPIN_EMU_MC_QUEUE_SIZE];
		spin_emu_send_mc(fd, mc);
		if (debug_mc_dispatch)
			fprintf(stderr, "[%d,%d] sent MC packet (%d) %x %x\n", chip, core,
				q->len, mc->key, mc->payload);
		q->pos++;
		q->len--;
	}
}

static void flush_mc_queues()
{
	while (mc_pending) {
		int c, i;
		mc_pending = 0;
		for (c = 0; c < NUM_CHIPS; c++)
		{
			route_one_mc_queue(c, &mc_queue[c][0]);
		}
		for (c = 0; c < NUM_CHIPS; c++)
		{
			for (i = 1; i < NUM_CPUS; i++)
			{
				if (debug_mc_dispatch > 2)
				{
					fprintf(stderr, "Core (%d:%d) trying to send MC packets.\n", c, i);
				}
				flush_one_mc_queue(c, i, spin_emu_socket_dispatch[c][i]);
			}
		}
	}
}




static void queue_sdp(sdp_msg_t *msg)
{
	uint x, y, c, id = 0;
	x = msg->dest_addr >> 8;
	y = msg->dest_addr & 0xff;
	c = msg->dest_port & 0x1f;
	id = chip_id(x,y);
	if (debug_sdp_dispatch)
	{
		fprintf(stderr, "Sending SDP to chip %d core %d\n", id, c);
	}

	msg->next = NULL;
	if (sdp_queue_tail[id][c]) {
		sdp_queue_tail[id][c]->next = msg;
		sdp_queue_tail[id][c] = msg;
	} else {
		sdp_queue_tail[id][c] = msg;
		sdp_queue[id][c] = msg;
	}
}

static void queue_sdp_xy(sdp_msg_t *msg, int id, int c)
{
	if (debug_sdp_dispatch)
		fprintf(stderr, "Sending SDP to chip %d core %d\n", id, c);
	msg->next = NULL;
	if (sdp_queue_tail[id][c]) {
		sdp_queue_tail[id][c]->next = msg;
		sdp_queue_tail[id][c] = msg;
	} else {
		sdp_queue_tail[id][c] = msg;
		sdp_queue[id][c] = msg;
	}
}

static void flush_sdp_queues()
{
	int c, i;
	for (c = 0; c < NUM_CHIPS; c++)
	{
		for (i = 0; i < NUM_CPUS; i++)
		{
			sdp_msg_t *msg = sdp_queue[c][i];
			while (msg)
			{
				sdp_msg_t *prev;
				if (debug_sdp_dispatch > 1)
				{
					fprintf(stderr, "[%d,%d] sdp flush %p\n", c, i, msg);
				}
				char buf[1+sizeof(sdp_msg_t)];
				buf[0] = SPIN_EMU_PACKET_SDP;
				memcpy(buf+1, msg, sizeof(sdp_msg_t));
				if (send(spin_emu_socket_dispatch[c][i], buf, 1+sizeof(sdp_msg_t), MSG_DONTWAIT) < 0)
					die_errno("send() failed in sdp flush\n");
				prev = msg;
				msg = msg->next;
				free(prev);
			}
			sdp_queue[c][i] = NULL;
			sdp_queue_tail[c][i] = NULL;
		}
	}
}

static void dispatch_mc(int chip, int core, char *buf, size_t len)
{
	struct spin_emu_mc_queue *q = &mc_queue[chip][core];

	assert(len == 4 || len == 8);

	if (debug_mc_dispatch)
		fprintf(stderr, "DISPATCHER dispatching mc %8x %8x to chip=%d\n", *(uint*)buf, len==8 ? *(uint*)(buf+4) : 0,chip);

	if (len == 4)
		queue_mc(q, *(uint*)buf, 0, 0);
	else
		queue_mc(q, *(uint*)buf, *(uint*)(buf+4), 1);
}


static void dispatch_sdp_to_ether(sdp_msg_t *msg)
{

	assert(msg->length <= sizeof(sdp_msg_t) - ((char*)&msg->flags-(char*)&msg->length));

	if (debug_sdp_dispatch)
	{
		fprintf(stderr, "dispatching sdp to ether [%02x] %04x:%02x -> %04x,%02x (tag %d)\n",
					msg->flags,
					msg->srce_addr, msg->srce_port,
					msg->dest_addr, msg->dest_port,
					msg->tag);

		fprintf(stderr, "arg1 = %08x, arg2 = %08x, arg3 = %08x, length = %d, data = %16X\n",
							msg->arg1, msg->arg2, msg->arg3, msg->length, *(msg->data));

	}

	/**
	 * TODO: spike_receiver.py needs packet lengths to be multiples of 4 bytes.
	 * but app_monitoring.c can sometimes create packet lengths which are not
	 * multiples of 2 bytes.
	 *
	 * MO didn't have time to look into this so added this very quick fix.
     * CP checked this.  It is not required so has been removed.
	//
	if(msg->length % 4 != 0)
	{
		if (debug_sdp_dispatch)
		{
			fprintf(stderr, "Making sure packet size is a multiple of 4 bytes\n");
		}
		msg->length = msg->length - (msg->length % 4);
	}
	 */
    

	char buf[msg->length+2];
	memset(&buf[0], 0, sizeof(buf));
			buf[0] = 1;
			buf[1] = 0;

	struct sockaddr_in receiver_addr;
	receiver_addr.sin_family = AF_INET;

	if (msg->tag < 0 || msg->tag >= TAG_TABLE_SIZE)
	{
		// If tag is out of range we're probably replying to a smemb from ybug (or something else which sets the tag to 255)
		msg->seq = senderseq;
		receiver_addr.sin_port = sendersport;
		inet_pton(AF_INET, inet_ntoa(*(struct in_addr *)&sendersip), &receiver_addr.sin_addr);
	}
	else
	{
		// TODO: rather than converting the iptag structure each time, it might be faster to use sockaddr?
		receiver_addr.sin_port = htons(iptags[msg->tag].port);
		inet_pton(AF_INET, inet_ntoa(*(struct in_addr *)&iptags[msg->tag].ip), &receiver_addr.sin_addr);
	}

	memcpy(buf+2, &msg->flags, msg->length);


	if (sendto(spin_emu_ether_fd, buf, msg->length+2, MSG_DONTWAIT, (struct sockaddr*)&receiver_addr, sizeof(receiver_addr)) < 0)
	{
		fprintf(stderr, "warning: sendto() on ether (tag %d) failed: %s\n",	msg->tag, strerror(errno));
	}

	free(msg);

}


static void dispatch_sdp_one(sdp_msg_t *msg)
{
	int x, y, c, id;


	if (debug_sdp_dispatch)
	{
		fprintf(stderr, "dispatching sdp [%02x] %04x:%02x -> %04x,%02x (tag %d)\n",
			msg->flags,
			msg->srce_addr, msg->srce_port,
			msg->dest_addr, msg->dest_port,
			msg->tag);
		fprintf(stderr, "\tcmd_rc %4x, seq %4x, args %x %x %x\n",
			msg->cmd_rc, msg->seq, msg->arg1, msg->arg2, msg->arg3);
		fprintf(stderr, "\tlen %4x\n",
					msg->length);
	}

	if (msg->dest_port == PORT_ETH)
	{
		dispatch_sdp_to_ether(msg);
		return;
	}

	x = msg->dest_addr >> 8;
	y = msg->dest_addr & 0xff;
	c = msg->dest_port & 0x1f;
	id = chip_id(x,y);
	if (debug_sdp_dispatch)
	{
		fprintf(stderr, "\tto chip %d core %d\n", id, c);
	}
	msg->next = NULL;
	if (sdp_queue_tail[id][c])
	{
		sdp_queue_tail[id][c]->next = msg;
		sdp_queue_tail[id][c] = msg;
	}
	else
	{
		sdp_queue_tail[id][c] = msg;
		sdp_queue[id][c] = msg;
	}
}


/**
 * IP tag functions
 */
void copy_ip (const uchar *f, uchar *t)
{
  ushort *ts = (ushort*) t;
  ushort *fs = (ushort*) f;

  ts[0] = fs[0];
  ts[1] = fs[1];
}

void iptag_init ()
{
	iptag_t *tag = tag_table;
	uint i;
	for (i = 0; i < TAG_TABLE_SIZE; i++)
    {
		tag->flags = 0;
		tag++;
    }

	// Added a default tubotron tag for the hello world tutorial
	// MO - I've set this to tag 1 (seems to be a sensible choice based on what i've seen)
	uint taginit = 1;
	iptag_t *tt = tag_table + taginit;
	uint timeout = 100;
	tt->timeout = timeout;
	tt->port = 17892;
	tt->ip[0] = 127;
	tt->ip[1] = 0;
	tt->ip[2] = 0;
	tt->ip[3] = 1;

}

uint iptag_new ()
{
	uint i;
	for (i = FIRST_POOL_TAG; i <= LAST_POOL_TAG; i++)
	{
		if (tag_table[i].flags == 0)
		{
			return i;
		}
	}
	return TAG_NONE;
}


static int cmd_iptag(sdp_msg_t *msg)
{
	uint op = msg->arg1 >> 16;
	uint tag = msg->arg1 & 255;

	if (op > IPTAG_MAX || tag >= TAG_TABLE_SIZE)
	{
		msg->cmd_rc = RC_ARG;
		return 0;
	}

	if (op == IPTAG_NEW || op == IPTAG_SET)
	{
		if (op == IPTAG_NEW)
		{
			tag = iptag_new ();
		}
		if (tag != TAG_NONE)
		{
			iptag_t *tt = tag_table + tag;
			uint timeout = (msg->arg2 >> 16) & 15;
			if (timeout != 0)
			{
				timeout = 1 << (timeout - 1);
			}
			tt->timeout = timeout;
			tt->port = msg->arg2 & 0xffff;
			uchar *ip_addr = (uchar *) &msg->arg3;
			copy_ip (ip_addr, tt->ip);
		}
		msg->arg1 = tag;
		return 4;
	}
	else if (op == IPTAG_GET)
	{
		iptag_t *tt = tag_table + tag;
		uint size = msg->arg2 * sizeof (iptag_t);
		if (size > BUF_SIZE)
		{
			msg->cmd_rc = RC_ARG;
			return 0;
		}
		memcpy ((uchar *) &msg->arg1, tt, size);
		return size;
	}
	else if (op == IPTAG_TTO)
	{
		msg->arg1 = (TAG_FIXED_SIZE << 24) + (TAG_POOL_SIZE << 16) + tag_tto;
		if (msg->arg2 < 16)
		{
			tag_tto = msg->arg2;
		}
		return 4;
	}
	else // IPTAG_CLR
	{
		tag_table[tag].flags = 0;
		return 0;
	}

}

/**
 *  Emulates the p2pc command from ybug.
 *  This command can only be used once as it basically tells the emulator how many CPUs to emulate
 */
void cmd_p2pc(int x, int y) {

	if(initialised == 0)
	{
		// Initialise first because the process can take a while
		initialised = 1;

		NUM_CHIPS_X = x;
		NUM_CHIPS_Y = y;
		NUM_CHIPS = (NUM_CHIPS_X*NUM_CHIPS_Y);

		// MALLOC memory for spin_emu_socket_dispatch and spin_emu_socket_core arrays
		// TODO: Only works on square arrays atm
		// TODO: Maybe some of these data structures can be reused or combined if space becomes an issue?
		// TODO: Use spin_emu_running to help reset the emulator
		spin_emu_socket_dispatch = malloc ( NUM_CHIPS * sizeof *spin_emu_socket_dispatch );
		spin_emu_socket_core = malloc ( NUM_CHIPS * sizeof *spin_emu_socket_core);
		spin_emu_routes = malloc ( NUM_CHIPS * sizeof *spin_emu_routes);
		channel_targets = malloc ( NUM_CHIPS * sizeof *channel_targets);
		mc_queue = malloc ( NUM_CHIPS * sizeof *mc_queue);
		spin_emu_running = malloc ( NUM_CHIPS * sizeof *spin_emu_running);
		sdp_queue = malloc ( NUM_CHIPS * sizeof *sdp_queue);
		sdp_queue_tail = malloc ( NUM_CHIPS * sizeof *sdp_queue_tail);
		spin_emu_child_pid = malloc ( NUM_CHIPS * sizeof *spin_emu_child_pid);

		int i;
		for (i = 0; i < NUM_CHIPS; i++ )
		{
			spin_emu_socket_dispatch[i] = malloc (NUM_CPUS * sizeof *spin_emu_socket_dispatch[i]);
			spin_emu_socket_core[i] = malloc (NUM_CPUS * sizeof *spin_emu_socket_core[i]);
			spin_emu_routes[i] = malloc (NUM_ROUTES * sizeof *spin_emu_routes[i]); // NOTE: NUM_ROUTES not NUM_CPU
			channel_targets[i] = malloc (NUM_LINKS * sizeof *channel_targets[i]); // NOTE: NUM_LINKS not NUM_CPU
			mc_queue[i] = malloc (NUM_CPUS * sizeof *mc_queue[i]);
			spin_emu_running[i] = malloc (NUM_CPUS * sizeof *spin_emu_running[i]);
			sdp_queue[i] = malloc (NUM_CPUS * sizeof *sdp_queue[i]);
			sdp_queue_tail[i] = malloc (NUM_CPUS * sizeof *sdp_queue_tail[i]);
			spin_emu_child_pid[i] = malloc (NUM_CPUS * sizeof *spin_emu_child_pid[i]);
		}
		// TODO: Free (or reuse) this memory when some sort of quit or restart command is received


		if(debug_startup)
			fprintf(stderr, "Created a %d by %d 2D board\n", x, y);

		// Set up channels to talk to cores
		spin_emu_tmpdir = mkdtemp(spin_emu_tmpdir_template);
		if (!spin_emu_tmpdir)
			die_errno("mkdtemp failed");
		if (strlen(spin_emu_tmpdir) > TMPDIR_LEN-1)
			die("I cannot handle a tmpdir %d chars long", strlen(spin_emu_tmpdir));
		spin_emu_setup_channels();

		// CP - add initialisation for routing tables 26th Oct 2012
		int perchip, perroute;
		for (perchip=0;perchip<NUM_CHIPS;perchip++)
		{
			for (perroute=0;perroute<NUM_ROUTES;perroute++)
			{
				spin_emu_routes[perchip][perroute].key=0xFFFFFFFF;
				spin_emu_routes[perchip][perroute].mask=0;
				spin_emu_routes[perchip][perroute].route=0;
			}
		}

		// CP - initialise the routing tables for the size of the system
		// CP - link directions, 0=East, 1=NE, 2=N, 3=W, 4=SW, 5=S.
		int perlink, targetx, targety;
		for (perchip=0;perchip<NUM_CHIPS;perchip++)
		{
			for (perlink=0;perlink<NUM_LINKS;perlink++)
			{
				targetx=chip_my_x(perchip); // based on this chip's current x coordinate
				targety=chip_my_y(perchip); // based on this chip's current y coordinate
				if (perlink>=0 && perlink<=1) targetx=chip_my_x(perchip)+1; // E/NE means we increment x coord
				if (perlink>=1 && perlink<=2) targety=chip_my_y(perchip)+1; // NE/N means we increment y coord
				if (perlink>=3 && perlink<=4) targetx=chip_my_x(perchip)-1; // W/SW means we decrement x coord
				if (perlink>=4 && perlink<=5) targety=chip_my_y(perchip)-1; // SW/S means we decrement y coord

				if (targetx<0 || targetx>=NUM_CHIPS_X || targety<0 || targety>=NUM_CHIPS_Y)
					channel_targets[perchip][perlink]=-1;  // target is off the edge of the chip grid (no wraparound cnnx available)
				else
				{
					channel_targets[perchip][perlink]=chip_id(targetx, targety);  // work out the internal chipid for the chip on this link
				}

				if(debug_startup)
				{
					printf("%d) L%d= %d.  ",perchip,perlink,channel_targets[perchip][perlink]);
					if (perlink==5) printf("\n");
				}
			}
		}

		spin_emu_spawn_cores();

	}
}

/**
 *  Starts application for CPUs in the mask and destination
 */
void dispatch_as(sdp_msg_t *msg) {

	uint mask = (int) msg->arg2;
	int c,i,x,y,id;

	x = msg->dest_addr >> 8;
	y = msg->dest_addr & 0xff;


	printf("chip reading %d, %d\n", x, y);

	id = chip_id(x,y);

	for (c = 0; c < NUM_CHIPS; c++) {
		// only want to dispatch it to the CPU given in the message
		if(c == id) {
			for (i = 0; i < NUM_CPUS; i++) {
				if (mask & (1<<i)) {
					// send command to cores specified by the mask
					msg->cmd_rc = SPIN_EMU_PACKET_AS;
					sdp_msg_t *msgf = xzalloc(sizeof(sdp_msg_t));
					memcpy(msgf, msg, sizeof(sdp_msg_t));
					queue_sdp_xy(msgf, c, i);
					//printf("Setting chip %d core %d running.\n",c,i);
					spin_emu_running[c][i] = 1;
					spin_emu_running_count++;
				}
			}
		}
	}
}

/**
 *  Starts application for all CPUs with the mask (used at the end of floodfill)
 */
void flood_as(sdp_msg_t *msg) {

	uint mask = (int) msg->arg2;
	int c,i;
	for (c = 0; c < NUM_CHIPS; c++) {
		for (i = 0; i < NUM_CPUS; i++) {
			if (mask & (1<<i)) {
				// send command to cores specified by the mask
				msg->cmd_rc = SPIN_EMU_PACKET_AS;
				sdp_msg_t *msgf = xzalloc(sizeof(sdp_msg_t));
				memcpy(msgf, msg, sizeof(sdp_msg_t));
				msgf->arg1 = floodfill; // put the starting location in
				queue_sdp_xy(msgf, c, i);

				spin_emu_running[c][i] = 1;
				spin_emu_running_count++;


			}
		}
	}
}

/**
 * Floodfill
 * Some changes needed to the floodfill packets so that a single core does the write:
 * 	1) changing packet to a basic write packet to the location in msg->arg3
 * 	2) Also change packet to specify write in bytes
 */
void cmd_floodfill(sdp_msg_t *msg) {

	msg->cmd_rc = SPIN_EMU_PACKET_WRITE;
	msg->arg1 = msg->arg3; // sys ram + (seq * 256)
	uint bytes = ((((msg->arg2 >> 8) & 255) + 1)*4);
	msg->arg2 = bytes;
	msg->arg3 = 0; // floods words but we write bytes

	int c;
	// Floods message to all chips because I can't find if/where the target chip is specified in the SDP packet (MO)
	// Important! because we're assuming a floodfill writes to shared memory,
	// this method just sends a write message to one core on each chip (doesn't matter which)
	for (c = 0; c < NUM_CHIPS; c++) {
		sdp_msg_t *msgf = xzalloc(sizeof(sdp_msg_t));
		memcpy(msgf, msg, sizeof(sdp_msg_t));
		queue_sdp_xy(msgf, c, 1);
	}
}


/**
 * Takes the message with a command and runs the relevant methods
 */
static void process_message(sdp_msg_t *msg)
{
	if (debug_sdp_dispatch)
	{
		fprintf(stderr, "SDP received [%02x] %04x:%02x -> %04x,%02x (tag %d)\n",
			msg->flags,
			msg->srce_addr, msg->srce_port,
			msg->dest_addr, msg->dest_port,
			msg->tag);
		fprintf(stderr, "\tcmd_rc %4x, seq %4x, args %x %x %x\n%s\n",
			msg->cmd_rc, msg->seq, msg->arg1, msg->arg2, msg->arg3,msg->data);
	}

	// get message command
	ushort type = msg->cmd_rc;
	printf("CMD rc %hu\n", type);
	// variables to be used by the different commands, e.g. p2pc x y
	uint x, y, cmd = 0;


	// TODO: Lots of message types are not yet supported
	// See the wiki on how to add new packet types
	switch (type)
	{
		case CMD_VER:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received sver command\n");
			msg->cmd_rc = SPIN_EMU_PACKET_VER;
			queue_sdp(msg);
			break;
		/*
		case CMD_RUN:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Low level run command not supported in the emulator.\n");
			break;
		*/
		case CMD_READ:
			// cores read and reply with their own memory data
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received READ command\n");
			msg->cmd_rc = SPIN_EMU_PACKET_READ;
			queue_sdp(msg);
			break;
		case CMD_WRITE:
			// cores write to their own memory
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received WRITE command\n");
			msg->cmd_rc = SPIN_EMU_PACKET_WRITE;
			//fprintf(stderr, "Sending %x bytes\n",msg->arg2);
			queue_sdp(msg);
			break;
			/*
		case CMD_APLX:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Emulator does not run APLX files, use AS command instead (after using sload or similar to load an executable binary).\n");
			break;
		case CMD_AP_MAX:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received AP_MAX command\n");
			break;
		case CMD_LINK_PROBE:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received LINK PROBE command\n");
			break;
		case CMD_LINK_READ:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received LINK READ command\n");
			break;
		case CMD_LINK_WRITE:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received LINK WRITE command\n");
			break;
		case CMD_xxx_19:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received XXX 19 command\n");
			break;
		*/
		case CMD_NNP:
			cmd = msg->arg1 >> 24;
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received NNP command: %d\n", cmd);
			// NN_CMD_FFS (6) means a flood fill is just about to start
			if(cmd == 6)
			{
				if (debug_sdp_dispatch > 1)
					fprintf(stderr, "Received NNP command: Flood fill start\n");
				// Just about to receive a bunch of flood fill packets... starting location will is stored in floodfill
				floodfill = msg->arg2;

			}
			// NN_CMD_FFE (15) signifies the end of a flood fill
			else if(cmd == 15) {
				if (debug_sdp_dispatch > 1)
					fprintf(stderr, "Received NNP command: Flood fill end\n");
				// All the flood fill packets have been received execute data which is stored in the floodfill variable
				flood_as(msg);
			}
			break;
		case CMD_P2PC:
			x = ((htonl(msg->arg2)) & 0x000000ff);
			y = ((htonl(msg->arg2) >> 8) & 0x000000ff);
			if (debug_sdp_dispatch > 1) {
				fprintf(stderr, "Received P2PC command x=%d y=%d\n", x, y);
			}
			// If not initialised, creates the cores, memory maps needed, and the sockets
			if(initialised == 0) {
				cmd_p2pc(x, y);
				printf("*** P2PC complete and cores now active ***\n");
			}
			break;
			
			/*
		case CMD_PING:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received ping command\n");
			break;
			*/
		case CMD_FFD:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received flood fill command\n");
			cmd_floodfill(msg);
			break;
		case CMD_AS:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received AS command\n");
			dispatch_as(msg); // each core process takes care of its own exec prep
			break;
			/*
		case CMD_LED:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received LED command\n");
			break;
			*/
		case CMD_IPTAG:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received IPTAG command\n");
			uint len2 = cmd_iptag(msg);
			msg->cmd_rc = 128;
			msg->length = len2+28;
			dispatch_sdp_to_ether(msg);
			break;
			/*
		case CMD_SROM:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received SROM command\n");
			break;
		case CMD_TUBE:
			if (debug_sdp_dispatch > 1)
				fprintf(stderr, "Received TUBE command\n");
			break;
			*/
		default:
			if (debug_sdp_dispatch > 1)
			{
				fprintf(stderr, "warning: unknown command '%#x' passing to core\n", type);
			}
			// Default behaviour is to multi-hop packets to the cores
			queue_sdp(msg);
			break;
	}
}

static void dispatch_sdp(int chip, int core, char *buf, size_t len)
{
	sdp_msg_t *msg = calloc(1,sizeof(sdp_msg_t));

	assert(len <= sizeof(sdp_msg_t));
	memcpy(msg, buf, len);
	dispatch_sdp_one(msg);

}

/**
 * The dispatcher also acts as the router for each chip
 */
static void store_route(int chip, int core, char *buf, size_t len)
{
	assert(len == sizeof(struct spin_emu_route_packet));
	struct spin_emu_route_packet *p = (struct spin_emu_route_packet *) buf;

	//fprintf(stderr, "Chip %d received key=%08x, route=%08x\n", chip,p->entry.key,p->entry.route);

	assert(p->index < NUM_ROUTES);
	memcpy(&spin_emu_routes[chip][p->index], &p->entry,
	       sizeof(struct spin_emu_route_t));

	if (debug_mc_dispatch)
		fprintf(stderr, "[%d] route %d = %x/%x -> %x\n",
			chip, p->index, p->entry.key, p->entry.mask, p->entry.route);
}




static void do_dispatch(int chip, int core, int fd)
{
	char buf[recvbufsize];
	ssize_t len;

	while ((len = recv(fd, buf, recvbufsize, MSG_DONTWAIT)) > 0)
	{
		char type = buf[0];
		switch (type)
		{
			case SPIN_EMU_PACKET_MC:
				dispatch_mc(chip, 0, buf+1, len-1);
				break;
			case SPIN_EMU_PACKET_SDP:
				dispatch_sdp(chip, core, buf+1, len-1);
				break;
			case SPIN_EMU_PACKET_ROUTE:
				store_route(chip, core, buf+1, len-1);
				break;
			case SPIN_EMU_PACKET_FINISHED:
				// TODO: Core finished, we can use this info to reset the emulator
				spin_emu_running[chip][core] = 0;
				spin_emu_running_count--;
				break;
			default:
				die("invalid packet type %#x", type);
				break;
		}
	}

	if (len == 0)
		die_errno("recv() returned empty packet");
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return;
	die_errno("recv() from internal socket");
}

/**
 * Received packets from the ethernet
 */
static void receive_msg_from_ether()
{
	char buf[recvbufsize];
	ssize_t len;
	struct sockaddr_storage sender;
	socklen_t sendsize = sizeof(sender);
	bzero(&sender, sizeof(sender));
	while ((len = recvfrom(spin_emu_ether_fd, buf, recvbufsize, MSG_DONTWAIT, (struct sockaddr*)&sender, &sendsize)) > 0)
	{
		sdp_msg_t *msg = xzalloc(sizeof(sdp_msg_t));


		if (debug_sdp_dispatch)
		{
		    fprintf(stderr, "len %d\n", len);
			int i;
			fprintf(stderr, "got ethernet sdp:");
			for (i = 0; i < len; i++)
				fprintf(stderr, " %2x", (int) (unsigned char) buf[i]);
			putc('\n', stderr);
		}

		
		memcpy(&msg->flags, buf+2, len-2);

		printf("msg->flags %d \n", msg->flags);
		msg->length = len-2;


		/**
		 * The Dispatcher replies before processing some messages.
		 * And stores message details to help reply later in other cases.
		 *
		 * TODO: 	ybug can send the same message 3 times if it doesn't receive a reply, and we only want to process each
		 * 			message once. I was going to ensure this with sequence numbers but the sequence coming from the visualiser
		 * 			and ybug are different, so some other solution is needed for the heat_demo application.
		 *
		 * Dispatcher responds immediately with RC_OK for all messages but READ, IPTAG, and VER
		 */
		if((msg->cmd_rc != CMD_VER) & (msg->cmd_rc != CMD_READ) & (msg->cmd_rc != CMD_IPTAG))
		{
			buf[10] = 0x80;
			if (sendto(spin_emu_ether_fd, buf, len+2, MSG_DONTWAIT, (struct sockaddr *)&sender, sendsize) < 0)
			{
				die_errno("cannot send ybug response packet");
			}
			if (debug_sdp_dispatch > 1)
			{
				fprintf(stderr, "Ybug message sent message back\n");
			}
		}
		else
		{
			struct sockaddr_in *sin = (struct sockaddr_in *)&sender;
			sendersip = sin->sin_addr.s_addr;
			sendersport = sin->sin_port;
			senderseq = msg->seq;
		}



		// Now dispatcher can begin processing the message more fully
		process_message(msg);

	}

	if (len == 0)
		die_errno("recv() returned empty packet");
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return;
	die_errno("recv() from ethernet socket");

}


/*
 * spin_emu_dispatcher() listens to ybug command packets and routes packets
 * between core processes
 */
void spin_emu_dispatcher(int num, ...)
{
	bind_ethernet_channel();

	// set up IP tags
	iptag_init ();

	printf("**********************************************************\n***         Welcome to the SpiNNaker emulator          ***\n");

	// check if we're supposed to set up cores
	if(num == 2)
	{
		va_list arguments;
		va_start ( arguments, num );

		int x = va_arg ( arguments, int );
		int y = va_arg ( arguments, int );
		va_end ( arguments );

		cmd_p2pc(x,y);
		printf("***       An emulated %d by %d board is now active       ***\n",x,y);

	}
	else
	{
		printf("*** Waiting on a P2PC packet before starting emulation ***\n");
	}

	printf("**********************************************************\n");

	while (1)
	{
		fd_set fds;
		int c, i;

        FD_ZERO(&fds);
		FD_SET(spin_emu_ether_fd, &fds);

		if (FD_ISSET(spin_emu_ether_fd, &fds))
		{
			receive_msg_from_ether();
		}

		if(initialised == 1)
		{
			// Cores have been created, dispatch messages for them.
			for (c = 0; c < NUM_CHIPS; c++)
			{
				for (i = 0; i < NUM_CPUS; i++)
				{
					int fd = spin_emu_socket_dispatch[c][i];
					FD_SET(fd, &fds);
				}
			}
			for (c = 0; c < NUM_CHIPS; c++)
			{
				for (i = 0; i < NUM_CPUS; i++)
				{
					int fd = spin_emu_socket_dispatch[c][i];
					if (FD_ISSET(fd, &fds))
					{
						do_dispatch(c, i, fd);
					}
				}
			}


			// send out queued packets over sockets
			flush_mc_queues();
			flush_sdp_queues();


		}
	}
}
