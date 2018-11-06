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


int
pthread_firewall(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;
    struct nf_statistics *statistics = nf_info_local->state;


    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", nf_info_local->lcore_id, nf_id);
    nf_firewall_init(statistics);
    int queue_full_cnt = 1;
    long long iteration_cnt = 0;
    int print_cnt = 3000000;
    int pkt_cnt = 0;
//    uint64_t start, end, cycle;


    while (1){

//        iteration_cnt++;
//        if(iteration_cnt%print_cnt == 0){
////            printf("nf %d : iteration = %lld, queue_full = %d\n",nf_id, iteration_cnt, queue_full_cnt);
////            iteration_cnt = 0;
////            queue_full_cnt = 0;
//        }
        nb_rx = rte_ring_sc_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
//        printf("NF: %d is here with packets :%d\n", nf_id, nb_rx);
        if (unlikely(nb_rx > 0)) {
//            start = rdtsc();

            pkt_cnt += nb_rx;
//
//            if (pkt_cnt % 10000 == 0) {
//                printf("NF %d recv %d pkts\n", nf_id, pkt_cnt);
//            }
            nf_firewall_handler(pkts, nb_rx, statistics);
//            end = rdtsc();
//            cycle = end - start;
//            printf("cycle: %d\n", cycle);
            nb_tx = rte_ring_sp_enqueue_burst(tq, pkts, nb_rx, NULL);

            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;
                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];

                    rte_pktmbuf_free(m);
                }

                queue_full_cnt += (nb_rx - nb_tx);
            }
//
//            if (queue_full_cnt % 256 == 0) {
//                printf("NF %d queue full with %d\n", nf_id, queue_full_cnt);
//            }

        }else{
        }
//        sched_yield();

    }
    return 0;

}
