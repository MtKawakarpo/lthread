//
// Created by zzl on 2018/10/21.
//

#ifndef PERFORMANCE_THREAD_CORE_MANAGER_H
#define PERFORMANCE_THREAD_CORE_MANAGER_H

#endif //PERFORMANCE_THREAD_CORE_MANAGER_H

#include <nf_lthread_api.h>
//#include "nf_lthread_sched.h"
/*
 * Register client with socket:
 * @ param identifier: enable core manager to index its related profiling table
 * @ param priority: to allocate cores
 * @ param nb_core: initial expected number of cores
 * return and set cpuset
 */
int registerAgent(uint16_t Agent_id, uint16_t priority, uint16_t nb_core){

    //TODO: this should be decided by CM
    uint16_t coremask = 0xf04;
    return coremask;

}
/*
 * request one core from Core Manager
 *
 * @param priority
 * @param drop rate
 *
 * @return 0/1
 */
int add_one_core(uint16_t priority, uint64_t drop_rate){

}

/*
 * give back a core to Core Manager
 *
 * @param core_id
 */
int give_back_core(uint8_t core_id){

}

/*
 * register Agent to CM
 *
 * @param priority
 *
 * @return Agent_id
 */


/*
 * check drop rate vector from shared state with switch
 */

int get_drop_rate_vector(void){

}

//api from vswitch
/*
 *
 */
int registerMonitor(int Agent_id, uint64_t queue_mask_count){

}
void updateMapping(void){

}
int updateDropVector(void){
    static int iteration = 0;
    iteration++;
    //TODO: iteration time can be change
    if(iteration % 1000 == 0){
        //TODO: call vswitch to update
   }

}

//TODO: this should be a shared variable with vSwitch
static uint64_t nf_drop_vector = 0;//bitmap to record whether each thread drop or not
static uint64_t core_drop_vector = 0;//bitmap to record whether each core drop or not

int checkIsDrop(int thread_id){
    int i;
    int nb_cores = core_mask_count&255;
    static int last_idle_core_0 = 0;
    static int flag = 0;
    uint64_t tmp_dv = nf_drop_vector;
    uint64_t tmp_core_dv = core_drop_vector;
    uint64_t bit_base = 1;

    if(flag == 0){
        last_idle_core_0 = core_list[0];
        flag = 1;
    }
    /* note: now just support 127 cores and 127 threads per core ar most */
    uint8_t drop = 0;
    drop = ((tmp_dv>>thread_id)&1UL);
    //for test
    static int cnt = 0;

    if(drop == 1){
        cnt++;
        if((((tmp_core_dv>>last_idle_core_0)&1UL)==0)){
            nf_drop_vector &=  (~(bit_base<<thread_id));
            printf(">>>try last core, suggest thread %d to migrate to %d, reset dv to %d\n",
                   thread_id, last_idle_core_0, nf_drop_vector);
            return last_idle_core_0;
        }

        for(i = 0;i<nb_cores;i++){
            tmp_core_dv = core_drop_vector;
            if(((tmp_core_dv>>core_list[i])&1UL)==0){
                last_idle_core_0 = core_list[i];
                printf(">>try core list, suggest thread %d to migrate to %d", thread_id, core_list[i]);
                return core_list[i];//migrate to core core_list[i]
            }
        }
        return -2;// no core available, should add core
    }else
        return -1;// no drop
}
