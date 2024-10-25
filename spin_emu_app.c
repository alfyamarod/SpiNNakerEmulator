#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>

#include "spin_emu.h"

pthread_t spin_emu_controller_thread;
pthread_t spin_emu_worker_thread;

int NUM_CHIPS_X;
int NUM_CHIPS_Y;
int NUM_CHIPS;
int **spin_emu_socket_dispatch;
int **spin_emu_socket_core;
int SLOW_DOWN_TIMER;

static void dummy_usr1_handler(int sig)
{
}

void c_main(void) {
    // Actual logic that runs in the core
}



int main (int argc, char *argv[])
{
    // The core running the app is just a subprocess
    // so getpid() gets called
	sigset_t blocked;
	struct sigaction sa;
	setpriority(PRIO_PROCESS, getpid(), 10);
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

	spin_emu_chip = atoi(argv[0]);
	spin_emu_core = atoi(argv[1]);
	NUM_CHIPS_X = atoi(argv[2]);
	NUM_CHIPS_Y = atoi(argv[3]);
	NUM_CHIPS = (NUM_CHIPS_X*NUM_CHIPS_Y);
	spin_emu_tmpdir = argv[4];
	SLOW_DOWN_TIMER = atoi(argv[5]);

	if(debug_exec)
		fprintf(stderr, "[%d,%d] starting, tempdir=%s\n", spin_emu_chip, spin_emu_core,spin_emu_tmpdir);

	spin_emu_mmap_core_mem(spin_emu_chip, spin_emu_core);


	pthread_create(&spin_emu_controller_thread, NULL,spin_emu_controller, NULL);
	spin_emu_worker_thread = pthread_self();


	// spin1_api functions that are called before application starts
	rts_init();


	/**
	 * The next block of commented out code is for experimental scheduling purposes,
	 * and should not be used yet.
	 * Each process is only allowed to run for a maximum time quantum.
	 * only possible with super user permissions
	 */
	/*
	struct sched_param param;
	sched_getparam(0, &param);
	int priority = sched_get_priority_max( SCHED_RR )-10;
	param.__sched_priority = priority;
	if(sched_setscheduler(0, SCHED_RR, &param) < 0)
	{
		printf("error while setting process priority to %d [%s]\n", priority, strerror(errno));
	}
	int priority = -1;
	if(setpriority(PRIO_PROCESS, 0, priority) < 0)
	{
		printf("error while setting process priority to %d [%s]\n", priority, strerror(errno));
	}
	*/

    // TODO: There is a race condition in app_monitoring c_main, documentation can be found in bug report ID:1
	c_main();

	sigset_t sigset;
	sigemptyset(&sigset);
	if (sigprocmask(SIG_SETMASK, &sigset, NULL))
		die_errno("sigprocmask");

	//fprintf(stderr, "[%d,%d] Simulation stopped.\n",spin_emu_chip, spin_emu_core);


	pthread_join(spin_emu_controller_thread, NULL);



	return 1; // CP fix compile warning about non-void function
}
