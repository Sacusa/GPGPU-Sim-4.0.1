#ifndef __DRAM_SCHED_QUEUE3_H__
#define __DRAM_SCHED_QUEUE3_H__

#include <algorithm>
#include <list>
#include <map>
#include <math.h>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define DRAM_SCHED_QUEUE3_MAX_QUEUE_DIFF 0.05

class queue3_scheduler : public dram_scheduler {
 public:
  queue3_scheduler(const memory_config *config, dram_t *dm,
      memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule_pim() override;

  bool queues_imbalanced();

 private:
  // The following values are provided by the DRAM
  unsigned m_mem_queue_size;
  unsigned m_pim_queue_size;

  // Values to keep track of PIM and MEM phase lengths
  unsigned m_last_pim_row;
  unsigned long long m_pim_batch_start_time;
};

#endif
