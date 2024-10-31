#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "spin_emu.h"

char *spin_emu_tmpdir;

int spin_emu_ether_fd;

int spin_emu_chip;
int spin_emu_core;


int SLOW_DOWN_TIMER;


int **spin_emu_child_pid;

void *spin_emu_dtcm_freeptr = (void*) DTCM_BASE;



uint virt_cpu;

int debug_mc_dispatch = 0;
int debug_sdp_dispatch = 1;
int debug_events = 0;
int debug_startup = 0;
int debug_exec = 0;


void spin_emu_setup_channels()
{
	int i,c;
	for (c = 0; c < NUM_CHIPS; c++) {
		for (i = 0; i < NUM_CPUS; i++) {
			int fd[2];
			if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) < 0)
				die_errno("socketpair() failed");
			spin_emu_socket_dispatch[c][i] = fd[0];
			spin_emu_socket_core[c][i] = fd[1];
		}

	}
}

/**
 * Create a socket to receive commands
 */
void bind_ethernet_channel()
{
	struct addrinfo hints, *servinfo, *p;
	int rv;

	 memset(&hints, 0, sizeof hints);
	 hints.ai_family = AF_INET; // use AF_INET6 to force IPv6
	 hints.ai_socktype = SOCK_DGRAM;

	 const int n = snprintf(NULL, 0, "%d", SPIN_EMU_PORT);
	 char buf[n+1];
	 snprintf(buf, n+1, "%d", SPIN_EMU_PORT);

	 rv = getaddrinfo("127.0.0.1", buf, &hints, &servinfo);

	 if (rv != 0) {
		 fprintf(stderr, "getaddrinfo: %s on port %d\n", gai_strerror(rv), SPIN_EMU_PORT);
		 exit(EXIT_FAILURE);
	 }

	 // loop through all the results and bind to the first we can (NOTE: careful if you enable IPv6, IPTAG currently doesn't support it)
	 for(p = servinfo; p != NULL; p = p->ai_next) {
	     if ((spin_emu_ether_fd = socket(p->ai_family, p->ai_socktype,
	             p->ai_protocol)) == -1) {
	    	  fprintf(stderr, "socket\n");
	         continue;
	     }

	     int reuse = 1;

	     if (setsockopt(spin_emu_ether_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int))
		 == -1) {
		 fprintf(stderr, "setsockopt(SO_REUSEADDR) failed");
		 close(spin_emu_ether_fd);
		 continue;
	     }

	     
	     if (bind(spin_emu_ether_fd, p->ai_addr, p->ai_addrlen) == -1) {
	    	 close(spin_emu_ether_fd);
	         fprintf(stderr, "socket could not bind\n");
	     	 continue;
		 }
	     break; // connected successfully
	 }

	 if (p == NULL) {
	     // looped off the end of the list with no socket
	     fprintf(stderr, "failed to bind socket\n");
	     exit(2);
	 }

	 freeaddrinfo(servinfo); // all done with this structure

}



void setup_map(void *base, size_t size, char *namefmt, ...)
{
	char name[PATH_MAX];
	int len;
	int fd;
	void *map;
	va_list ap;

	va_start(ap, namefmt);
	len = strlen(spin_emu_tmpdir);
	memcpy(name, spin_emu_tmpdir, len);
	name[len] = '/';
	vsnprintf(name+len+1, PATH_MAX-len-1, namefmt, ap);
	va_end(ap);

	if (debug_startup)
		fprintf(stderr, "mapping '%s' at %p (%d)\n",
			name, base, size);

	fd = open(name, O_CREAT|O_RDWR, 0644);
	if (fd < 0)
		die_errno("open %s", name);
	if (ftruncate(fd, size) < 0)
		die_errno("ftruncate %s", name);
	map = mmap(base, size, PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED, fd, 0);
	if (map == MAP_FAILED || map != base)
		die_errno("mmap sysfile failed or bad");
}

void spin_emu_mmap_core_mem(int chip, int core)
{
	setup_map((void*)DTCM_BASE, DTCM_SIZE, "dtcm_%d_%d", chip, core);
	setup_map((void*)SYSRAM_BASE, (SYSRAM_SIZE+0x00010000), "sysram_%d", chip);   // plus 0x00010000 for SV (System) RAM definitions
	setup_map((void*)SDRAM_BASE, SDRAM_SIZE, "sdram_%d", chip);
}

void spin_emu_munmap_core_mem()
{
	munmap((void*)DTCM_BASE, DTCM_SIZE);
	munmap((void*)SYSRAM_BASE, (SYSRAM_SIZE+0x00010000)); // plus 0x00010000 for SV (System) RAM definitions
	munmap((void*)SDRAM_BASE, SDRAM_SIZE);
}

void spin_emu_msync_core_mem()
{
	msync((void*)DTCM_BASE, DTCM_SIZE,MS_SYNC);
	msync((void*)SYSRAM_BASE, (SYSRAM_SIZE+0x00010000),MS_SYNC); // plus 0x00010000 for SV (System) RAM definitions
	msync((void*)SDRAM_BASE, SDRAM_SIZE,MS_SYNC);
}

/*
 * Starts a virtual "core"
 * The parent process is  the emulator which is responsible for listening to messages containing instructions
 */
static void run_core(int chip, int core)
{

	if(debug_startup)
	{
		fprintf(stderr, "Creating core [%d,%d] fd=%d, emu_sock=%d\n", chip, core,spin_emu_socket_core[chip][core],SPIN_EMU_SOCK);
	}

	//char buf[PATH_MAX+1];
	if (dup2(spin_emu_socket_core[chip][core], SPIN_EMU_SOCK) < 0)
		die_errno("cannot dup2() to comm socket[%d,%d]", chip, core);


	spin_emu_chip = chip;
	spin_emu_core = core;
	// Creates the memory mapped files for each chip/core
	spin_emu_mmap_core_mem(chip, core);

	// Core listening for packets until an "as" command is received at which point this controller is killed and replaced by an application
	spin_emu_controller(NULL);
}



/**
 * Runs each "core" as a simple process which listens for SDP messages
 * NOTE: each forked process may be replaced at a later time following an AS
 * message (see scamp_emu.c)
 */

void spin_emu_spawn_cores()
{
	int c, i;
	for (c = 0; c < NUM_CHIPS; c++) {
		for (i = 0; i < NUM_CPUS; i++) {
			int pid = fork();
			if (pid < 0)
				die_errno("fork() for core");
			if (pid > 0)
				spin_emu_child_pid[c][i] = pid;
			if (!pid) {
				run_core(c, i);
				exit(0);
			}


		}
	}
}
