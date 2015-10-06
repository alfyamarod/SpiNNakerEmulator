#ifndef SCAMP_H
#define SCAMP_H

#define VER_SW			SW_SCAMP
#define VER_NUM  		0			// < 1024, please
#define VER_STR  		"SC&MP Emulator/SpiNNaker"


#define TAG_FIXED_SIZE		4	// At bottom of table
#define TAG_POOL_SIZE		12

#define FIRST_POOL_TAG		TAG_FIXED_SIZE
#define LAST_POOL_TAG		(TAG_FIXED_SIZE + TAG_POOL_SIZE - 1)

#define TAG_TABLE_SIZE 		(TAG_FIXED_SIZE + TAG_POOL_SIZE)


#ifdef TLM // !! dups
#define HOP_TABLE_SIZE		512
#else
#define HOP_TABLE_SIZE		16384
#endif

#define NN_HOP_MASK 		0x3ff		// !! Allows hops <= 1023

uint cmd_ver (sdp_msg_t *msg);
uint cmd_write(sdp_msg_t *msg);
uint cmd_read (sdp_msg_t *msg);
uint cmd_as (sdp_msg_t *msg);


typedef struct pkt_t
{
  uint ctrl;
  uint data;
  uint key;
} pkt_t;


typedef struct pkt_buf_t
{
  struct pkt_buf_t *next;
  volatile uchar flags;
  uchar fwd;
  uchar delay;
  uchar link;
  pkt_t pkt;
} pkt_buf_t;


#endif
