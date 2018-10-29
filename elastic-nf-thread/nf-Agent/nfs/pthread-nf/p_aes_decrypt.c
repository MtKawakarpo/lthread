#include "../includes/aes.h"
#include "../includes/aes_decrypt.h"
#include "../includes/nf_common.h"

int
pthread_aes_decryt(void *dumy){

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

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
    nf_aes_decrypt_init(statistics);
    printf("finish init aes encryt\n");

    while (1){

        nb_rx = rte_ring_sc_dequeue_bulk(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {

            nf_aes_decrypt_handler(pkts, nb_rx, statistics);
//            printf("aes decrypt %d suc transfer %d pkts\n", nf_id, nb_tx);
            nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);

            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;
                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];

                    rte_pktmbuf_free(m);
                }
            }
        }
        sched_yield();
    }
    return 0;

}
