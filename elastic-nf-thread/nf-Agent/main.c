/*
 *  Core Manager
 */
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
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
#include <stdio.h>
#include <stdlib.h>

#include <nf_lthread_api.h>
#include <thread_manager.h>
#include "flow_distributer.h"
//#include "nfs/includes/nf_common.h"
//#include "nfs/includes/simple_forward.h"
//#include "nfs/includes/firewall.h"
//#include "nfs/includes/aes_encrypt.h"
//#include "nfs/includes/aes_decrypt.h"

#include "nf_common.h"
#include "simple_forward.h"
#include "firewall.h"
#include "aes_encrypt.h"
#include "aes_decrypt.h"

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192 * 128
#define MBUF_SIZE (1600 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MBUF_CACHE_SIZE 0
#define NO_FLAGS 0
#define RING_MAX 1  // 1个ring

#define NUM_PORTS 1
#define MAX_NUM_PORT 4
#define MAX_LCORE_NUM 24
#define MAX_NF_NUM 1000
#define MAX_AGENT_NUM 5
#define NF_QUEUE_RING_SIZE 256
#define NF_NAME_RX_RING "Agent_NF_%u_rq"
#define NF_NAME_TX_RING "Agent_NF_%u_tq"


/* configuartion */

#define SFC_CHAIN_LEN 3 //FIXME: 限定SFC长度为3
#define MONITOR_PERIOD 30  // 3秒钟更新 一次 monitor的信息

uint16_t nb_nfs = 10; //修改时必须更新nf_func_config, service_time_config, priority_config, start_sfc_config, flow_ip_table
uint16_t nb_agents = 4;//修改时必须更新coremask_set
int rx_exclusive_lcore = 2;//根据不同机器来制定, 0预留给core manager
int tx_exclusive_lcore = 4;
static const int dv_tolerance = 0;//NF丢包率超过这个阈值才进行扩展处理
static const int mini_sertime_per_core = 1;//core的total service time低于这个阈值则被认定空闲，应该回收
lthread_func_t nf_fnuc_config[MAX_NF_NUM]={lthread_firewall, lthread_firewall, lthread_firewall, lthread_forwarder,
                                           lthread_forwarder, lthread_forwarder, lthread_forwarder, lthread_forwarder,
                                           lthread_forwarder, lthread_forwarder, lthread_forwarder, lthread_forwarder};//NF函数，在nfs头文件里面定义
int nf_service_time_config[MAX_NF_NUM] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
int nf_priority_config[MAX_NF_NUM]={0, 0, 0, 0, 0, 1, 1, 1, 3, 3};
int start_sfc_config_flag[MAX_NF_NUM]={0, 0, 0, 0, 0, 1, 1, 1, 0, 0};
uint64_t flow_ip_table[MAX_NF_NUM]={
        16820416, 33597632, 67152064, 83929280, 100706496, 117483712, 50374848, 0, 0, 134260928};
/* initial core policy:
 * CoreManager: core 0,
 * piority 0: core 6,8,10,(0x540)
 * p1:12,14(0x5000), p2:16(0x10000)  p3:18,20(0xc140000)*/
uint64_t coremask_set[MAX_AGENT_NUM]={
        0x54003, 0x500002, 0x1000001, 0xc14000002};

struct Agent_info{

    int core_cnt;
    int priority;
    int Agent_id;
    int nfs_num;
    int core_list[MAX_LCORE_NUM];
};
struct core_info{
    uint64_t drop_rate;
    uint64_t service_time_total;
    uint16_t nf_total;
    int agent_id;

};

/* variable */
struct Agent_info *agents_info_data;
struct core_info *cores_info_data;
struct nf_thread_info *nf[ MAX_NF_NUM];
static uint8_t port_id_list[NUM_PORTS] = {  0};
static struct rte_mempool *pktmbuf_pool;
static uint8_t keep_running = 1;
uint16_t nb_lcores = 0;
int last_idle_core[MAX_AGENT_NUM]={-1};

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
//    printf(">success\n");
    struct rte_eth_link  link;
    int cnt = 0;
    do{
        rte_eth_link_get(port, &link);
        cnt++;
        printf("try get link %d\n", cnt);
        if(cnt==5)
            break;
    }while(link.link_status != ETH_LINK_UP);
    if(link.link_status == ETH_LINK_DOWN){
//        rte_exit(EXIT_FAILURE, ":: error: link is still down\n");
        printf("link down with port %d\n", port);
        return 0;
    }
    return 0;
}

static int
init_mbuf_pools(void) {

    const unsigned num_mbufs = NUM_MBUFS * NUM_PORTS;

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

static void
lthread_null(__rte_unused void *args)
{
    int lcore_id = rte_lcore_id();
    lthread_exit(NULL);
}
/* before give a new core to a Agent, update its core params */
void add_core_to_set(int agent_id, int lcore_id){

    int count = agents_info_data[agent_id].core_cnt;
    printf("adding a core %d to agent %d, old core cnt=%d\n", lcore_id, agent_id, count);
    count += 1;
    agents_info_data[agent_id].core_list[count-1]=lcore_id;
    agents_info_data[agent_id].core_cnt = count;
    printf("update agent %d core list, new cnt= %d\n", agent_id, count);
    return;
}
void dec_core_from_set(int agent_id, int victim_lcore_id){

    uint8_t count = (agents_info_data[agent_id].core_cnt);
    int i, index=-1;//lcore
    for(i = count-1;i>=0; i--){
        if(agents_info_data[agent_id].core_list[i] == victim_lcore_id){
            index = i;
            break;
        }
    }
    if(index==-1){
        printf("agent %d without %d, dec failure\n", agent_id, victim_lcore_id);
    }
    for(i=index+1;i<count;i++){
        agents_info_data[agent_id].core_list[i] = agents_info_data[agent_id].core_list[i+1];
    }
    agents_info_data[agent_id].core_cnt -= 1;
    printf("update agent %d core list: cnt=%d ", agent_id, count-1);
    for( i = 0;i<count-1;i++)
        printf(" core %d,", agents_info_data[agent_id].core_list[i]);
    printf("\n");

    return;
}
/*
 * notify thread manager to giveback the core of a Agent,
 * thread manager will migrate nfs to another core of the Agent
 * */
int notify_to_give_back(int victim_lcore_id, int agent_id){

    uint8_t count = (agents_info_data[agent_id].core_cnt);
    int dst_core = agents_info_data[agent_id].core_list[count-2];//FIXME:策略：总是让倒数最后一个core迁移到倒数第二个
    int i, tmp_core;

    printf("CM notify sched %d give back, migrate existing nfs to core %d (with sv time=%d)\n", victim_lcore_id, dst_core,
           cores_info_data[dst_core].service_time_total);
    if(cores_info_data[victim_lcore_id].nf_total > 0){
        set_migrate_to_core(dst_core, victim_lcore_id);
        set_migrate_flag(2, victim_lcore_id);
        while(read_migrate_flag(victim_lcore_id)>0){
            rte_delay_ms(1);
        }
    }
    printf("CM recv give back finish msg\n");
    return dst_core;
}

int reply_to_Agent_request(int agent_id, int lcore_id){

    int i, core_cnt, j, dst_core, nf_id;
    int without_wait = 0;
    dst_core = last_idle_core[agent_id];

    if(cores_info_data[dst_core].drop_rate >= dv_tolerance){//在Agent内再找一个空闲核
        core_cnt = (agents_info_data[agent_id].core_cnt);
        dst_core = -1;
        for(i = 0;i< core_cnt; i++){
            if(cores_info_data[i].drop_rate < dv_tolerance){
                dst_core = i;
            }
        }
    }//end
//    printf(">>inner Agent, last idle core = %d\n", dst_core);

    if(dst_core < 0){
        //select a victim from other Agents
        uint8_t first_visit = 0;
        int victim_low_priority = -1;
        int victim_idle  = -1;
        int k, count = 0, tmp_service_tm = 9999999;

        for(j = nb_agents - 1; j>agent_id; j--){
            count = (agents_info_data[j].core_cnt);
            if(count<=1)
                continue;
            if(first_visit == 0){
                victim_low_priority = agents_info_data[j].core_list[count -1];
                first_visit = 1;
            }
            for(k = 0;k<count; k++){
                if(cores_info_data[k].service_time_total < mini_sertime_per_core){
                    victim_idle = k;
                    break;
                }
            }//end for count
            if(victim_idle>0)
                break;
        }//nd for agents

        if(victim_idle<0){
            dst_core = victim_idle;
        } else{
            dst_core = victim_low_priority;
        }
        if(dst_core<0)//no available resource
            return -1;
        printf("preemptive core %d fron Agent %d\n", dst_core, cores_info_data[dst_core].agent_id);
        //通知give back
        int push_aside_core = notify_to_give_back(dst_core, cores_info_data[dst_core].agent_id);
        //更新dst_core info, victim_core info
        cores_info_data[push_aside_core].nf_total += cores_info_data[dst_core].nf_total;
        cores_info_data[push_aside_core].service_time_total += cores_info_data[dst_core].service_time_total;
        cores_info_data[dst_core].service_time_total = 0;
        cores_info_data[dst_core].nf_total = 0;
        //更新 agent info
        add_core_to_set(agent_id, dst_core);
        dec_core_from_set(cores_info_data[dst_core].agent_id, dst_core);

    }
    if(dst_core<0)
        return -1;

    //notify thread manager
    printf(">CM notify requesting core %d to use new core %d\n", lcore_id, dst_core);
    set_migrate_to_core(dst_core, lcore_id);
    set_migrate_flag(1, lcore_id);
    while(read_migrate_flag(lcore_id)>0){
        rte_delay_ms(1);
    }
    nf_id = read_migrate_to_core(lcore_id) - nb_lcores;//返回的NF thread_id
    printf(">CM recv msg from core %d, migrated nf %d\n\n", lcore_id, nf_id);
    cores_info_data[dst_core].nf_total += 1;
    cores_info_data[dst_core].service_time_total += nfs_info_data[nf_id].service_time;
    nfs_info_data[nf_id].lcore_id = dst_core;
    last_idle_core[agent_id] = dst_core;

    return 0;
}
/*
 * main loop of core manager
 */

static int
lthread_master_spawner(__rte_unused void *arg) {

    struct lthread *lt[MAX_NF_NUM];
    int lcore_id, agent_id;
    int nfs_num_per_core;
    int i, sched, add_flag, new_core_id, sfc_flag = 0, nf_id;
    int index_of_agent[MAX_AGENT_NUM];
    int monitor_tick = 0;
    printf("Entering core manager loop on core %d\n", rte_lcore_id());

    for(i = 0;i<MAX_AGENT_NUM;i++){
        index_of_agent[i] = 0;
    }

    for(i = 0;i<nb_nfs; i++){
        rte_delay_ms(10);

        sfc_flag = start_sfc_config_flag[i];
        agent_id = nfs_info_data[i].agent_id;
        if(sfc_flag == 1){
            lcore_id = agents_info_data[agent_id].core_list[index_of_agent[agent_id]];
            printf("dispatch sfc: nf %d-> nf %d-> nf %d on core %d of Agent %d\n", i, i+1, i+2, lcore_id, agent_id);
            launch_sfc(lt, &lcore_id, SFC_CHAIN_LEN, nfs_info_data[i].fun, (void *)nf[i],
                             nfs_info_data[i+1].fun, (void *)nf[i+1], nfs_info_data[i+2].fun, (void *)nf[i+2]);
            cores_info_data[lcore_id].nf_total += SFC_CHAIN_LEN;
            nfs_info_data[i].lcore_id = lcore_id;
            int k= 0;
            for(k = 0;k<SFC_CHAIN_LEN; k++){
                //FIXME: 这里应该根据流速来计算，为了方便先粗略忽略流速的影响
                cores_info_data[lcore_id].service_time_total += nfs_info_data[i++].service_time;
            }
            i-=1;
        }else{
            lcore_id = agents_info_data[agent_id].core_list[index_of_agent[agent_id]];
            cores_info_data[lcore_id].nf_total += 1;
            cores_info_data[lcore_id].service_time_total += nfs_info_data[i].service_time;
            nfs_info_data[i].lcore_id = lcore_id;
            printf("dispatch nf %d on core %d of Agent %d\n", i, lcore_id, agent_id);
            launch_batch_nfs(lt, &lcore_id, 1, nfs_info_data[i].fun, (void *)nf[i]);
        }
        index_of_agent[agent_id]++;
        index_of_agent[agent_id] %= (agents_info_data[agent_id].core_cnt);
    }

    uint64_t processed_pps = 0;
    uint64_t dropped_pps = 0; // nf 0 每秒丢包数
    double dropped_ratio = 0;
    uint64_t update_core_dv_iteration = 10000;
    uint64_t cnt = 0;
    int dst_core;
    int ret, core_id;
    while (keep_running){

        rte_delay_ms(1000);
        //update需要更新: agent.core_mask, core_list,  core.nf_cnt, core service time
        for(core_id = 0; core_id< MAX_LCORE_NUM; core_id++){
            if(cores_info_data[core_id].nf_total == 0)
                continue;
            cores_info_data[core_id].drop_rate = 0;
        }
        for(nf_id = 0;nf_id<nb_nfs; nf_id++){
            if(nfs_info_data[nf_id].belong_to_sfc == 1){
                continue;//TODO: 默认不迁移、不扩展SFC
            }else{
                cnt++;
                processed_pps = get_processed_pps_with_nf_id(nf_id);  // nf 0 的处理能力
                dropped_pps = get_dropped_pps_with_nf_id(nf_id); // nf 0 每秒丢包数
                dropped_ratio = get_dropped_ratio_with_nf_id(nf_id); // nf 0 这段时间的丢包率

                if(dropped_pps > 0){
                    printf(">>> NF %d <<< processing pps: %9ld, drop pps: %9ld, drop ratio: %9lf\n", nf_id, processed_pps,
                           dropped_pps, dropped_ratio);
                    printf(">processing nf %d on core %d in Agent %d\n", nf_id,nfs_info_data[nf_id].lcore_id, nfs_info_data[nf_id].agent_id);
                    //scaling
                    ret = reply_to_Agent_request((nfs_info_data[nf_id].agent_id), nfs_info_data[nf_id].lcore_id);
                    if(ret <0){
                        printf("no available resouce to add\n");
                    }
                    cores_info_data[nfs_info_data[nf_id].lcore_id].drop_rate += dropped_pps;
                }
            }
        }

        // 在这里算nf丢包信息
        monitor_tick++;
        if (monitor_tick == MONITOR_PERIOD) {
            monitor_update(3);
            monitor_tick = 0;
        }
    }

}

static int
sched_spawner(__rte_unused void *arg) {
    struct lthread *lt;
    int lcore_id = rte_lcore_id();

    printf(">launching scheduler on core %d\n", lcore_id);
    launch_batch_nfs(&lt, &lcore_id, 1, lthread_null, NULL);
    slave_scheduler_run();

    return 0;
}
/*
 * at first give cores to each agent
 */

int main(int argc, char *argv[]) {

    int ret, i, j;
    uint8_t total_ports, cur_lcore;

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

    for(i = 0;i<NUM_PORTS;i++){
        ret = port_init(port_id_list[i]);
        if (ret != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %u\n",port_id_list[i]);
    }
    printf(">>>try to allocate nf info data\n");
    nfs_info_data = rte_calloc("nfs info",
                               MAX_NF_NUM, sizeof(*nfs_info_data), 0);
    if (nfs_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for nf  details\n");
    printf(">>>suc\n");
    agents_info_data = rte_calloc("agents info", MAX_AGENT_NUM, sizeof(*agents_info_data), 0);
    if (agents_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for agent = details\n");
    cores_info_data = rte_calloc("cores info", LTHREAD_MAX_LCORES, sizeof(*cores_info_data), 0);
    if(cores_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate mem for core info\n");

    // 分配一个核给 flow distributor rx thread
    flow_table_init();
    struct port_info *port_info1 = calloc(1, sizeof(struct port_info));
    port_info1->port_id = 0;
    port_info1->queue_id = 0;
    cur_lcore = rx_exclusive_lcore;
    if (rte_eal_remote_launch(flow_director_rx_thread, (void *) port_info1, cur_lcore) == -EBUSY) {
        printf("Core %d is already busy, can't use for RX of flow director \n", cur_lcore);
        return -1;
    }

    // 分配一个核给 flow distributor tx thread
    struct port_info *port_info2 = calloc(1, sizeof(struct port_info));
    port_info2->port_id = 0;
    port_info2->queue_id = 0;
    cur_lcore = tx_exclusive_lcore;
    if (rte_eal_remote_launch(flow_director_tx_thread, (void *) port_info2, cur_lcore) == -EBUSY) {
        printf("Core %d is already busy, can't use for TX of flow director \n", cur_lcore);
        return -1;
    }

    /* init core info */
    for(i = 0; i<MAX_LCORE_NUM; i++){
        cores_info_data[i].drop_rate = 0;
        cores_info_data[i].nf_total = 0;
        cores_info_data[i].service_time_total = 0;
    }

    /* init Agent info */
    uint64_t tmp_mask;
    int index = 0, lc=0;
    int start_sfc = 0;

    for(i = 0;i<nb_agents;i++){
        agents_info_data[i].Agent_id = 1000+i;
        agents_info_data[i].nfs_num = 0;
        agents_info_data[i].priority = i;
    }
    /* init nf info */
    for(i = 0;i<nb_nfs;i++){
        rq_name = get_nf_rq_name(i);
        tq_name = get_nf_tq_name(i);
        nfs_info_data[i].rx_q = rte_ring_create(rq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SP_ENQ);
        nfs_info_data[i].tx_q = rte_ring_create(tq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SP_ENQ);
        if(nfs_info_data[i].rx_q == NULL || nfs_info_data[i].tx_q == NULL){
            rte_exit(EXIT_FAILURE, "cannot create ring for nf %d\n", i);
        }
        nfs_info_data[i].nf_id = i;
        nfs_info_data[i].fun = nf_fnuc_config[i];
        nfs_info_data[i].service_time = nf_service_time_config[i];
        nfs_info_data[i].priority = nf_priority_config[i];
        nfs_info_data[i].agent_id = nfs_info_data[i].priority;
        nfs_info_data[i].state = rte_calloc("nf state", MAX_STATE_LEN, sizeof(*nfs_info_data[i].state), 0);
        nfs_info_data[i].state->hash_map = rte_calloc("nf hash map", BIG_PRIME, sizeof(*nfs_info_data[i].state->hash_map), 0);
//        nfs_info_data[i].state->key_schedule = rte_calloc("nf key schedule", AES_ENCRYPT_LEN, sizeof(WORD), 0);

        printf(">>>suc to allocate nf state\n");
        if(start_sfc_config_flag[i] == 0){
            printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[i], i);
            flow_table_add_entry(flow_ip_table[i], i); // flow hash (这里使用的是flow的源IP值), nf_id
            bind_nf_to_rxring(i, nfs_info_data[i].rx_q);
            printf("register nf %d tx ring\n", i);
            nf_need_output(i, 0, nfs_info_data[i].tx_q);
            if(start_sfc == 1){
                start_sfc = 0;
                printf("register nf %d tx ring\n", i-1);
                nf_need_output(i, 0, nfs_info_data[i-1].tx_q);  // 参数为 nf_id, 要往哪个port 上 发数据, nf_id 对应的tx_ring
            }
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
                    nf_need_output(i, 0, nfs_info_data[i].tx_q);
                }
            }
        }
        agents_info_data[nfs_info_data[i].priority].nfs_num +=1;
        nf[i] = calloc(1, sizeof(struct nf_thread_info));
        nf[i]->nf_id = i;

    }

    for(i = 0;i<nb_agents ;i++){

        index = 0; lc=0;
        tmp_mask = coremask_set[i];
        agents_info_data[i].core_cnt = (tmp_mask & 255);
        nb_lcores += (tmp_mask&255);
        printf(">>init Agent %d with priotity %d, %d core\n", agents_info_data[i].Agent_id, agents_info_data[i].priority,
               (agents_info_data[i].core_cnt));
        tmp_mask = (tmp_mask>>8);
        while(tmp_mask>0){
            if((tmp_mask&1UL)==1){
                agents_info_data[i].core_list[index++] = lc;
                cores_info_data[lc].agent_id = i;
                printf(">>>>get core %d\n", agents_info_data[i].core_list[index-1]);
            }
            lc++;
            tmp_mask = tmp_mask>>1;
        }
    }

    /* launch scheduler of each Agent */
    for(j = 0; j<nb_agents;j++){
        int core_cnt = (agents_info_data[j].core_cnt);
        for(i = 0;i<core_cnt;i++){
            cur_lcore = agents_info_data[j].core_list[i];
            if (rte_eal_remote_launch(sched_spawner, NULL, cur_lcore) == -EBUSY) {
                printf("Core %d is already busy, can't use for sched\n", cur_lcore);
                return -1;
            }
        }
    }

    /* main loop of CM */
    lthread_master_spawner(NULL);


    return 0;
}