#ifndef _SPIN_EMU_H_
#define _SPIN_EMU_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <netinet/in.h>
#include <assert.h>

/* suppress warning */
//#undef NULL
//#include "spin1_api.h"


// ------------------------------------------------------------------------
// general parameters and definitions
// ------------------------------------------------------------------------
/* boolean constants */
//! Boolean true
#define TRUE               (0 == 0)
//! Boolean false
#define FALSE              (0 != 0)
/* function results   */
//! Function result: Success
#define SUCCESS            (uint) 1
//! Function result: Failure
#define FAILURE            (uint) 0

typedef unsigned char uchar; 
typedef unsigned int uint;
typedef unsigned short int ushort;
typedef uint64_t uint64;



// ------------------------------------------------------------------------
// DMA transfer parameters
// ------------------------------------------------------------------------
//! DMA transfer direction (from core point of view)
enum {
    DMA_READ =          0, //!< DMA read from SDRAM to TCM
    DMA_WRITE =         1  //!< DMA write from TCM to SDRAM
};


// ------------------------------------------------------------------------
// packet parameters
// ------------------------------------------------------------------------
//! Spinnaker packet payload presence flag
enum {
    NO_PAYLOAD =        0, //!< No payload word present
    WITH_PAYLOAD =      1  //!< Payload word present
};

#define DTCM_SIZE     0x00010000  //64 KB
#define DTCM_BASE     0x00400000
#define DTCM_TOP      (DTCM_BASE + DTCM_SIZE)
#define SYSRAM_BASE   0xf5000000  // unbuffered
//#define SYSRAM_BASE   0xe5000000  // buffered
#define SYSRAM_SIZE   0x00008000  //32 KB
#define SDRAM_BASE    0x60000000  //unbuffered
//#define SDRAM_BASE    0x70000000  //buffered
#define SDRAM_SIZE    (128 * 1024 * 1024)   //128 MB
#define SYSRAM_TOP (SYSRAM_BASE + SYSRAM_SIZE)

#define NUM_CPUS      8 // cores
#define NUM_LINKS     6 // Number of SpiNNaker links per chip

#define PORT_ETH 255



/*******************************************************************/
// sark.h


#define SV_SSIZE        32          //!< SROM data size
#define SV_USIZE        96          //!< Uninitialised size
#define SV_ISIZE        128         //!< Initialised to 0
#define SV_VSIZE        32          //!< Reset vectors
#define SV_RSIZE        64          //!< Random in SysRAM
#define SV_SIZE         0x1000      //!< Everything fits in this!

#define SV_SROM         (SYSRAM_TOP - SV_SSIZE)          //!< e5007fe0
#define SV_UBASE        (SV_SROM - SV_USIZE)             //!< e5007f80
#define SV_IBASE        (SV_UBASE - SV_ISIZE)            //!< e5007f00
#define SV_SV           SV_IBASE                         //!< e5007f00

//! \name SARK I/O stream identifiers
//! \{
//! Used with io_printf() and io_put_char()

#define IO_STD     ((char *) 0)    //!< Direct I/O via SDP
#define IO_DBG     ((char *) 1)    //!< Low-level debug I/O
#define IO_BUF     ((char *) 2)    //!< SDRAM buffer (normal for running apps)
#define IO_NULL    ((char *) 3)    //!< Output > `/dev/null`

//! Memory size types
enum sark_scp_memory_size_types {
    TYPE_BYTE = 0,              //!< Specifies byte access
    TYPE_HALF = 1,              //!< Specifies short (16-bit) access
    TYPE_WORD = 2               //!< Specifies word (32-bit) access
};


/*!
Allocations of SysCtl Test & Set registers (locks)
*/

enum spin_lock_e {
    LOCK_MSG,           //!< Msg buffers in SysRAM
    LOCK_MBOX,          //!< Mailbox flag variable
    LOCK_ETHER,         //!< Ethernet Tx (unused...)
    LOCK_GPIO,          //!< GPIO port (unused...)
    LOCK_API_ROOT,      //!< Spin1 API
    LOCK_SEMA,          //!< Sema access
    LOCK_HEAP,          //!< Heap in System / SDRAM
    LOCK_RTR            //!< Router
};


typedef struct vcpu {           // 128 bytes
    uint r[8];                  //!<  0 - r0-r7
    uint psr;                   //!< 32 - cpsr
    uint sp;                    //!< 36 - sp
    uint lr;                    //!< 40 - lr
    uchar rt_code;              //!< 44 - RT error code
    uchar phys_cpu;             //!< 45 - Physical CPU
    uchar cpu_state;            //!< 46 - CPU state
    uchar app_id;               //!< 47 - Application ID
    void *mbox_ap_msg;          //!< 48 - mbox msg MP->AP
    void *mbox_mp_msg;          //!< 52 - mbox msg AP->MP
    volatile uchar mbox_ap_cmd; //!< 56 - mbox command MP->AP
    volatile uchar mbox_mp_cmd; //!< 57 - mbox command AP->MP
    ushort sw_count;            //!< 58 - SW error count (saturating)
    char *sw_file;              //!< 60 - SW source file name
    uint sw_line;               //!< 64 - SW source line (could be short?)
    uint time;                  //!< 68 - Time of loading
    char app_name[16];          //!< 72 - Application name
    void *iobuf;                //!< 88 - IO buffer in SDRAM (or 0)
    uint sw_ver;                //!< 92 - SW version
    uint __PAD[4];              //!< 96 - (spare)
    uint user0;                 //!< 112 - User word 0
    uint user1;                 //!< 116 - User word 1
    uint user2;                 //!< 120 - User word 2
    uint user3;                 //!< 124 - User word 3
} vcpu_t;

//! Used in the block memory allocator (4 bytes)
typedef struct mem_link {
    struct mem_link *next;      //!< Pointer to next free block
} mem_link_t;

//! Used in the block memory allocator (8 bytes)
typedef struct mem_block {
    mem_link_t *free;   //!< Pointer to first free block
    ushort count;       //!< Count of blocks in use
    ushort max;         //!< Maximum blocks used
} mem_block_t;

//! Copy of router entry (16 bytes)
typedef struct rtr_entry {
    ushort next;        //!< Index of next block
    ushort free;        //!< Index of next free block (or app_id)
    uint route;         //!< Route word
    uint key;           //!< Key word
    uint mask;          //!< Mask word
} rtr_entry_t;

//! \brief Stores info relating to AppIDs.
//!
//! The "cores" field is zero if the ID is not in use.
typedef struct app_data {
    uchar cores;        //!< Number of cores using this ID
    uchar clean;        //!< Set if this ID has been cleaned
    uchar sema;         //!< Semaphore
    uchar lead;         //!< Lead core number
    uint mask;          //!< Mask of cores using this ID
} app_data_t;



// Heap data structures

/*!
\brief Heap data block.

One of these appears at the start of each
block in the heap so each allocation of N bytes in the heap
requires N+8
*/

typedef struct block {
    struct block *next;         //!< Chains all blocks together (in address order)
    struct block *free;         //!< Chains free blocks together (in address order)
} block_t;

/*!
\brief Heap root structure.

One of these appears at the start of the
heap area and maintains two lists, one containing all blocks in
the heap and one containing only free blocks. The heap always
contains a 'last' block, of zero size, which is used to mark the
end of the heap.
*/

typedef struct {
    block_t *free;              //!< Root of free block chain
    block_t *first;             //!< First block
    block_t *last;              //!< Last block (zero size, never used for storage)
    uint free_bytes;            //!< Number of free bytes left
    uchar buffer[];             //!< Buffer for blocks
} heap_t;


/*!
\brief SDP message definition

\note the length field is the number of bytes _following the checksum._
It will be a minimum of 8 as the SDP header should always be present.

\note make sure to comply with sark_block_init() requirements:
    1. size must be a non-zero multiple of 4 bytes.
    2. first field must be a pointer to same struct type.
*/


#define SDP_BUF_SIZE    256     //!< SDP data buffer capacity
#define BUF_SIZE SDP_BUF_SIZE

#pragma pack(push, 1)
typedef struct sdp_msg {        // SDP message - 292 bytes
    struct sdp_msg *next;       //!< Next in free list
    ushort length;              //!< length
    ushort checksum;            //!< checksum (if used)

    // sdp_hdr_t (mandatory)

    uchar flags;                //!< Flag byte
    uchar tag;                  //!< IP tag
    uchar dest_port;            //!< Destination port/CPU
    uchar srce_port;            //!< Source port/CPU
    ushort dest_addr;           //!< Destination address
    ushort srce_addr;           //!< Source address

    // cmd_hdr_t (optional)

    ushort cmd_rc;              //!< Command/Return Code
    ushort seq;                 //!< Sequence number
    uint arg1;                  //!< Arg 1
    uint arg2;                  //!< Arg 2
    uint arg3;                  //!< Arg 3

    // user data (optional)

    uchar data[SDP_BUF_SIZE];   //!< User data (256 bytes)

    uint __PAD1;                //!< Private padding
} sdp_msg_t;

#pragma pack(pop)


/*!
\brief Struct holding the System Variables.

Placed at the top of System RAM at 0xe5007f00 (#SV_SV)
*/

typedef struct sv {
    ushort p2p_addr;            //!< 00 P2P address of this chip
    ushort p2p_dims;            //!< 02 P2P dimensions

    ushort dbg_addr;            //!< 04 P2P address for debug messages
    uchar  p2p_up;              //!< 06 Non-zero if P2P networking active
    uchar  last_id;             //!< 07 Last ID used in NNBC

    ushort eth_addr;            //!< 08 P2P address of nearest Ethernet node
    uchar hw_ver;               //!< 0a Hardware version
    uchar eth_up;               //!< 0b Non-zero if Ethernet active

    uchar p2pb_repeats;         //!< 0c Number of times to send out P2PB packets
    uchar p2p_sql;              //!< 0d P2P sequence length (**)
    uchar clk_div;              //!< 0e Clock divisors for system & router bus
    uchar tp_scale;             //!< 0f Scale for router phase timer

    volatile uint64 clock_ms;   //!< 10 Milliseconds since boot
    volatile ushort time_ms;    //!< 18 Milliseconds in second (0..999)
    ushort ltpc_period;         //!< 1a Local Time Phase Control message interval (*10 ms)

    volatile uint unix_time;    //!< 1c Seconds since 1970
    uint tp_timer;              //!< 20 Router time phase timer

    ushort cpu_clk;             //!< 24 CPU clock in MHz
    ushort mem_clk;             //!< 26 SDRAM clock in MHz

    uchar forward;              //!< 28 NNBC forward parameter
    uchar retry;                //!< 29 NNBC retry parameter
    uchar peek_time;            //!< 2a Timeout for link read/write (us)
    uchar led_period;           //!< 2b LED flash period (* 10 ms)

    uchar netinit_bc_wait;      //!< 2c Minimum time after last BC during netinit (*10 ms)
    uchar netinit_phase;        //!< 2d Phase of boot process
    ushort p2p_root;            //!< 2e The P2P address from which the system was booted

    uint led0;                  //!< 30 LED definition words (for up
    uint led1;                  //!< 34 to 15 LEDs)
    int clock_drift;            //!< 38 drift of clock from boot chip clock in ticks / us
                                //      NOTE: This is a fixed point number!!!
    uint random;                //!< 3c Random number seed

    uchar root_chip;            //!< 40 Set if we are the root chip
    uchar num_buf;              //!< 41 Number of SHM buffers
    uchar boot_delay;           //!< 42 Delay between boot NN pkts (us)
    uchar soft_wdog;            //!< 43 Soft watchdog control

    uint sync_alignment;        //!< 44 delay for sync0/1 alignment (us)

    heap_t *sysram_heap;        //!< 48 Heap in SysRAM
    heap_t *sdram_heap;         //!< 4c Heap in SDRAM

    uint iobuf_size;            //!< 50 Size of IO buffers (bytes)
    uint *sdram_bufs;           //!< 54 SDRAM buffers
    uint sysbuf_size;           //!< 58 Size of system buffers (words)
    uint boot_sig;              //!< 5c Boot signature word

    uint mem_ptr;               //!< 60 Memory pointer for NNBC memory setter

    volatile uchar lock;        //!< 64 Lock variable
    uchar link_en;              //!< 65 Bit map of enabled links
    uchar last_biff_id;         //!< 66 Last ID used in BIFF
    uchar bt_flags;             //!< 67 Board Test flags

    mem_block_t shm_root;       //!< 68 Control block for SHM bufs

    uint utmp0;                 //!< 70 Four temps...
    uint utmp1;                 //!< 74
    uint utmp2;                 //!< 78
    uint utmp3;                 //!< 7c

    uchar status_map[20];       //!< 80 Set during SC&MP ROM boot
    uchar p2v_map[20];          //!< 94 Phys to Virt core map
    uchar v2p_map[20];          //!< a8 Virt to Phys core map

    uchar num_cpus;             //!< bc Number of good cores
    uchar rom_cpus;             //!< bd SC&MP ROM good cores
    ushort board_addr;          //!< be Position of chip on PCB

    uint *sdram_base;           //!< c0 Base of user SDRAM
    uint *sysram_base;          //!< c4 Base of user SysRAM
    uint *sdram_sys;            //!< c8 System SDRAM
    vcpu_t *vcpu_base;          //!< cc Start of VCPU blocks

    heap_t *sys_heap;           //!< d0 System heap in SDRAM
    rtr_entry_t *rtr_copy;      //!< d4 Copy of router MC tables
    uint *hop_table;            //!< d8 P2P hop table
    block_t** alloc_tag;        //!< dc Start of alloc_tag table
    ushort rtr_free;            //!< e0 Start of free router entry list
    ushort p2p_active;          //!< e2 Count of active P2P addresses
    app_data_t *app_data;       //!< e4 Array of app_id structs
    sdp_msg_t *shm_buf;         //!< e8 SHM buffers
    uint mbox_flags;            //!< ec AP->MP communication flags

    uchar ip_addr[4];           //!< f0 IP address (or 0)
    uint fr_copy;               //!< f4 (Virtual) copy of router FR reg
    uint *board_info;           //!< f8 Pointer to board_info area !!
    uint sw_ver;                //!< fc Software version number
} sv_t;


// Pointers to useful bits of system RAM

/*!
"sv" points to SV struct at top of sys RAM
*/

static sv_t*   const sv         = (sv_t *)   SV_SV;



/********************************************************/
// spin1_api_params.h


#define BARRIER_GO_WAIT       50000
#define BARRIER_RDY2_WAIT     25000
#define BARRIER_RDYGO_WAIT    25000
#define BARRIER_LOCK_WAIT     50000
#define BARRIER_RESEND_WAIT   100

#define API_PRINT_DLY 200
#define ROUTER_INIT_WAIT 5000



#define TAG_NONE                255     //!< Invalid tag/transient request

enum scamp_iptag_commands {
    IPTAG_NEW = 0,      //!< Allocate and configure IPTag
    IPTAG_SET = 1,      //!< Configure existing IPTag
    IPTAG_GET = 2,      //!< Read IPTag status
    IPTAG_CLR = 3,      //!< Deallocate all IPTags
    IPTAG_TTO = 4,       //!< Set IPTag timeout (common)

    IPTAG_MAX = 4
};


enum sark_scp_command_codes {
    CMD_VER = 0,                //!< Return version/core info
    CMD_RUN = 1,                //!< Run at PC (Deprecated)
    CMD_READ = 2,               //!< Read memory
    CMD_WRITE = 3,              //!< Write memory
    CMD_APLX = 4,               //!< Run via APLX (Deprecated)
    CMD_FILL = 5,               //!< Fill memory

    // Following for monitors only
    CMD_COUNT = 15,             //!< Count state of application processors
    CMD_REMAP = 16,             //!< Remap application core
    CMD_LINK_READ = 17,         //!< Read neighbour memory
    CMD_LINK_WRITE = 18,        //!< Write neighbour memory
    CMD_AR = 19,                //!< Application core reset

    CMD_NNP = 20,               //!< Send broadcast NN packet
    CMD_APP_COPY_RUN = 21,      //!< Copy app from adjacent chip and reset
    CMD_SIG = 22,               //!< Send signal to apps
    CMD_FFD = 23,               //!< Send flood-fill data

    CMD_AS = 24,                //!< Application core APLX start
    CMD_LED = 25,               //!< Control LEDs
    CMD_IPTAG = 26,             //!< Configure IPTags
    CMD_SROM = 27,              //!< Read/write/erase serial ROM

    CMD_ALLOC = 28,             //!< Memory allocation
    CMD_RTR = 29,               //!< Router control
    CMD_RSVD = 30,              //!< Reserved (used elsewhere in software)
    CMD_INFO = 31,              //!< Get chip/core info
    CMD_SYNC = 32,              //!< Control sending of synchronization msgs

    CMD_P2PC = 33,              // Just for simulator, no bmp
    
    // 48-63 reserved for BMP

    CMD_TUBE = 64               //!< Tube output
};

//! SCP return codes
enum sark_scp_return_codes {
    RC_OK = 0x80,               //!< Command completed OK
    RC_LEN = 0x81,              //!< Bad packet length
    RC_SUM = 0x82,              //!< Bad checksum
    RC_CMD = 0x83,              //!< Bad/invalid command
    RC_ARG = 0x84,              //!< Invalid arguments
    RC_PORT = 0x85,             //!< Bad port number
    RC_TIMEOUT = 0x86,          //!< Timeout
    RC_ROUTE = 0x87,            //!< No P2P route
    RC_CPU = 0x88,              //!< Bad CPU number
    RC_DEAD = 0x89,             //!< SHM dest dead
    RC_BUF = 0x8a,              //!< No free SHM buffers
    RC_P2P_NOREPLY = 0x8b,      //!< No reply to open
    RC_P2P_REJECT = 0x8c,       //!< Open rejected
    RC_P2P_BUSY = 0x8d,         //!< Dest busy
    RC_P2P_TIMEOUT = 0x8e,      //!< Dest died?
    RC_PKT_TX = 0x8f,           //!< Pkt Tx failed
};


//! IPTag entry (24 bytes)
typedef struct {
    uint8_t ip[4];      //!< IP address of target
    uint8_t mac[6];     //!< MAC address of target
    uint16_t port;      //!< UDP port of target
    uint16_t timeout;   //!< Timeout (in 10ms ticks)
    uint16_t flags;     //!< Flags (::bmp_iptag_flags)
    uint32_t count;     //!< Count of messages sent via IPTag
    uint32_t _PAD;
} iptag_t;

typedef void (*callback_t) (uint, uint);

#define GO_KEY                (uint) ((0x1ffff << 11) | 0)
#define RDYGO_KEY             (uint) ((0x1ffff << 11) | 1)
#define RDY1_KEY              (uint) ((0x1ffff << 11) | 2)
#define RDY2_KEY              (uint) ((0x1ffff << 11) | 3)
#define BARRIER_MASK (uint)(0xffffffff)

#define CLEAR_LCK             0
#define RTR_INIT_LCK          0xad
#define RDYGO_LCK             0xbe


#define MC_TABLE_SIZE 1024

enum spin1_api_multicast_entries {
    SYS_MC_ENTRIES = 1,
    APP_MC_ENTRIES = (MC_TABLE_SIZE - SYS_MC_ENTRIES)
};

//! link orientation codes
enum spin1_api_link_orientations {
    EAST =                0,
    NORTH_EAST =          1,
    NORTH =               2,
    WEST =                3,
    SOUTH_WEST =          4,
    SOUTH =               5
};

//! internal error/warning return codes
enum spin1_api_error_codes {
    NO_ERROR =            0,
    TASK_QUEUE_FULL =     1,
    DMA_QUEUE_FULL =      2,
    PACKET_QUEUE_FULL =   4,
    WRITE_BUFFER_ERROR =  8,
    SYNCHRO_ERROR =      16
};

//! interrupt service routine
typedef void (*isr_t) (void);

// ------------------------------------------------------------------------
// event definitions
// ------------------------------------------------------------------------
//! event-related parameters
enum {
    NUM_EVENTS =           8, //!< Count of possible events
    MC_PACKET_RECEIVED =   0, //!< Multicast packet received
    DMA_TRANSFER_DONE =    1, //!< DMA transfer complete
    TIMER_TICK =           2, //!< Regular timer tick
    SDP_PACKET_RX =        3, //!< SDP message received
    USER_EVENT =           4, //!< User-triggered interrupt
    MCPL_PACKET_RECEIVED = 5, //!< Multicast packet with payload received
    FR_PACKET_RECEIVED =   6, //!< Fixed route packet received
    FRPL_PACKET_RECEIVED = 7  //!< Fixed route packet with payload received
};

// -----------------------
/* scheduler/dispatcher */
// -----------------------

// -----------------------
/* scheduler/dispatcher */
// -----------------------
//! callback queue parameters
enum spin1_api_callback_queue_params {
    N_TASK_QUEUES = 4,    //!< Number of priorities - 1 because priority 0 is
                          //!< not queued
    NUM_PRIORITIES =  5,  //!< Number of priorities
    TASK_QUEUE_SIZE = 16  //!< Size of task queue
};


//! An external interrupt handler
typedef struct {
    callback_t cback;           //!< Pointer to the function to call
    int priority;               //!< The interrupt priority
} cback_t;

//! An internal interrupt/callback handler
typedef struct {
    callback_t cback;           //!< Pointer to the function to call
    uint arg0;                  //!< The first arbitrary parameter
    uint arg1;                  //!< The second arbitrary parameter
} task_t;

//! \brief The queue of callbacks to do.
//! \details Implemented as a circular buffer
typedef struct {
    uint start;                 //!< Index of first task
    uint end;                   //!< Index of last task
    //! Array holding task descriptors
    task_t queue[TASK_QUEUE_SIZE];
} task_queue_t;



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



extern uint spin1_send_mc_packet(uint key, uint data, uint load);

extern uint spin1_send_sdp_msg (sdp_msg_t *msg, uint timeout);

extern uint  spin1_get_core_id(void);

extern uint  spin1_get_chip_id(void);

// ---------------------
/* rts initialization */
// ---------------------
extern void rts_init (void);
extern void rts_cleanup (void);
// ---------------------


extern uint spin1_start(void);
extern void spin1_stop(void);
extern void spin1_kill(uint error);
extern void spin1_set_timer_tick(uint time);

extern void spin1_set_core_map(uint chips, uint * core_map);
extern uint spin1_get_simulation_time(void);

extern void spin1_delay_us (uint n);


extern uint p2p_get (uint entry);

extern void spin1_callback_on(uint event_id, callback_t cback, int priority);
extern void spin1_callback_off(uint event_id);

extern uint spin1_schedule_callback(callback_t cback, uint arg0, uint arg1, uint priority);

extern uint spin1_trigger_user_event(uint arg0, uint arg1);
extern uint spin1_dma_transfer(uint tag, void *system_address, void *tcm_address,
			       uint direction, uint length);

extern void spin1_memcpy(void *dst, void const *src, uint len);

extern uint spin1_send_mc_packet(uint key, uint data, uint load);

extern void spin1_flush_rx_packet_queue(void);

extern void spin1_flush_tx_packet_queue(void);

extern void spin1_msg_free (sdp_msg_t *msg);

extern sdp_msg_t* spin1_msg_get (void);

extern uint spin1_send_sdp_msg (sdp_msg_t *msg, uint timeout);

extern uint spin1_irq_disable(void);

extern uint spin1_fiq_disable(void);

extern uint spin1_fiq_disable(void);

extern uint spin1_int_disable(void);

extern void spin1_mode_restore(uint sr);

extern uint  spin1_get_id(void);

extern uint  spin1_get_core_id(void);

extern uint  spin1_get_chip_id(void);

extern uchar spin1_get_leds(void);

extern void  spin1_set_leds(uchar leds);

extern uint spin1_set_mc_table_entry(uint entry, uint key, uint mask, uint route);

extern void* spin1_malloc(uint bytes);

// ----------------
/* data transfer */
// ----------------
//! DMA transfer parameters: <b>16</b>-doubleword bursts
//TODO: may need adjustment for SpiNNaker
#define DMA_BURST_SIZE        4
//! DMA transfer parameters: 16-<b>doubleword</b> bursts
#define DMA_WIDTH             1
//! internal DMA queue size
#define DMA_QUEUE_SIZE        16
//! select write buffer use
#define USE_WRITE_BUFFER      FALSE


// ----------------
/* data transfer */
// ----------------
//! Describes a DMA transfer
typedef struct {
    uint id;                    //!< ID
    uint tag;                   //!< User label
    uint* system_address;       //!< Address in SDRAM (or other shared memory)
    uint* tcm_address;          //!< Address in local TCM
    uint description;           //!< Control descriptor
} copy_t;


//! \brief The queue of DMA transfers.
//! \details Implemented as a circular buffer
typedef struct {
    uint start;                 //!< Index of first transfer
    uint end;                   //!< Index of last transfer
    //! Array holding transfer descriptors
    copy_t queue[DMA_QUEUE_SIZE];
} dma_queue_t;



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
extern void *spin_emu_controller(void *arg);
extern void spin_emu_send_rr_ping(int chip, int core);
extern void spin_emu_send_mc(int fd, struct spin_emu_mc_t *mc);
extern void spin_emu_send_route(int fd, uint entry, uint key, uint mark, uint route);
extern void spin_emu_die(const char *msg, ...);
extern void spin_emu_die_errno(const char *msg, ...);
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
