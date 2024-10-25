#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "spin_emu.h"
//#include "spin1_api_params.h"
#include "scamp_emu.h"
#include "dma.h"

// ------------------------------------------------------------------------
// helpful macros
// ------------------------------------------------------------------------
//! \brief The address of a chip
//! \param[in] x: X coordinate of the chip
//! \param[in] y: Y coordinate of the chip
//! \return The packed chip coordinates
#define CHIP_ADDR(x, y)      ((x << 8) | y)
//! \brief A peer-to-peer route
//! \param[in] addr: The address of the chip
//! \return The route bit to that chip
#define P2P_ROUTE(addr)      (1 << p2p_get(addr))
//! \brief The route to a core on the current chip
//! \param[in] core: The core to route to
//! \return The route bit to that core
#define CORE_ROUTE(core)     (1 << (core + NUM_LINKS))



uint spin1_send_mc_packet(uint key, uint data, uint load);

uint spin1_send_sdp_msg (sdp_msg_t *msg, uint timeout);

uint  spin1_get_core_id(void);

uint  spin1_get_chip_id(void);


struct qevent
{
	struct qevent *next;
	uint arg1;
	uint arg2;
    callback_t cback; // CP added so support different callbacks in an event type
};

struct
{
	int priority;     
	callback_t cback; 
	struct qevent *queue;
	struct qevent *queue_tail;
}evinfo[NUM_EVENTS];  


uint spin_emu_sim_time;
dma_pipeline_t dma_pipeline;

pthread_mutex_t evinfo_mutex = PTHREAD_MUTEX_INITIALIZER;




// ---------------------
/* simulation control */
// ---------------------
static uchar  rootAp;                 // root appl. core has special functions
static uchar  rootChip;               // root chip appl. core has special functions
static ushort rootAddr;               // root appl. core has special functions

static uint   my_chip = 0;            // chip address in core_map coordinates

static volatile uint run = 0;         // controls simulation start/stop
volatile uint ticks = 0;              // number of elapsed timer periods
static volatile uint timer_tick = 0;  // timer tick period
static volatile uint nchips = 1;      // chips in the simulation
static volatile uint ncores = 1;      // application cores in the simulation
static volatile uint my_ncores = 0;   // this chip's appl. cores in the sim.
volatile uint exit_val = NO_ERROR;    // simulation return value
// number of cores that have reached the synchronisation barrier
volatile uint barrier_rdy1_cnt;
volatile uint barrier_rdy2_cnt;
// synchronisation barrier go signals
volatile uint barrier_go = FALSE;
volatile uint barrier_rdygo = FALSE;
// default fiq handler -- restored after simulation
isr_t def_fiq_isr;
int fiq_event = -1;

// ---------------
/* dma transfer */
// ---------------
dma_queue_t dma_queue;
// ---------------

// -----------------------
/* scheduler/dispatcher */
// -----------------------
cback_t callback[NUM_EVENTS];
volatile uchar user_pending = FALSE;
uint user_arg0;
uint user_arg1;

// ---------------------
/* rts initialization */
// ---------------------
void rts_init (void);
// ---------------------



// ------------------------------------------------------------------------
// functions
// ------------------------------------------------------------------------


// ----------------------------
/* intercore synchronisation */
// ----------------------------
static void barrier_setup (uint chips, uint * core_map);
static void barrier_wait (void);


/* reproduce the chip lock*/
static uint lock_try (uint lock)
{
	char name[PATH_MAX];
	int len;

	len = strlen(spin_emu_tmpdir);
	memcpy(name, spin_emu_tmpdir, len);
	name[len] = '/';
	sprintf(name+len+1, "syslockID%dchip%d", lock, spin_emu_chip);
	int pid_file = open(name, O_CREAT | O_RDWR, 0666);
	int rc = flock(pid_file, LOCK_EX | LOCK_NB);
	/*
	if(rc)
	{
		if(EWOULDBLOCK == errno)
			fprintf(stderr, "[%d,%d] is failed\n",spin_emu_chip, spin_emu_core);
	}
	*/
	return !rc;
}



// ------------------------------------------------------------------------
//  general message handling
// ------------------------------------------------------------------------

static void queue_event(uint event_id, uint arg1, uint arg2, callback_t cbacktoq)
{


	pthread_mutex_lock(&evinfo_mutex);
	if (!evinfo[event_id].cback)
	{
		pthread_mutex_unlock(&evinfo_mutex);
		if (debug_events)
		{
			fprintf(stderr, "[%d,%d] queue_event: %d(%x,%x) unhandled\n",
				spin_emu_chip, spin_emu_core,
				event_id, arg1, arg2);
		}
		return;
	}

	if (evinfo[event_id].priority <= 0)
	{
		if (debug_events)
		{
			fprintf(stderr, "[%d,%d] queue_event: %d(%x,%x) called directly\n",
				spin_emu_chip, spin_emu_core,
				event_id, arg1, arg2);
		}
		// directly execute
		//callback_t cb = evinfo[event_id].cback;
        callback_t cb = cbacktoq;
		pthread_mutex_unlock(&evinfo_mutex);
		cb(arg1, arg2);

		if (debug_events)
		{
			fprintf(stderr, "[%d,%d] queue_event: %d(%x,%x) successfully called directly\n",
				spin_emu_chip, spin_emu_core,
				event_id, arg1, arg2);
		}

		return;
	}
	else
	{
		struct qevent *ev = xmalloc(sizeof(struct qevent));
		ev->arg1 = arg1;
		ev->arg2 = arg2;
        ev->cback = cbacktoq; 
		ev->next = NULL;
		if (evinfo[event_id].queue)
			evinfo[event_id].queue_tail->next = ev;
		else
			evinfo[event_id].queue = ev;
		evinfo[event_id].queue_tail = ev;
		pthread_mutex_unlock(&evinfo_mutex);
		if (debug_events)  // CPDEBUG invert this to print events being queued
		{
			fprintf(stderr, "[%d,%d] queue_event: %d(%x,%x) queued\n",
				spin_emu_chip, spin_emu_core,
				event_id, arg1, arg2);
		}
	}
}

static void read_mc(char *buf, ssize_t len)
{
	if (debug_mc_dispatch)
		fprintf(stderr, "[%d,%d] got mc %x,%x\n",
			spin_emu_chip, spin_emu_core,
			*(int*)buf, len == 8 ? *(int*)(buf+4) : 0);
	assert(len == 4 || len == 8);

	uint key = *(uint*)buf;

	switch (key)
	{
		case RDYGO_KEY:
			//fprintf(stderr, "@@@@) [%d,%d] Received RDYGO_KEY\n", spin_emu_chip, spin_emu_core);
			barrier_rdygo = TRUE;
			break;
		case GO_KEY:
			//fprintf(stderr, "[%d,%d] Received GO_KEY\n", spin_emu_chip, spin_emu_core);
			barrier_go = TRUE;
			break;
		case RDY1_KEY:
			//fprintf(stderr, "@@@@) [%d,%d] Received RDY1_KEY barrier_rdy2_cnt=%d\n", spin_emu_chip, spin_emu_core,barrier_rdy1_cnt);
			barrier_rdy1_cnt = barrier_rdy1_cnt+1;
			break;
		case RDY2_KEY:
			barrier_rdy2_cnt = barrier_rdy2_cnt+1;
			//fprintf(stderr, "[%d,%d] Received RDY2_KEY barrier_rdy2_cnt=%d\n", spin_emu_chip, spin_emu_core,barrier_rdy2_cnt);
			break;
		default:
			//fprintf(stderr, "[%d,%d] Received unknown MC packet\n", spin_emu_chip, spin_emu_core);
			if (len == 4)
				queue_event(MC_PACKET_RECEIVED, *(uint*)buf, 0, evinfo[MC_PACKET_RECEIVED].cback);
			else
				queue_event(MC_PACKET_RECEIVED, *(uint*)buf, *(uint*)(buf+4), evinfo[MC_PACKET_RECEIVED].cback);
			break;
	}
}

/**
 * Core reads SDP, calls special emulator commands that handle SDP messages
 * E.g. To write to memory locations (SPIN_EMU_PACKET_WRITE) and executing applications (SPIN_EMU_PACKET_AS)
 */
static void read_sdp(char *buf, ssize_t len)
{
	sdp_msg_t *msg = xmalloc(sizeof(sdp_msg_t));
	assert(len == sizeof(sdp_msg_t));
	memcpy(msg, buf, len);
	uint dport = msg->dest_port >> 5;
	ushort type = msg->cmd_rc;
	uint len2 =0;
	if(debug_sdp_dispatch)
	{
		fprintf(stderr, "[%d,%d] reading sdp packet seq=%d\n", spin_emu_chip, spin_emu_core,msg->seq);
	}
	switch (type) {
		case SPIN_EMU_PACKET_WRITE:
			if(debug_sdp_dispatch)
			{
				fprintf(stderr, "[%d,%d] writing packet data to memory location=%x\n", spin_emu_chip, spin_emu_core,msg->arg1);
			}
			// Cores dont respond to writes because there are a few ways to write and it would be too complicated
			cmd_write(msg);
			break;
		case SPIN_EMU_PACKET_READ:
			if(debug_sdp_dispatch)
			{
				fprintf(stderr, "[%d,%d] reading data from memory location=%x\n", spin_emu_chip, spin_emu_core,msg->arg1);
			}
			// respond with the new message (dispatcher will send this over the ethernet to whoever send the request)
			len2 = cmd_read(msg);
			msg->dest_port = msg->srce_port;
			msg->dest_addr = msg->srce_addr;
			msg->srce_port = spin1_get_core_id();
			msg->srce_addr = spin1_get_chip_id();
			msg->cmd_rc = 0x80;
			msg->length = len2+12;
			spin1_send_sdp_msg(msg, 1);
			break;
		case SPIN_EMU_PACKET_VER:
			if(debug_sdp_dispatch)
			{
				fprintf(stderr, "[%d,%d] returning version info seq=%d, tag=%d\n", spin_emu_chip, spin_emu_core,msg->seq,msg->tag);
			}
			// respond with the new message (dispatcher will send this over the ethernet to whoever send the request)
			len2 = cmd_ver(msg);
			msg->dest_port = msg->srce_port;
			msg->dest_addr = msg->srce_addr;
			msg->srce_port = spin1_get_core_id();
			msg->srce_addr = spin1_get_chip_id();
			msg->cmd_rc = 0x80;
			msg->length = len2+12;
			spin1_send_sdp_msg(msg, 1);
			break;
		case SPIN_EMU_PACKET_AS:
			// Dont need to respond to AS messages as the emulator does this
			cmd_as(msg);
			break;
		default:
			queue_event(SDP_PACKET_RX, (uint) msg, dport, evinfo[SDP_PACKET_RX].cback);
			break;
	}
}

// ------------------------------------------------------------------------
//  Receives messages sent to a core
// ------------------------------------------------------------------------
#define RECVBUFSIZE 1024
static void read_packets()
{
	char buf[RECVBUFSIZE];
	ssize_t len;
	while ((len = recv(SPIN_EMU_SOCK, buf, RECVBUFSIZE, MSG_DONTWAIT)) > 0)
	{
		char type = buf[0];
		switch (type)
		{
			case SPIN_EMU_PACKET_MC:
				read_mc(buf+1, len-1);
				break;
			case SPIN_EMU_PACKET_SDP:
				read_sdp(buf+1, len-1);
				break;
			default:
				die("invalid packet type %#x", type);
				break;
		}
	}

	if (errno != EAGAIN && errno != EWOULDBLOCK)
		fprintf(stderr, "recv() failed: %s\n", strerror(errno));
}

void spin_emu_controller()
{
	/*
	sigset_t sigset;
	sigfillset(&sigset);
	if (sigprocmask(SIG_SETMASK, &sigset, NULL))
		die_errno("sigprocmask");

	 */

	while (1) {
		fd_set fds;
		int ret;
		FD_ZERO(&fds);
		FD_SET(SPIN_EMU_SOCK, &fds);
		//fprintf(stderr, "Waiting for message\n");
		ret = select(SPIN_EMU_SOCK+1, &fds, NULL, NULL, NULL);
		if (ret < 0 && errno != EINTR)
			die_errno("select() failed");
		if (FD_ISSET(SPIN_EMU_SOCK, &fds))
			read_packets();
	}
}

uint spin1_emu_quit;
uint spin1_emu_retval;

static void dummy_usr1_handler(int sig)
{
}

// ------------------------------------------------------------------------
// simulation control functions
// ------------------------------------------------------------------------
uint spin1_start()
{


	// int counter=0; // CP debug for spin_emu
	sigset_t blocked, unblocked;
	struct sigaction sa;

	sa.sa_handler = dummy_usr1_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGUSR1, &sa, NULL) < 0)
		die_errno("sigaction");

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGALRM);

	if (sigprocmask(SIG_SETMASK, &blocked, NULL))
		die_errno("sigprocmask");
	sigemptyset(&unblocked);

	// synchronise with other application cores
	if (ncores > 1)
	{
		barrier_wait();
	}

	// initialise counter and ticks for simulation
	ticks = 0;
	run = 1;

	// simulate!
	while (!spin1_emu_quit) {

		int minprio = INT_MAX;
		int minev = -1;
		int i;


		pthread_mutex_lock(&evinfo_mutex);

		// Go through the registered event types and see if we have any queued
		for (i = 0; i < NUM_EVENTS; i++) {
			if (debug_events > 1)
				fprintf(stderr, "[%d,%d] ev queue %i pr=%d cb=%p queue %p\n",
					spin_emu_chip, spin_emu_core,
					i,
					evinfo[i].priority, evinfo[i].cback,
					evinfo[i].queue);
			if (!evinfo[i].cback || !evinfo[i].queue)
				continue;
			if (evinfo[i].priority > minprio)
				continue;
			minev = i;
			minprio = evinfo[i].priority;
		}  // task with minimum priority comes out of this. minev = callback type, minprio = its priority

		pthread_mutex_unlock(&evinfo_mutex);
		if (debug_events > 1)
			fprintf(stderr, "[%d,%d] minev %d\n",
				spin_emu_chip, spin_emu_core,
				minev);
		if (minev >= 0) {
			struct qevent *ev = NULL;
			callback_t cback;

			pthread_mutex_lock(&evinfo_mutex);
			ev = evinfo[minev].queue;
			//cback = evinfo[minev].cback;
            cback = ev->cback;
			if (debug_events > 1)
			{
				fprintf(stderr, "[%d,%d] ev %i pr=%d cb=%p item %x,%x\n",
					spin_emu_chip, spin_emu_core,
					minev,
					evinfo[i].priority, cback,
					ev->arg1, ev->arg2);
			}
			evinfo[minev].queue = ev->next;
			if (!ev->next)
				evinfo[minev].queue_tail = NULL;
			pthread_mutex_unlock(&evinfo_mutex);
			cback(ev->arg1, ev->arg2);
		}
		else {
			if (debug_events > 1)
				fprintf(stderr, "[%d,%d] suspending\n",
					spin_emu_chip, spin_emu_core);
			sigsuspend(&unblocked);
			if (debug_events > 1)
				fprintf(stderr, "[%d,%d] sigsuspend returned (%s)\n",
					spin_emu_chip, spin_emu_core,
					strerror(errno));
			if (errno != EINTR)
				die_errno("sigsuspend");
		}
		//pthread_kill(spin_emu_worker_thread, SIGUSR1);
	}

	return spin1_emu_retval;
}

void spin1_stop()
{
	// Tell the emulator that a core has stopped working
	char msg[] = {SPIN_EMU_PACKET_FINISHED};
	if (send(SPIN_EMU_SOCK, msg, 1, 0) < 0)
	{
		die_errno("send() failed for finished packet");
	}
	spin1_emu_quit = 1;

}

void spin1_kill(uint error)
{
	spin1_emu_quit = 1;
	spin1_emu_retval = error;
}

uint warning = 0;

/**
 * Every tick in the simulator we queue a timer event to call TIMER_TICK
 */
static void handle_timer_tick(int sig)
{
	assert(sig == SIGALRM);
	spin_emu_sim_time++;

	if (evinfo[TIMER_TICK].queue)
	{
		warning = warning+1;
		if(warning > 1)
			fprintf(stderr, "Warning! timer queue is not empty. In some cases this is fine, but you may wish to increase SLOW_DOWN_TIMER in default.cfg\n");
	}
	else
	{
		warning = 0;
	}
	queue_event(TIMER_TICK, spin_emu_sim_time, 0, evinfo[TIMER_TICK].cback);



	if(!spin1_emu_quit)
	{
		struct itimerval itv;
		struct sigaction sa;
		sa.sa_handler = handle_timer_tick;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		if (sigaction(SIGALRM, &sa, NULL) < 0)
			die_errno("sigaction");

		itv.it_interval.tv_sec = 0; //0
		itv.it_interval.tv_usec = 0; //time
    	itv.it_value.tv_sec = (timer_tick*SLOW_DOWN_TIMER)/1000000;
	    itv.it_value.tv_usec = (timer_tick*SLOW_DOWN_TIMER)%1000000; 
		if (setitimer(ITIMER_REAL, &itv, NULL) < 0)
			die_errno("setitimer");

	}
}

void spin1_set_timer_tick(uint time)
{
	struct itimerval itv;
	struct sigaction sa;
	timer_tick = time;
	sa.sa_handler = handle_timer_tick;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGALRM, &sa, NULL) < 0)
		die_errno("sigaction");


	itv.it_interval.tv_sec = 0; //0
	itv.it_interval.tv_usec = 0; //time
	itv.it_value.tv_sec = (timer_tick*SLOW_DOWN_TIMER)/1000000;
	itv.it_value.tv_usec = (timer_tick*SLOW_DOWN_TIMER)%1000000; 
	if (setitimer(ITIMER_REAL, &itv, NULL) < 0)
		die_errno("setitimer");
}

void spin1_set_core_map(uint chips, uint * core_map)
{


	uint i, j;
	uint nc = 0;
	uint cores;
	// count the number of cores
	for (i = 0; i < chips; i++)
	{
		cores = core_map[i];
		/* exclude monitor -- core 0 */
		for (j = 1; j < NUM_CPUS; j++)
	    {
			if (cores & (1 << j)) nc++;
	    }
	}

	ncores = nc;

	// if needed, setup mc routing table to implement barrier synchronisation
	// only the root application core does the setup
	if ((ncores > 1) && (rootAp))
	{
		barrier_setup(chips, core_map);
	}

}

uint spin1_get_simulation_time(void)
{
	return spin_emu_sim_time;
}

void spin1_delay_us (uint n)
{
	usleep(n);
}

// ------------------------------------------------------------------------

/****f* spin1_api.c/p2p_get
*
* SUMMARY
*  At the moment this function just returns the route to (0,0)
*/
uint p2p_get (uint entry)
{
	uint chipID = spin1_get_chip_id();
	uint my_x = chipID >> 8;
	uint my_y = chipID & 0xff;

	if(my_x > 0 && my_y > 0)
		return 4;
	else if(my_x > 0)
		return 3;
	else if(my_y > 0)
		return 5;
	else
		return 0;
}


/****f* spin1_api.c/barrier_setup
*
* SUMMARY
*
* SYNOPSIS
*  void barrier_setup(void)
*
* SOURCE
*/
void barrier_setup (uint chips, uint * core_map)
{

  uint i;
  uint my_cores = 0;

  // TODO: needs extending -- works for square core maps only!
  uint bsize = 0;
  uint bside = 1;
  for (i = 0; i < 32; i++)
  {
    if ((chips & (0x80000000 >> i)) != 0)
    {
      bsize = (31 - i) >> 1;
      bside = 1 << bsize;
      break;
    }
  }

  uint chipID = spin1_get_chip_id();

  // TODO: needs extending -- works for square core maps only!
  uint my_x = chipID >> 8;
  uint my_y = chipID & 0xff;
  my_chip = (my_x << bsize) | my_y;

  // check if root chip
  rootChip = (chipID == CHIP_ADDR(0, 0));
  rootAddr = CHIP_ADDR(0, 0);

  // setup routing entries for synchronisation barrier
  uint loc_route = 0;
  uint off_route = 0;
  uint rdygo_route;
  uint rdy2_route;

  // setup the local (on-chip) routes
  my_cores = core_map[my_chip] & 0xfffffffe;  // exclude monitor
  // need to do virtual to physical core ID conversion
  uint v;
  for (v = 1; v < NUM_CPUS; v++)  // go through virt_cpus
  {
	  if (my_cores & (1 << v))
	  {
		  //loc_route |= CORE_ROUTE(sv->v2p_map[v]);
		  // is sv->v2p_map[v] just the virtual core id of v?
		  loc_route |= CORE_ROUTE(v);
		  my_ncores++;
	  }
  }


  #if API_DEBUG == TRUE
    io_printf (IO_STD, "\t\t[api_debug] chip: %d, core map: 0x%x\n",
               my_chip, my_cores);
    spin1_delay_us (API_PRINT_DLY);
  #endif

  // TODO: needs fixing -- fault-tolerant tree
  // setup off-chip routes -- check for borders!
  // north
  if ((my_x == 0) && (my_y < (bside - 1)) &&
      ((core_map[my_chip + 1] & 0xfffffffe) != 0))
  {
    off_route |= (1 << NORTH); // add link to north chip
  }

  // east
  if ((my_x < (bside - 1)) && (my_y == 0) &&
      ((core_map[my_chip + bside] & 0xfffffffe) != 0))
  {
    off_route |= (1 << EAST); // add link to east chip
  }

  // north-east
  if ((my_y < (bside - 1)) && (my_x < (bside - 1)) &&
      ((core_map[my_chip + bside + 1] & 0xfffffffe) != 0))
  {
    off_route |= (1 << NORTH_EAST); // add link to north-east chip
  }

  // TODO: needs fixing -- non-fault-tolerant tree
  // TODO: doesn't use wrap around
  // spanning tree from root chip N, E & NE
  if (rootChip)
  {
    // compute number of active chips
    for (i = 0; i < chips; i++)
    {
      if ((i != my_chip) && ((core_map[i] & 0xfffffffe) != 0))
      {
        nchips++;

      }
    }

    // setup the RDYGO entry -- only off-chip routes!
    rdygo_route = off_route;

    // setup the RDY2 route -- packets come to me
    rdy2_route = CORE_ROUTE(spin1_get_core_id());

  }
  else
  {
    // setup the RDYGO entry -- packets to me and forward
    rdygo_route = CORE_ROUTE(chipID) | off_route;

    // setup the RDY2 route -- packets go to root chip
    // use p2p routing table to find the way
    rdy2_route = P2P_ROUTE(rootAddr);



  }

  // MC route table entries are given to the emulator using the following functions
  // setup the RDYGO entry
  spin_emu_send_route(SPIN_EMU_SOCK, 1, RDYGO_KEY, BARRIER_MASK, rdygo_route);

  // setup the RDY2 entry
  //fprintf(stderr, "\t\t[%d,%d] Sending rdy2_route=%08x\n", spin1_get_chip_id(), spin1_get_core_id(),rdy2_route);
  spin_emu_send_route(SPIN_EMU_SOCK, 3, RDY2_KEY, BARRIER_MASK, rdy2_route);

  // setup the GO entry
  uint route = (loc_route | off_route);
  spin_emu_send_route(SPIN_EMU_SOCK, 0, GO_KEY, BARRIER_MASK, route);




}
/*
*******/


/****f* spin1_api.c/barrier_wait
*
* SUMMARY
*
* SYNOPSIS
*  void barrier_wait(uint n)
*
* INPUTS
*  uint n: number of cores that synchronise
*
* SOURCE
*
*/
void barrier_wait (void)
{
	// TODO: A clear refactor here is to clean up the `(clock() / (CLOCKS_PER_SEC / 1000))` used everywhere
	clock_t starttime = clock() / (CLOCKS_PER_SEC / 1000);
	uint start;
	uint resend;

	if (rootAp)
	{
	    // let other cores go!
	    sv->lock = RDYGO_LCK;
		if (rootChip)
		{
			// root node, root application core
			// send rdygo packet
			spin1_send_mc_packet(RDYGO_KEY, 0, NO_PAYLOAD);
			// wait until all ready packets arrive and give the go signal
			start = starttime;          // initial value
			resend = start;           // may need to resend rdygo

			while ((((clock() / (CLOCKS_PER_SEC / 1000)) - start) < (BARRIER_RDY2_WAIT))
				  && ((barrier_rdy1_cnt < my_ncores) || (barrier_rdy2_cnt < nchips)))
			{

				if (((clock() / (CLOCKS_PER_SEC / 1000)) - resend) > BARRIER_RESEND_WAIT)
				{
					// send a new rdygo packet -- just in case the first was missed!
					spin1_send_mc_packet(RDYGO_KEY, 0, NO_PAYLOAD);
					resend = (clock() / (CLOCKS_PER_SEC / 1000));
				}

			}
			if ((barrier_rdy1_cnt != my_ncores) || (barrier_rdy2_cnt != nchips))
			{
			  fprintf(stderr, "\t\t[%d,%d] warning: failed to synchronise (%d/%d) (%d/%d).\n", spin1_get_chip_id(), spin1_get_core_id(),
								 barrier_rdy1_cnt, my_ncores, barrier_rdy2_cnt, nchips);
			  spin1_delay_us (API_PRINT_DLY);
			}
			// send go packet
			spin1_send_mc_packet(GO_KEY, 0, NO_PAYLOAD);

		}
		else
		{
			start = starttime;
			// non-root node, root application core
			// wait until the rdygo packet and all local ready packets arrive
			// timeout if taking too long!
			while ((((clock() / (CLOCKS_PER_SEC / 1000)) - start) < BARRIER_RDYGO_WAIT)
				  && ((barrier_rdy1_cnt < my_ncores) || (!barrier_rdygo))
			)
			{
			 continue;
			}
			// send rdy2 packet
			spin1_send_mc_packet(RDY2_KEY, 0, NO_PAYLOAD);
		}
	}

	else
	{
		// all others
		// wait for lock -- from local root application core
		// timeout if taking too long!
		start = starttime;
		while ((((clock() / (CLOCKS_PER_SEC / 1000)) - start) < BARRIER_LOCK_WAIT) && (sv->lock < RDYGO_LCK))
		{
		  continue;
		}

		// send local ready packet
		spin1_send_mc_packet(RDY1_KEY, 0, NO_PAYLOAD);

	}

	// wait until go packet arrives
	// timeout if taking too long!
	start = (clock() / (CLOCKS_PER_SEC / 1000));          // initial value
	while ((((clock() / (CLOCKS_PER_SEC / 1000)) - start) < BARRIER_GO_WAIT) && (barrier_go == FALSE))
	{
		continue;
	}
}



// ------------------------------------------------------------------------
// callback and task functions
// ------------------------------------------------------------------------
void spin1_callback_on(uint event_id, callback_t cback, int priority)
{
	pthread_mutex_lock(&evinfo_mutex);
	evinfo[event_id].priority = priority;
	evinfo[event_id].cback = cback;
	pthread_mutex_unlock(&evinfo_mutex);
}

void spin1_callback_off(uint event_id)
{
	pthread_mutex_lock(&evinfo_mutex);
	evinfo[event_id].priority = NULL;
	evinfo[event_id].cback = NULL;
	pthread_mutex_unlock(&evinfo_mutex);
}

uint spin1_schedule_callback(callback_t cback, uint arg0, uint arg1, uint priority)
{
	//spin1_callback_on(USER_EVENT, cback, priority);
	queue_event(USER_EVENT, arg0, arg1, cback);
    //spin1_trigger_user_event(arg0, arg1);
	// Abuses the user event because it makes the event dispatch simpler
	return 1;
}

uint spin1_trigger_user_event(uint arg0, uint arg1)
{
	queue_event(USER_EVENT, arg0, arg1, evinfo[USER_EVENT].cback);
	return 1;
}

// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
//  data transfer functions
// ------------------------------------------------------------------------
uint spin1_dma_transfer(uint tag, void *system_address, void *tcm_address, uint direction, uint length)
{

	static uint dma_id = 0;

	if (direction == DMA_READ)
	{


		memcpy(tcm_address, system_address, length);

	}
	else if (direction == DMA_WRITE)
	{

		memcpy(system_address, tcm_address, length);

	}
	else
	{

		die("spin1_dma_transfer got an invalid direction");
	}

	dma_pipeline.flip ^= 1; // Added to fix the get row_size line in dma_callback (in app_frame/dma.c)
	queue_event(DMA_TRANSFER_DONE, dma_id, tag, evinfo[DMA_TRANSFER_DONE].cback);

	return dma_id++;

}

void spin1_memcpy(void *dst, void const *src, uint len)
{
	memcpy(dst, src, len);
}


// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
//  communications functions
// ------------------------------------------------------------------------
uint spin1_send_mc_packet(uint key, uint data, uint load)
{
	struct spin_emu_mc_t mc = {key, data, load};
	spin_emu_send_mc(SPIN_EMU_SOCK, &mc);
	return 1;
}

void spin1_flush_rx_packet_queue(void)
{
}

void spin1_flush_tx_packet_queue(void)
{
}

// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// SDP related functions
// ------------------------------------------------------------------------
void spin1_msg_free (sdp_msg_t *msg)
{
	free(msg);
}

sdp_msg_t* spin1_msg_get (void)
{
	return xmalloc(sizeof(sdp_msg_t));
}

uint spin1_send_sdp_msg (sdp_msg_t *msg, uint timeout)
{

	char buf[1+sizeof(sdp_msg_t)];
	buf[0] = SPIN_EMU_PACKET_SDP;
	memcpy(buf+1, msg, sizeof(sdp_msg_t));
	if (debug_sdp_dispatch)
		fprintf(stderr, "[%d,%d] sending sdp to %x,%x\n",
			spin_emu_chip, spin_emu_core,
			msg->dest_addr, msg->dest_port);
	send(SPIN_EMU_SOCK, buf, 1+sizeof(sdp_msg_t), MSG_DONTWAIT);
	return 1;
}

// ------------------------------------------------------------------------


uint spin_emu_interrupt_mode = 0;
// ------------------------------------------------------------------------
//  interrupt control functions
// ------------------------------------------------------------------------
static uint set_interrupt_mode(uint mode)
{
	uint old_mode = spin_emu_interrupt_mode;
	sigset_t sigset;
	sigfillset(&sigset);
	if (!mode) {
		sigdelset(&sigset, SIGUSR1);
		sigdelset(&sigset, SIGALRM);
	}
	if (sigprocmask(SIG_SETMASK, &sigset, NULL))
		die_errno("sigprocmask");
	spin_emu_interrupt_mode = mode;
	return old_mode;
}

uint spin1_irq_disable(void)
{
	return set_interrupt_mode(1);
}

uint spin1_fiq_disable(void)
{
	return set_interrupt_mode(1);
}

uint spin1_int_disable(void)
{
	return set_interrupt_mode(1);
}

void spin1_mode_restore(uint sr)
{
	set_interrupt_mode(sr);
}

// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
//  system resources access funtions
// ------------------------------------------------------------------------

uint  spin1_get_id(void)
{
	return (spin1_get_chip_id() << 5) | spin1_get_core_id();
}

uint  spin1_get_core_id(void)
{
	return spin_emu_core;
}

uint  spin1_get_chip_id(void)
{
	return ( (chip_my_x(spin_emu_chip) <<8) + chip_my_y(spin_emu_chip) );
}


uchar spin1_get_leds(void)
{
	// DUMMY
	return 1; // CP fix compile warning about non-void function
}

void  spin1_set_leds(uchar leds)
{
	// DUMMY
}

uint spin1_set_mc_table_entry(uint entry, uint key, uint mask, uint route)
{

	if (entry < APP_MC_ENTRIES)
	{
		// top priority entries reserved for the system
		entry += SYS_MC_ENTRIES;
		spin_emu_send_route(SPIN_EMU_SOCK, entry, key, mask, route);
		return SUCCESS;
	}
	else
	{
		return FAILURE;
	}

}


void* spin1_malloc(uint bytes)
{
	void *ptr = spin_emu_dtcm_freeptr;
	spin_emu_dtcm_freeptr += bytes;
	return ptr;
}


// ------------------------------------------------------------------------
// rts initialization function
// called before the application program starts!
// ------------------------------------------------------------------------
/****f* spin1_api.c/rts_init
*
* SUMMARY
*  This function is a stub for the run-time system.
*  initializes peripherals in the way the RTS
*  is expected to do
*
* SYNOPSIS
*  void rts_init (void)
*
* SOURCE
*/
void rts_init (void)
{
  // try to become root application core for this chip
  rootAp = lock_try (LOCK_API_ROOT);

  if (rootAp)
  {
	  //uint chipID = spin1_get_chip_id();
	  // --- setup routing table ---
	  // (remember the MC routing table is managed by the dispatcher in the emulator)
	  // setup the RDY1 entry
	  // local ready packets come to me
	  uint route = CORE_ROUTE(spin_emu_core);
	  spin_emu_send_route(SPIN_EMU_SOCK, 2, RDY1_KEY, BARRIER_MASK, route);

	  // Initialise ready counts (I'm already ready!)
	  barrier_rdy1_cnt = 1;
	  barrier_rdy2_cnt = 1;
	  //fprintf(stderr, "[%d,%d] init barrier_rdy2_cnt=%d\n",spin_emu_chip, spin_emu_core,barrier_rdy2_cnt);

	  // router initialized -- let other cores go!
	  sv->lock = RTR_INIT_LCK;

  }
  else {

	  clock_t starttime = clock() / (CLOCKS_PER_SEC / 1000);

	  while((((clock() / (CLOCKS_PER_SEC / 1000)) - starttime) < ROUTER_INIT_WAIT) && (sv->lock < RTR_INIT_LCK)) {
		  continue;
	  }
  }
}

// ------------------------------------------------------------------------
// rts cleanup function
// called after the application program finishes!
// ------------------------------------------------------------------------
void rts_cleanup (void)
{
	fprintf(stderr, "[%d,%d] rts_cleanup not implemented!\n",spin_emu_chip, spin_emu_core);
	// TODO: Will we need to send some form of packet to the emulator to get it to reinitialise cores and routing tables?
	// So far this has not caused any problems so has been left unimplemented.
}
