#ifndef __DRAM_SCHED_PIM_FIRST_H__
#define __DRAM_SCHED_PIM_FIRST_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

class pim_first_scheduler : public dram_scheduler {
 public:
  pim_first_scheduler(const memory_config *config, dram_t *dm,
          memory_stats_t *stats);
  void update_mode() override;
};

#endif
