#ifndef __DRAM_SCHED_I3__
#define __DRAM_SCHED_I3__

#include <list>
#include <map>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define MAX_PIM_BATCHES 8

class i3_scheduler : public dram_scheduler {
 public:
  i3_scheduler(const memory_config *config, dram_t *dm, memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

 private:
  std::vector<unsigned> m_num_non_pim_reqs;
  std::vector<unsigned> m_max_non_pim_reqs;

  std::vector<unsigned long long> m_non_pim_req_start_time;
  std::vector<unsigned long long> m_non_pim_batch_dur;

  unsigned m_last_pim_row;
 
  unsigned long long m_pim_batch_start_time;
  unsigned long long m_pim_batch_dur;

  unsigned m_finished_batches;
};

#endif
