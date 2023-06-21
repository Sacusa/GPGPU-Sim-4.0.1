#ifndef __DRAM_SCHED_I1__
#define __DRAM_SCHED_I1__

#include <list>
#include <map>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

class i1_scheduler : public dram_scheduler {
 public:
  i1_scheduler(const memory_config *config, dram_t *dm, memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;
  bool is_pim_req_store(dram_req_t *req);

 private:
  std::vector<unsigned> m_reqs_per_bank;
  unsigned m_max_req_per_bank;
 
  unsigned long m_pim_curr_transaction_start_time;
  unsigned long m_pim_last_transaction_dur;

  bool m_is_last_pim_store;
};

#endif
