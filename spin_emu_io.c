#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "spin_emu.h"
//#include "spin1_api.h"
//#include "spinn_io.h"


/* UNIMPLEMENTED
void io_put_char (char *stream, uint c);
void io_put_str (char *stream, char *s, int d);
void io_put_int (char *stream, int n, uint d, uint pad); // pad not used!
void io_put_uint (char *stream, uint n, uint d, uint pad);
void io_put_zhex (char *stream, uint n, uint d);
void io_put_hex (char *stream, uint n, uint d, uint pad);
void io_put_nl (char *stream);
void io_put_mac (char *stream, uchar *s);
void io_put_ip (char *stream, uchar *s);
*/

void io_printf (char *stream, char *fmt, ...)
{
	FILE *fp;
	const char *type;
	char buf[1024];
	int i = 0;
	va_list ap;

	if (stream == IO_STD)
	{
		fp = stdout;
		type = "std";
	}
	else if (stream == IO_DBG)
	{
		fp = stderr;
		type = "dbg";
	}
	else if (stream == IO_BUF)
	{
		fp = stderr;
		type = "dbg";
	}
	else if (stream == IO_NULL)
	{
		return;
	}
	else
	{
		die("unknown stream type %d", (int) stream);
	}

	i += snprintf(buf+i, 1024-i, "[%s %d,%02d] ", type, spin_emu_chip, spin_emu_core);

	va_start(ap, fmt);
	i += vsnprintf(buf+i, 1024-i, fmt, ap);
	va_end(ap);


	if (stream == IO_STD) {
		// Also send message to tubotron
		sdp_msg_t *msg = xmalloc(sizeof(sdp_msg_t));
		int len = strlen(buf);
		memcpy(&msg->arg1, buf, len);
		msg->tag = 0;                    // IPTag 0 (corrected from 1 by CP 1st Oct 2013)
		msg->dest_port = PORT_ETH;       // Ethernet
		msg->dest_addr = 0;   		     // Root chip
		msg->srce_port = spin1_get_core_id();
		msg->srce_addr = spin1_get_chip_id();
		msg->cmd_rc = 64; // Tube
		msg->length = len+12;
		spin1_send_sdp_msg(msg, 100);

	}
	else {
		fwrite(buf, strlen(buf), 1, fp);
		fflush(fp);
	}


}
