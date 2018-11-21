#define _GNU_SOURCE
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <sys/time.h>
#include <stdlib.h>

#include "flow_distributer.h"
#include "nfs/includes/nf_common.h"
#include "nfs/includes/simple_forward.h"
#include "nfs/includes/firewall.h"
#include "nfs/includes/aes_encrypt.h"
#include "nfs/includes/aes_decrypt.h"

#define __USE_GNU
#include <sched.h>
#include <pthread.h>

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192 * 128
#define MBUF_SIZE (1600 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MBUF_CACHE_SIZE 0
#define NO_FLAGS 0
#define RING_MAX 1  // 1个ring
#define NF_NAME_RX_RING "NF_%u_rq"
#define NF_NAME_TX_RING "NF_%u_tq"

#define NUM_PORTS 1
#define MAX_NUM_PORT 4
#define MAX_LCORE_NUM 24
#define MAX_NF_NUM 1000
#define MAX_AGENT_NUM 5
#define NF_QUEUE_RING_SIZE (256*32)

pthread_t pthreads[MAX_NF_NUM];
pthread_attr_t pthread_attrs[MAX_NF_NUM];
struct sched_param sches[MAX_NF_NUM];

/* configuartion */

//#define SFC_CHAIN_LEN 3
#define MONITOR_PERIOD 30  // 3秒钟更新 一次 monitor的信息

uint16_t nb_nfs = 1; //修改时必须更新nf_func_config, service_time_config, priority_config, start_sfc_config, flow_ip_table
int rx_exclusive_lcore[RING_MAX] = {2, 4,6,8};//server 39
//int rx_exclusive_lcore[RING_MAX] = {1,2,3,4,5};//server 33
// 根据不同机器来制定, 0预留给core manager
int tx_exclusive_lcore[RING_MAX] = {10,12,14,16};//server 39
//int tx_exclusive_lcore[RING_MAX] = {6,7,8,9,10};//server 33
lthread_func_t nf_fnuc_config[MAX_NF_NUM]={pthread_firewall, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,
                                           pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt, pthread_aes_decryt,};//NF函数，在nfs头文件里面定义
int start_sfc_config_flag[MAX_NF_NUM]={ 0,0 ,0,0, 0,0,0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0,};
uint64_t flow_ip_table[MAX_NF_NUM]={
        16820416, 33597632,50374848, 67152064, 83929280, 100706496, 117483712, 134260928,
        151038144, 167815360, 184592576, 201369792, 218147008, 234924224, 251701440, 268478656,
        285255872,302033088, 318810304, 335587520, 352364736, 369141952, 385919168, 402696384,
        419473600, 436250816,453028032,  469805248, 486582464, 503359680, 520136896, 536914112,
        553691328, };
int nf_tx_port[MAX_NF_NUM] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0,}; // 指定NF应该output到哪个端口

/* variable */
struct nf_thread_info *nf[ MAX_NF_NUM];
static uint8_t port_id_list[NUM_PORTS] = {0, 1};
static struct rte_mempool *pktmbuf_pool;
static uint8_t keep_running = 1;
uint16_t nb_lcores = 0;

static const struct rte_eth_conf port_conf_default = {
        .rxmode = {
                .mq_mode = ETH_MQ_RX_RSS,
                .max_rx_pkt_len = ETHER_MAX_LEN,
        },
        .rx_adv_conf = {
                .rss_conf = {
                        .rss_key = NULL,
                        .rss_hf = ETH_RSS_TCP,
                },
        },
        .txmode = {
                .mq_mode = ETH_MQ_TX_NONE,
        },
};

void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        keep_running = 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static int port_init(uint8_t port) {
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = RING_MAX;
    const uint16_t tx_rings = RING_MAX;

    int retval;
    uint16_t q;

    if (port >= rte_eth_dev_count())
        return -1;

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
                                        rte_eth_dev_socket_id(port), NULL, pktmbuf_pool);
        if (retval < 0)
            return retval;
    }

    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
                                        rte_eth_dev_socket_id(port), NULL);
        if (retval < 0)
            return retval;
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    /* Display the port MAC address. */
    struct ether_addr addr;
    rte_eth_macaddr_get(port, &addr);
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
    " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            (unsigned)port,
            addr.addr_bytes[0], addr.addr_bytes[1],
            addr.addr_bytes[2], addr.addr_bytes[3],
            addr.addr_bytes[4], addr.addr_bytes[5]);

    /* Enable RX in promiscuous mode for the Ethernet device. */
    printf("try to enable port %d\n", port);
    rte_eth_promiscuous_enable(port);
    printf(">success\n");
    struct rte_eth_link  link;
    int cnt = 0;
    do{
        rte_eth_link_get(port, &link);
        cnt++;
        printf("try get link %d\n", cnt);
        if(cnt==2)
            break;
    }while(link.link_status != ETH_LINK_UP);
    if(link.link_status == ETH_LINK_DOWN){
//        rte_exit(EXIT_FAILURE, ":: error: link is still down\n");
        printf("link down with port %d\n", port);
        return 0;
    }
//    rte_delay_ms(1);
    printf("finish init\n");
    return 0;
}

static int
init_mbuf_pools(void) {

    const unsigned num_mbufs = NUM_MBUFS * NUM_PORTS;

    /* don't pass single-producer/single-consumer flags to mbuf create as it
     * seems faster to use a cache instead */
    printf("Creating mbuf pool '%s' [%u mbufs] ...\n",
           "MBUF_POOL", num_mbufs);
    pktmbuf_pool = rte_mempool_create("MBUF_POOL", num_mbufs,
                                      MBUF_SIZE, MBUF_CACHE_SIZE,
                                      sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init,
                                      NULL, rte_pktmbuf_init, NULL, rte_socket_id(), NO_FLAGS);

    return (pktmbuf_pool == NULL); /* 0  on success */
}

static inline const char * get_nf_rq_name(int i){
    static char buffer[sizeof(NF_NAME_RX_RING) + 10];
    snprintf(buffer, sizeof(buffer)-1, NF_NAME_RX_RING, i);
    return buffer;
}
char *get_nf_tq_name(int i){
    static char buffer[sizeof(NF_NAME_TX_RING)+10];
    snprintf(buffer, sizeof(buffer)-1, NF_NAME_TX_RING, i);
    return buffer;

}

static int
lthread_nf(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx;
    struct ipv4_hdr *ipv4_hdr;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
    rte_delay_ms(1000);
    for (; keep_running;) {
//        printf("nf %d try to dequeue pkts\n", nf_id);
        nb_rx = rte_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
//        printf("suc\n");
        if (likely(nb_rx > 0)) {
            //do somthing
            nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("nf %d suc transfer %d pkts\n", nf_id, nb_tx);

            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;
                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];
                    rte_pktmbuf_free(m);
                }
            }

        }else {
            continue;
        }
    }
    return 0;

}

const char *sched_policy[] = {
        "SCHED_OTHER",
        "SCHED_FIFO",
        "SCHED_RR",
        "SCHED_BATCH"
};

int main(int argc, char *argv[]) {


    int ret, i, j;
    uint8_t total_ports, cur_lcore;
    int start_sfc = 0;
    int port_nb = 1;

    errno = 0;

    cpu_set_t pthread_cpu_info;

    struct timespec tv;

//    struct sched_param sp = {
////            .sched_priority = 1//SCHED_RR
//            .sched_priority = 0//SCHED_BATCH
//    };
//    ret = sched_setscheduler(0, SCHED_BATCH, &sp);
//    printf("return = %d\n", ret);
//    if (errno) {
//        printf("errno = %d\n", errno); // errno = 33
//        printf("error: %s\n", strerror(errno)); // error: Numerical argument out of domain
//    }
//    printf("Scheduler Policy now is %s.\n", sched_policy[sched_getscheduler(0)]);
//    ret = sched_rr_get_interval(0, &tv);
//    if(ret == 0){
//        printf("time quota : %9lu.%9lu\n", tv.tv_sec, tv.tv_nsec);
//    }



    int socket_id = rte_socket_id();
    const char *rq_name;
    const char *tq_name;

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    total_ports = rte_eth_dev_count();

    printf("available ports: %d\n", total_ports);

    cur_lcore = rte_lcore_id();
    if (total_ports < 1)
        rte_exit(EXIT_FAILURE, "ports is not enough\n");
    ret = init_mbuf_pools();
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "Cannot create needed mbuf pools\n");

    for(i = 0;i<port_nb;i++){
        ret = port_init(port_id_list[i]);
        if (ret != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %u\n",port_id_list[i]);
    }
    printf(">>>try to allocate nf info data\n");
    nfs_info_data = rte_calloc("nfs info",
                               MAX_NF_NUM, sizeof(*nfs_info_data), 0);
    if (nfs_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for nf  details\n");
    printf(">>>suc allocate mem\n");

    // 分配一个核给 flow distributor rx thread
    flow_table_init();
    for (i = 0; i < port_nb*RING_MAX; ++i) {
        struct port_info *port_info1 = calloc(1, sizeof(struct port_info));
//        port_info1->port_id = i;
        port_info1->port_id = 0;
        port_info1->queue_id = i;
        port_info1->thread_id = i;
        port_info1->nb_ports = port_nb*RING_MAX;
        cur_lcore = rx_exclusive_lcore[i];
        if (rte_eal_remote_launch(flow_director_rx_thread, (void *) port_info1, cur_lcore) == -EBUSY) {
            printf("Core %d is already busy, can't use for RX of flow director \n", cur_lcore);
            return -1;
        }
    }

    /* init nf info */
    for(i = 0;i<nb_nfs;i++){
        rq_name = get_nf_rq_name(i);
        tq_name = get_nf_tq_name(i);
        nfs_info_data[i].rx_q = rte_ring_create(rq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SC_DEQ | RING_F_SP_ENQ);
        nfs_info_data[i].tx_q = rte_ring_create(tq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SC_DEQ | RING_F_SP_ENQ);
        if(nfs_info_data[i].rx_q == NULL || nfs_info_data[i].tx_q == NULL){
            rte_exit(EXIT_FAILURE, "cannot create ring for nf %d\n", i);
        }
        nfs_info_data[i].nf_id = i;
        nfs_info_data[i].fun = nf_fnuc_config[i];
        nfs_info_data[i].state = rte_calloc("nf state", MAX_STATE_LEN, sizeof(*nfs_info_data[i].state), 0);
        nfs_info_data[i].state->hash_map = rte_calloc("nf hash map", BIG_PRIME, sizeof(*nfs_info_data[i].state->hash_map), 0);
//        nfs_info_data[i].state->key_schedule = rte_calloc("nf key schedule", AES_ENCRYPT_LEN, sizeof(WORD), 0);

        printf(">>>suc to allocate nf state\n");
        if(start_sfc_config_flag[i] == 0){
            printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[i], i);
            flow_table_add_entry(flow_ip_table[i], i); // flow hash (这里使用的是flow的源IP值), nf_id
            bind_nf_to_rxring(i, nfs_info_data[i].rx_q);
            printf("register nf %d tx ring\n", i);
            nf_need_output(i, nf_tx_port[i], nfs_info_data[i].tx_q);
            if(start_sfc == 1){
                start_sfc = 0;
                printf("register nf %d tx ring\n", i-1);
                nf_need_output(i, nf_tx_port[i-1], nfs_info_data[i-1].tx_q);  // 参数为 nf_id, 要往哪个port 上 发数据, nf_id 对应的tx_ring
            }
            //FIXME:for test
//            if(i == 0) {
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs], i); // flow hash (这里使用的是flow的源IP值), nf_id
//            }
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs+1], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs+1], i); // flow hash (这里使用的是flow的源IP值), nf_id
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs+2], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs+2], i); // flow hash (这里使用的是flow的源IP值), nf_id

//            }else if (i == 1){
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs+3], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs+3], i); // flow hash (这里使用的是flow的源IP值), nf_id
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs+4], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs+4], i); // flow hash (这里使用的是flow的源IP值), nf_id
//            }else if( i == 2){
//                printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[nb_nfs+5], i);
//                flow_table_add_entry(flow_ip_table[nb_nfs+5], i); // flow hash (这里使用的是flow的源IP值), nf_id
//            }
        }else{
            if(start_sfc == 0){
                start_sfc = 1;//first nf of sfc
                printf(">>>add flow entry %d-->chain head nf %d\n", flow_ip_table[i], i);
                flow_table_add_entry(flow_ip_table[i], i); // flow hash (这里使用的是flow的源IP值), nf_id
                bind_nf_to_rxring(i, nfs_info_data[i].rx_q);
                //绑定下一跳的rx
            }else{
                //覆盖上一跳NF的tx ring
                nfs_info_data[i-1].tx_q = nfs_info_data[i].rx_q;
                if(i == nb_nfs -1){
                    printf("register nf %d tx ring\n", i);
                    nf_need_output(i, nf_tx_port[i], nfs_info_data[i].tx_q);
                }
            }
        }
        nf[i] = calloc(1, sizeof(struct nf_thread_info));
        nf[i]->nf_id = i;

    }
    printf("finish load nfs info\n");
    cur_lcore = 12;

    /*for(i = 0;i<nb_nfs;i++){
//        cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);

        printf("launching nf thread %d\n", i);
        if(rte_eal_remote_launch(nfs_info_data[i].fun, (void *)nf[i], cur_lcore) == -EBUSY){
            printf("core %d cannot use for nf %d\n", cur_lcore, i);
            return -1;
        }
        cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);

    }*/
    //TODO:使用原生pthread有个bug，程序不能通过ctrl+c退出，使用原生的和使用eal的性能一样，但是原生的可以测上下文切换时间
    // 使用pthread原生api
    CPU_ZERO(&pthread_cpu_info);
    CPU_SET(cur_lcore, &pthread_cpu_info);
    for (i = 0; i < nb_nfs; ++i) {
        pthread_attr_init(&pthread_attrs[i]);

        // 设置优先级
        if (1) {
            ret = pthread_attr_setschedpolicy(&pthread_attrs[i], SCHED_RR);
            if (ret != 0) {
                printf("Pthread policy failed for NF %d\n", i);

            }

            pthread_attr_getschedparam(&pthread_attrs[i], &sches[i]);

//            sches[i].sched_priority = -10;

            pthread_attr_setschedparam(&pthread_attrs[i], &sches[i]);
        }


        nfs_info_data[i].lcore_id = cur_lcore;
        if (pthread_create(&pthreads[i], &pthread_attrs[i],
                nfs_info_data[i].fun, (void*)nf[i]) != 0) {
            printf("Pthread creation failed for NF %d\n", i);

            return 2;
        }

        if (pthread_setaffinity_np(pthreads[i], sizeof(cpu_set_t), &pthread_cpu_info) != 0) {
            printf("Pthread affinity setting failed for NF %d\n", i);
        }

    }
    printf("Scheduler Policy now is %s.\n", sched_policy[sched_getscheduler(0)]);
    ret = sched_rr_get_interval(0, &tv);
    if(ret == 0){
        printf("time quota : %9lu.%9lu\n", tv.tv_sec, tv.tv_nsec);
    }
//
//    for(i = 0;i<nb_nfs;i++){
////        cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
//
//        printf("launching nf thread %d\n", i);
//        if(rte_eal_remote_launch(nfs_info_data[i].fun, (void *)nf[i], cur_lcore) == -EBUSY){
//            printf("core %d cannot use for nf %d\n", cur_lcore, i);
//            return -1;
//        }
//        cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
//
//    }


    // 分配一个核给 flow distributor tx thread2
    for (i = 0; i < port_nb; ++i) {
        struct port_info *port_info2 = calloc(1, sizeof(struct port_info));
        port_info2->port_id = 0;
        port_info2->queue_id = i;
        port_info2->thread_id = i;
        port_info2->nb_ports = port_nb;
        cur_lcore = tx_exclusive_lcore[i];
        if (rte_eal_remote_launch(flow_director_tx_thread, (void *) port_info2, cur_lcore) == -EBUSY) {
            printf("Core %d is already busy, can't use for TX of flow director \n", cur_lcore);
            return -1;
        }
    }
    printf("Entering main loop on core %d\n", rte_lcore_id());
    int monitor_tick = 0, nf_id;
    uint64_t processed_pps = 0;
    uint64_t dropped_pps = 0; // nf 0 每秒丢包数
    uint64_t tx_pps = 0;
    double dropped_ratio = 0;

    for (; keep_running;){

        rte_delay_ms(1000 * 3);
//        rte_delay_ms(500);
        monitor_tick++;
//        if (monitor_tick == MONITOR_PERIOD) {
            monitor_update(3);
            monitor_tick = 0;
//        }
        for(nf_id = 0;nf_id<nb_nfs; nf_id++){

            processed_pps = get_processed_pps_with_nf_id(nf_id);  // nf 0 的处理能力
            dropped_pps = get_dropped_pps_with_nf_id(nf_id); // nf 0 每秒丢包数
            dropped_ratio = get_dropped_ratio_with_nf_id(nf_id); // nf 0 这段时间的丢包率
            tx_pps = get_tx_pps_with_nf_id(nf_id);
            printf(">>> NF %d <<< processing pps: %9ld, drop pps: %9ld, drop ratio: %9lf, tx pps: %9ld \n", nf_id, processed_pps,
                   dropped_pps, dropped_ratio, tx_pps);

        }
        printf("---------------------------------------------\n");


    }

    for (i = 0; i < nb_nfs; ++i) {
        pthread_join(pthreads[i], NULL);
    }


//    }
    return 0;
}