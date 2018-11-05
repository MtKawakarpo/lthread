#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include "../includes/nf_common.h"
#include "../includes/simple_forward.h"


int
pthread_forwarder(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;

    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

//    uint64_t start, end, cycle;

    long long queue_full_cnt = 0;
    long long iteration_cnt = 0;
    int print_cnt = 3000000;
    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);

    while (1){
        iteration_cnt++;
        if(iteration_cnt%print_cnt == 0){
            printf("nf %d : iteration = %lld, queue_full = %d\n",nf_id, iteration_cnt, queue_full_cnt);
//            iteration_cnt = 0;
//            queue_full_cnt = 0;
        }
        nb_rx = rte_ring_sc_dequeue_bulk(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {

//            start = rdtsc();
            nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);

            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;
                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];

                    rte_pktmbuf_free(m);
                }
            }
//            end = rdtsc();
//            cycle = end - start;
//            printf("cycle: %d\n", cycle);
            queue_full_cnt++;

        }
        sched_yield();

    }
    return 0;

}
