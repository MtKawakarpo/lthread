#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include "../includes/nf_common.h"
#include "../includes/firewall.h"

static inline void
firewall_fill_ipv4_5tuple_key(struct ipv4_5tuple *key, void *ipv4_hdr) {
    struct tcp_hdr *tcp_hdr;
    struct udp_hdr *udp_hdr;

    memset(key, 0, sizeof(struct ipv4_5tuple));
    key->proto  = ((struct ipv4_hdr *)ipv4_hdr)->next_proto_id;
    key->ip_src = ((struct ipv4_hdr *)ipv4_hdr)->src_addr;
    key->ip_dst = ((struct ipv4_hdr *)ipv4_hdr)->dst_addr;

    if (key->proto == IP_PROTOCOL_TCP) {
        tcp_hdr = (struct tcp_hdr *)((uint8_t*)ipv4_hdr + sizeof(struct ipv4_hdr));
        key->port_src = rte_be_to_cpu_16(tcp_hdr->src_port);
        key->port_dst = rte_be_to_cpu_16(tcp_hdr->dst_port);
    } else if (key->proto == IP_PROTOCOL_UDP) {
        udp_hdr = (struct udp_hdr *)((uint8_t*)ipv4_hdr + sizeof(struct ipv4_hdr));
        key->port_src = rte_be_to_cpu_16(udp_hdr->src_port);
        key->port_dst = rte_be_to_cpu_16(udp_hdr->dst_port);
    } else {
        key->port_src = 0;
        key->port_dst = 0;
    }
}

/* calc hash value */
static uint32_t
nf_hash_val(struct ipv4_5tuple* tmp_turple) {
    uint64_t ret = 0;
    ret = tmp_turple->ip_dst % BIG_PRIME;
    ret = (ret * shift_32 + tmp_turple->ip_src) % BIG_PRIME;
    ret = (ret * shift_16 + tmp_turple->port_dst) % BIG_PRIME;
    ret = (ret * shift_16 + tmp_turple->port_src) % BIG_PRIME;
    ret = (ret * shift_8 + tmp_turple->proto) % BIG_PRIME;
    return (uint32_t)ret;
}

/* hash set insert */
static void
nf_hash_insert(struct nf_statistics* stats, struct ipv4_firewall_hash_entry* entry) {

    uint32_t index = nf_hash_val(&entry->key);
    struct hash_node* ptr = &(stats->hash_map[index]);
    ptr->is_valid = 1;
    ptr->ip_src = entry->key.ip_src;
    ptr->ip_dst = entry->key.ip_dst;
    ptr->port_src = entry->key.port_src;
    ptr->port_dst = entry->key.port_dst;
    ptr->proto = entry->key.proto;
    ptr->action = entry->action;
}

/* hash set find */
static int
nf_hash_lookup(struct nf_statistics* stats, struct ipv4_5tuple* key) {
    uint32_t index = nf_hash_val(key);
    bool found = false;
    uint8_t action;
    struct hash_node ptr = stats->hash_map[index];

    if (ptr.ip_src == key->ip_src && ptr.ip_dst == key->ip_dst && \
    ptr.port_src == key->port_src && ptr.port_dst == key->port_dst && \
    ptr.proto == key->proto && ptr.is_valid) {
        action = ptr.action;
        found = true;
    }

    // logic here could be modified
    if (found) {
        return action;
    }
    else {
        return PASS;
    }
}


/* init tenant state */
void
nf_firewall_init(struct nf_statistics *stats) {

    uint32_t i;
//    printf("Entering firewall init\n");
    for (i = 0; i < BIG_PRIME; i ++) {
//        printf("init index %d\n", i);
        stats->hash_map[i].is_valid = 0;
    }

    for (i = 0; i < rule_number; i ++) {
//        printf("insert index %d\n", i);
        nf_hash_insert(stats, &ipv4_firewall_hash_entry_array[i]);
    }
}

/* handle tenant packets */
uint16_t
nf_firewall_handler(struct rte_mbuf *pkt[], uint16_t num, struct nf_statistics* stats) {

    struct ipv4_hdr* ipv4hdr;
    struct ipv4_5tuple key;
    uint16_t i, num_out;
    uint8_t ret;

    num_out = 0;
    for (i = 0; i < num; i++) {
        ipv4hdr = nf_pkt_ipv4_hdr(pkt[i]);
        firewall_fill_ipv4_5tuple_key(&key, ipv4hdr);
        ret = nf_hash_lookup(stats, &key);

        /* drop packets */
//        if (ret == PASS) {
//            pkt[num_out] = pkt[i];
//            num_out ++;
//        } else {
//            rte_pktmbuf_free(pkt[i]);
//        }
    }

    return num_out;
}

/* Set *hi and *lo to the high and low order bits of the cycle counter.
 *    Implementation requires assembly code to use the rdtsc instruction. */
static uint64_t rdtsc_2(void)
{
    uint64_t var;
    uint32_t hi, lo;

    __asm volatile
    ("rdtsc" : "=a" (lo), "=d" (hi));

    var = ((uint64_t)hi << 32) | lo;
    return (var);
}

int
lthread_firewall(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;
    struct nf_statistics *statistics = nf_info_local->state;

    lthread_set_data((void *)nf_info_local);


    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
    nf_firewall_init(statistics);
    int tx_yield_cnt = 0;
    int rx_yield_cnt = 0;
    int cnt = 0;
    struct timespec time_start={0, 0},time_end={0, 0};
    clock_gettime(CLOCK_REALTIME, &time_start);
    uint64_t start, end, cycle, cycle_poll, last_start;

    start = rdtsc_2();
    while (1){


        nb_rx = rte_ring_sc_dequeue_bulk(rq, pkts, BURST_SIZE, NULL);

        if(unlikely(nb_rx == 0)){
            end = rdtsc_2();
            cycle_poll = end - start;
            lthread_yield_with_cycle(0,cycle_poll );
            start = rdtsc_2();
        }
        else {

            start = rdtsc_2();

            nf_firewall_handler(pkts, nb_rx, statistics);
            nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;

                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];

                    rte_pktmbuf_free(m);
                }
            }
            end = rdtsc_2();
            cycle = end - start;
            lthread_yield_with_cycle(cycle, cycle_poll);

        }
    }

}
