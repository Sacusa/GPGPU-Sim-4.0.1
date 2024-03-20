#ifndef __DRAM_SCHED_QUEUE2_H__
#define __DRAM_SCHED_QUEUE2_H__

#include <algorithm>
#include <list>
#include <map>
#include <math.h>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define DRAM_SCHED_QUEUE2_PHASE_LENGTH 1000

class queue2_scheduler : public dram_scheduler {
 public:
  queue2_scheduler(const memory_config *config, dram_t *dm,
      memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  void update_switch_conditions();

 private:
  // The following values are provided by the DRAM
  unsigned m_mem_queue_size;
  unsigned m_pim_queue_size;
  unsigned m_prev_ave_mrqs;
  unsigned m_prev_ave_pim_mrqs;

  // Values to keep track of PIM and MEM phase lengths
  unsigned m_last_pim_row;
  unsigned long long m_pim_batch_start_time;
  unsigned long long m_pim_phase_dur;
  unsigned long long m_mem_phase_start_time;
  unsigned long long m_next_update_cycle;

  // Adjustable MEM and PIM phase lengths
  unsigned m_finished_pim_batches;
  unsigned m_min_pim_batches;
  float m_mem_time_ratio;
  
  // User defined parameters
  unsigned m_min_pim_batches_hi;
  float m_mem_time_ratio_hi;
  float m_mem_time_ratio_lo;
};

#endif
