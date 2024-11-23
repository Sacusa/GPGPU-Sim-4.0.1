#ifndef __DRAM_SCHED_PIM_FRFCFS_H__
#define __DRAM_SCHED_PIM_FRFCFS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum pim_frfcfs_switch_reason {
  PIM_FRFCFS_CAP_EXCEEDED = 0,
  PIM_FRFCFS_OUT_OF_REQUESTS,
  PIM_FRFCFS_NUM_SWITCH_REASONS
};

const std::string pim_frfcfs_switch_reason_str[] = {"CapExceeded",
  "OutOfRequests"};

class pim_frfcfs_scheduler : public dram_scheduler {
 public:
  pim_frfcfs_scheduler(const memory_config *config, dram_t *dm,
         memory_stats_t *stats);
  void update_mode() override;

  // Stats
  std::map<pim_frfcfs_switch_reason, unsigned> m_mem2pim_switch_reason;
  std::map<pim_frfcfs_switch_reason, unsigned> m_pim2mem_switch_reason;

 private:
  unsigned m_num_bypasses;  // Used to enforce CAP
};

#endif
