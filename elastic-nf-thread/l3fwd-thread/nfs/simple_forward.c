//
// Created by zzl on 2018/10/26.
//

#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include "nf_common.h"


int
lthread_forwarder(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;

    lthread_set_data((void *)nf_info_local);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
//	rte_delay_ms(1000);
    for (; keep_running;) {
//    while(1){
        nb_rx = nf_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {
//            printf("nf %d recv %d pkts\n", nf_id, nb_rx);
            //do somthing
            nb_tx = nf_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("nf %d suc transfer %d pkts\n", nf_id, nb_tx);

        }else {
            continue;
        }
    }
    return 0;

}
