#ifndef __DRAM_SCHED_DYN_THRESH_H__
#define __DRAM_SCHED_DYN_THRESH_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define DYN_THRESH_MAX_OCCUPANCY_HIT 2

class dyn_thresh_scheduler : public dram_scheduler {
 public:
  dyn_thresh_scheduler(const memory_config *config, dram_t *dm,
      memory_stats_t *stats);
  void add_req(dram_req_t *req) override;
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

 private:
  std::list<std::list<dram_req_t *>::iterator> *m_pim_queue_it;
  unsigned long long m_pim_batch_start_time;
  unsigned m_last_pim_row;

  std::vector<unsigned long long> m_bank_pending_mem_requests;

  unsigned m_mem_stall_time;
  unsigned m_pim_stall_time;
  unsigned m_max_pim_stall_time;
  unsigned m_num_times_mem_hit_occupancy;

  unsigned m_max_mem_occupancy;
};

#endif
