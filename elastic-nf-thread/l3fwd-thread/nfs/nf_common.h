//
// Created by zzl on 2018/10/26.
//

#ifndef LTHREAD_NF_COMMON_H
#define LTHREAD_NF_COMMON_H

#endif //LTHREAD_NF_COMMON_H
#define BURST_SIZE 32

typedef void (*lthread_func_t) (void *);

struct nf_info{

    lthread_func_t fun;
    struct rte_ring *rx_q;
    struct rte_rinf *tx_q;
    int nf_id;
    int agent_id;
    int priority;
    uint16_t lcore_id;

};
struct nf_thread_info{
    int nf_id;
};
static uint8_t keep_running = 1;
struct nf_info *nfs_info_data;
static void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        keep_running = 0;
}
