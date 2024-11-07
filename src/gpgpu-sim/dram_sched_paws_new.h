#ifndef __DRAM_SCHED_PAWS_NEW__
#define __DRAM_SCHED_PAWS_NEW__

#include <list>
#include <map>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

class paws_new_scheduler : public dram_scheduler {
 public:
  paws_new_scheduler(const memory_config *config, dram_t *dm,
      memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  void finalize_stats();

  // Additional stats for research
  std::vector<unsigned long long> m_pim_batch_exec_time;
  std::vector<unsigned long long> m_mem_batch_exec_time;
  std::vector<unsigned long long> m_mem_wasted_cycles;

 private:

  unsigned long long m_num_pim_executed;
  unsigned long long m_pim_batch_start_time;
  unsigned long long m_prev_pim_batch_boundary;
  unsigned m_last_pim_row;

  unsigned long long m_base_mem_duration;
  unsigned long long m_mem_batch_start_time;
  unsigned long long m_mem_to_pim_switch_cycle;
};

#endif
