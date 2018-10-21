//
// Created by zzl on 2018/10/21.
//

#ifndef PERFORMANCE_THREAD_CORE_MANAGER_H
#define PERFORMANCE_THREAD_CORE_MANAGER_H

#endif //PERFORMANCE_THREAD_CORE_MANAGER_H
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
int register_Agent(uint16_t priority){

}

/*
 * check drop rate vector from shared state with switch
 */

int get_drop_rate_vector(void){

}
