#ifndef __DMA_H__
#define __DMA_H__

typedef struct
{
    uint neuron_id;
    uint coarse_stdp_time_constant;
    uint fine_stdp_time_constant;
    uint synapses[];
} synaptic_row_t;

typedef struct
{
    uint core_id;
    uint core_mask;
    synaptic_row_t *synaptic_block;
    uint branch_right;
    uint row_size;         //synaptic weight row length
} synapse_lookup_t;

typedef struct
{
    uint busy;
    uint flip;
    uint row_size_max;
    synaptic_row_t *cache[2];
    synapse_lookup_t *synapse_lookup_address[2];
    ushort row_size[2];
} dma_pipeline_t;

extern dma_pipeline_t dma_pipeline;
extern synapse_lookup_t *synapse_lookup;

void buffer_mc_packet(uint, uint);
void dma_callback(uint, uint);
void feed_dma_pipeline(uint, uint);
void lookup_synapses(uint key);
uint mc_packet_buffer_empty(void);

extern int overflows;

#endif
