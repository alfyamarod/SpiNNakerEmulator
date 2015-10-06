/**
 *  scamp like functions that are performed by virtual cores in the emulator
 *  Note: some SDP commands will never reach cores, e.g. p2pc
 */

#include <sys/time.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "spin_emu.h"
#include "spin1_api.h"
#include "scamp_emu.h"

uint cmd_write(sdp_msg_t *msg)
{
	// arg1=addr
	// arg2=len
	// arg3=type
	uint len = msg->arg2;
	uint type = msg->arg3;
	uchar *buffer = msg->data;

	if(debug_sdp_dispatch)
	{
		fprintf(stderr, "Writing data to %x\n", msg->arg1);
	}

	// MO: The below commented out lines (or something similar) are needed if we want to make memory executable,
	// but i'm avoiding having to do this by writing applications to a file and executing the file.
	/*
	if(msg->arg1 < 0x10000000)
		{
		if( mprotect((msg->arg1 - (msg->arg1 % getpagesize())), len, PROT_EXEC | PROT_READ | PROT_WRITE) == -1)
		{
			printf("mprotect 1 failed!!! %s\n", strerror(errno));
			exit(1);
		}
	}
	*/

	if (len > BUF_SIZE || type > TYPE_WORD)
	{
		msg->cmd_rc = RC_ARG;
		return 0;
	}

	// we have to guard against things writing to locations where they're not allowed to in the emulator
	// spiNNaker boards location 0xf5000000 is 0x1FFF8000 in the emulator
	if(msg->arg1 >= 0xf5000000) {
		msg->arg1 = msg->arg1 - (0xf5000000 - 0x1FFF8000);
	}
	uint addr = msg->arg1;


	uint i;
	if (type == TYPE_BYTE)
	{
		uchar *mem = (uchar*) addr;
		uchar *buf = (uchar*) buffer;
		for (i = 0; i < len; i++)
		{
			mem[i] = buf[i];
		}
	}
	else if (type == TYPE_HALF)
	{
		ushort* mem = (ushort*) addr;
		ushort* buf = (ushort*) buffer;

		for (i = 0; i < len / 2; i++)
		{
			mem[i] = buf[i];
		}
	}
	else {
		uint *mem = (uint *) addr;
		uint *buf = (uint *) buffer;

		for (i = 0; i < len / 4; i++)
		{
			mem[i] = buf[i];
		}
	}

	return len;
}


uint cmd_read (sdp_msg_t *msg)
{
	uint len = msg->arg2;
	uint type = msg->arg3;

	if (len > BUF_SIZE || type > TYPE_WORD)
	{
		msg->cmd_rc = RC_ARG;
		return 0;
	}

	// we have to guard against things reading from locations where they're not allowed to
	// spiNNaker boards location 0xf5000000 is 0x1FFF8000 in the emulator
	if(msg->arg1 >= 0xf5000000)
	{
		msg->arg1 = msg->arg1 - (0xf5000000 - 0x1FFF8000);
	}
	uint addr = msg->arg1;

	uchar *buffer = (uchar *) &msg->arg1;
	uint i;
	if (type == TYPE_BYTE)
	{
		uchar *mem = (uchar *) addr;
		uchar *buf = (uchar *) buffer;

		for (i = 0; i < len; i++)
		{
			buf[i] = mem[i];
		}
	}
	else if (type == TYPE_HALF)
	{
		ushort *mem = (ushort *) addr;
		ushort *buf = (ushort *) buffer;

		for (i = 0; i < len / 2; i++)
		{
			buf[i] = mem[i];
		}
	}
	else
    {
		uint *mem = (uint *) addr;
		uint *buf = (uint *) buffer;

		for (i = 0; i < len / 4; i++)
		{
			buf[i] = mem[i];
		}
    }

	return len;
}

uint cmd_ver (sdp_msg_t *msg)
{
	char p2p_addr = spin1_get_chip_id();

	msg->arg1 = (p2p_addr << 16) + (spin1_get_chip_id() << 8) + spin1_get_core_id();
	msg->arg2 = (VER_NUM << 16) + BUF_SIZE;
	time_t rawtime;
	time(&rawtime);
	msg->arg3 = rawtime;
	strcpy((char *)msg->data, VER_STR);

	return 12 + sizeof (VER_STR);
}

/**
 * Gets the program (which is stored at the memory location specified by msg->arg1) and runs it by creating a file from it.
 * This method had 2 complications that it had to overcome:
 * 	1) How to execute a program stored in memory?
 * 	2) How to know how big a program is in memory when a standard AS packet doesn't contain that data?
 *
 * 1) How to execute a program stored in memory?
 * There are tons of options that allow us to run a program from memory, all of them horrible. e.g. __asm___
 * There are more portable ways of running the program from a file, e.g. exec() and system()
 * I've tested a few methods, and exec is the easiest and lets processes keep their fd's
 *
 * 2) How to know how big a program is in memory, when a standard AS packet doesn't contain that data?
 * As no information about the end memory location is given we will have to store it somewhere before getting here,
 * or calculated it from the ELF headers in memory.
 *
 */
uint cmd_as (sdp_msg_t *msg)
{
	// TODO: check that first four bytes are  0x7f 'E' 'L' 'F' before executing.
	// Easiest way to get something from memory to run? copy it to a file first!
	// create the file
	char executable[PATH_MAX];
	char foo[15];
	sprintf(foo, "%d", spin_emu_chip);
	char bar[15];
	sprintf(bar, "%d", spin_emu_core);
	strcpy (executable, spin_emu_tmpdir);
	strcat (executable, "/tempapp");
	strcat (executable, foo);
	strcat (executable, bar);
	if(debug_exec)
		fprintf(stderr, "temp app file=%s\n", executable);
	FILE *fp;
	remove(executable);
	fp=fopen(executable, "a+");

	//now where to copy data from?
	if(msg->arg1 >= 0xf5000000)
	{
		// this as command is refering to the spiNNaker SYRAM location (might not be the same in the emulator)
		msg->arg1 = msg->arg1 - (0xf5000000 - 0x1FFF8000);
	}
	// else assume the starting address is in range
	// Now we just need to figure out how much data to write to file
	// TODO: this bit is a hack until we add code to read ELF headers properly
	uint size;
	if(msg->arg1 >= SYSRAM_BASE && msg->arg1 < SYSRAM_TOP)
	{
		size = SYSRAM_TOP - msg->arg1;
	}
	else if(msg->arg1 >= DTCM_BASE && msg->arg1 < DTCM_TOP)
	{
		size = DTCM_TOP - msg->arg1;
	}
	uchar *mem = (uchar*) msg->arg1;
	fwrite(mem, 1, size, fp);
	fclose(fp);

	// execute the program
	if(chmod(executable, S_IRUSR|S_IXUSR) < 0){
		perror("chmod failed");
	    exit(1);
	 }



	char arg0[12],arg1[12],arg2[12],arg3[12],arg4[PATH_MAX],arg5[12];
	snprintf(arg0, 12, "%d", spin_emu_chip);
	snprintf(arg1, 12, "%d", spin_emu_core);
	snprintf(arg2, 12, "%d", NUM_CHIPS_X);
	snprintf(arg3, 12, "%d", NUM_CHIPS_Y);
	snprintf(arg4, PATH_MAX, "%s", spin_emu_tmpdir);
	snprintf(arg5, 12, "%d", SLOW_DOWN_TIMER);
	char * const argv[7] = {arg0,arg1,arg2,arg3,arg4,arg5,NULL};

	//fprintf(stderr, "[%d,%d] executing\n", spin_emu_chip, spin_emu_core);
	execv(executable, argv);
	die("[%d,%d] execv('%s') failed, cannot recover", spin_emu_chip, spin_emu_core,executable);
	return 0;
}

