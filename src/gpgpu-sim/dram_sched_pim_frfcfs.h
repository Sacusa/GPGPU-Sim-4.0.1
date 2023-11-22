#ifndef __DRAM_SCHED_PIM_FRFCFS_H__
#define __DRAM_SCHED_PIM_FRFCFS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

class pim_frfcfs_scheduler : public dram_scheduler {
 public:
  pim_frfcfs_scheduler(const memory_config *config, dram_t *dm,
                   memory_stats_t *stats);
  void add_req(dram_req_t *req) override;
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

 private:
  std::list<std::list<dram_req_t *>::iterator> *m_pim_queue_it;
  unsigned m_last_pim_row;

  std::vector<unsigned> m_promotion_count;

  bool is_next_req_pim(unsigned bank, unsigned curr_row);
};

#endif
