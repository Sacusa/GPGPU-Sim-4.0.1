#ifndef __DRAM_SCHED_F3FS_H__
#define __DRAM_SCHED_F3FS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum f3fs_switch_reason {
  F3FS_CAP_EXCEEDED = 0,
  F3FS_OUT_OF_REQUESTS,
  F3FS_NUM_SWITCH_REASONS
};

const std::string f3fs_switch_reason_str[] = {"CapExceeded",
  "OutOfRequests"};

class f3fs_scheduler : public dram_scheduler {
 public:
  f3fs_scheduler(const memory_config *config, dram_t *dm,
         memory_stats_t *stats);
  void update_mode() override;

  // Stats
  std::map<f3fs_switch_reason, unsigned> m_mem2pim_switch_reason;
  std::map<f3fs_switch_reason, unsigned> m_pim2mem_switch_reason;

 private:
  unsigned m_pim_cap;
  unsigned m_num_bypasses;  // Used to enforce CAP
};

#endif
