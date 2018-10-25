/*
* Created by Zhilong Zheng
*/

#ifndef ELASTIC_FLOW_DISTRIBUTER_H
#define ELASTIC_FLOW_DISTRIBUTER_H

#include <stdio.h>
#include <stdint.h>


#define MAX_NF_NB  64
#define MAX_TABLE_ENTRIES (1417)

struct port_info{
    uint8_t port_id;
    uint8_t queue_id;
};

struct flow {
    uint32_t flow_id;
    uint32_t flow_hash;  // We use flow hashing value to identify a flow
    uint32_t nf_id;  // assign to an nf instance
};

struct table_entry {
    uint32_t packet_hash;
    struct flow *m_flow;
};

struct flow_table {
    struct table_entry *entries[MAX_TABLE_ENTRIES];
    uint32_t nb_entries;

};

extern struct flow *flows;
extern int flow_count;
extern struct flow_table *flow_table;
struct rte_ring *nf_rxring_mapping[MAX_NF_NB];

void flows_init(uint32_t nb_flows);
struct flow* add_flow(int flow_hash, int nf_id);
void flow_table_init();
void flow_table_add_entry(uint32_t hash, int nf_id);
struct flow *flow_table_get_flow(uint32_t hash);

int flow_director_thread(struct port_info *args);


#endif //ELASTIC_FLOW_DISTRIBUTER_H
