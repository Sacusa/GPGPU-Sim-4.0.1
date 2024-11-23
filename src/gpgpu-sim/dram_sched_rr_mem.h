#ifndef __DRAM_SCHED_RR_MEM__
#define __DRAM_SCHED_RR_MEM__

#include <list>
#include <map>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define RR_MEM_PIM_GRANULARITY 16

class rr_mem_scheduler : public dram_scheduler {
 public:
  rr_mem_scheduler(const memory_config *config, dram_t *dm,
      memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  // Additional stats for research
  //std::vector<unsigned long long> m_pim_batch_exec_time;
  //std::vector<unsigned long long> m_mem_batch_exec_time;
  //std::vector<unsigned long long> m_mem_wasted_cycles;

 private:

  unsigned long long m_num_pim_executed;
  unsigned long long m_pim_cap;

  unsigned long long m_num_mem_executed;
};

#endif
