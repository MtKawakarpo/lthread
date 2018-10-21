//
// Created by zzl on 2018/10/21.
//

#ifndef PERFORMANCE_THREAD_THREAD_MANAGER_H
#define PERFORMANCE_THREAD_THREAD_MANAGER_H

#endif //PERFORMANCE_THREAD_THREAD_MANAGER_H

/*
 * launch NF threads
 */

/*
 * Retrieve a burst of input packets from a receive queue of an Ethernet device
 * like rte_eth_rx_burst
 */
static __rte_always_inline unsigned nf_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
                                                    struct rte_mbuf **rx_bufs, const uint16_t burst_size){

    uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, rx_bufs, burst_size);
    if (unlikely(nb_rx == 0))
        lthread_yield();
    //TODO: 基于阈值
    return nb_rx;
}

/*
 * like rte_eth_tx_burst
 */
static uint16_t nf_eth_tx_burst(uint16_t 	port_id, uint16_t queue_id,
                                         struct rte_mbuf **tx_pkts, uint16_t nb_pkts){
    uint16_t nb_tx = rte_eth_tx_burst(port_id, queue_id, tx_pkts, nb_pkts);
    lthread_yield();
    return nb_tx;

}

/*
 * like rte_ring_dequeue
 */
static __rte_always_inline int nf_ring_dequeue(struct rte_ring *r, void **pkt){
    uint16_t nb_rx = rte_ring_dequeue(rx_ring, pkt);
    if(unlikely(nb_rx == 0))
        lthread_yield();
    return nb_rx;

}

/*
 * like rte_ring_dequeue_bulk
 */
static __rte_always_inline unsigned int nf_ring_dequeue_bulk	(struct rte_ring *rx_ring, void **rx_bufs,
                                                                      unsigned int burst_size, unsigned int *available){
    uint16_t nb_rx = rte_ring_dequeue_bulk(rx_ring, rx_bufs, burst_size, NULL);
    if(unlikely(nb_rx == 0))
        lthread_yield();
    return nb_rx;
}
/*
 * like rte_ring_dequeue_burst
 */
static __rte_always_inline unsigned nf_ring_dequeue_burst(struct rte_ring *rx_ring, void **rx_bufs,
                                                                   unsigned int burst_size, unsigned int *available){
    uint16_t nb_rx = rte_ring_dequeue_burst(rx_ring, rx_bufs, burst_size, NULL);
    if(unlikely(nb_rx == 0)){
        lthread_yield();
    }
    return nb_rx;
}

/*
 * FIXME: if a thread hold a lock and would not unlock until finish enqueue operation,then it will sleep with a lock
 * otherwise we let thread don't unlock depend on enqueue ret
 */
static __rte_always_inline int nf_ring_enqueue(struct rte_ring *tx_ring, void *tx_pkt){
    uint16_t nb_tx = rte_ring_enqueue(tx_ring, tx_pkt);
    lthread_yield();
    return nb_tx;
}

/*
 * like rte_ring_enqueue_burst
 */
static __rte_always_inline unsigned nf_ring_enqueue_burst(struct rte_ring *tx_ring, void *const *obj_table,
                                                                   unsigned int 	burst_size, unsigned int *free_space){
    uint16_t nb_tx = rte_ring_enqueue_burst(tx_ring, obj_table, burst_size, NULL);
    lthread_yield();
    return nb_tx;

}

/*
 * like rte_ring_enqueue_bulk
 */
static __rte_always_inline unsigned int nf_ring_enqueue_bulk	(struct rte_ring *tx_ring, void *const * tx_bufs,
                                                                      unsigned int burst_size, unsigned int *free_space){
    uint16_t nb_tx = rte_ring_enqueue_bulk(tx_ring, tx_bufs, burst_size, NULL);
    lthread_yield();
    return nb_tx;

}