#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <lcfg_static.h>

#include "spin_emu.h"


/**
 * Simple main to kick off the emulator process
 * main looks for a config file supplied on the command line
 * or uses default.cfg from the emu directory
 */
int main(int argc, char *argv[])
{

	SLOW_DOWN_TIMER = 1;

	struct lcfg *c;
	if( argc == 2 )
	{
		c = lcfg_new(argv[1]);
		printf("Loading config: %s\n",argv[1]);
	}
	else
	{
		c = lcfg_new("./default.cfg");
		printf("Loading config: default.cfg\n");
	}

	if( lcfg_parse(c) != lcfg_status_ok )
	{
		printf("config parse error: %s\nStarting emulator anyway and will wait for P2PC packet.\n", lcfg_error_get(c));
		spin_emu_dispatcher(0);
	}
	else
	{
		printf("config loaded OK!\n");

		void* data;
		size_t len;

		// TODO - Low priority: Reading in each value like this looks lazy, does liblcfg give us a nicerway?
		lcfg_value_get(c, "X_CHIPS", &data, &len);
		char stringValue1[len+1];
		memcpy(stringValue1, data, len);
		stringValue1[len] = NULL;
		int x = atoi(stringValue1);

		lcfg_value_get(c, "Y_CHIPS", &data, &len);
		char stringValue2[len+1];
		memcpy(stringValue2, data, len);
		stringValue2[len] = NULL;
		int y = atoi(stringValue2);

		lcfg_value_get(c, "SLOW_DOWN_TIMER", &data, &len);
		char stringValue3[len+1];
		memcpy(stringValue3, data, len);
		stringValue3[len] = NULL;
		SLOW_DOWN_TIMER = atoi(stringValue3);

		spin_emu_dispatcher(2, x, y);
	}

	lcfg_delete(c);

	return 1;
}
