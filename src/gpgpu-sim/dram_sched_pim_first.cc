#include "dram_sched.h"
#include "dram_sched_pim_first.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

pim_first_scheduler::pim_first_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{}

void pim_first_scheduler::update_mode() {
  if (m_num_pim_pending > 0) {
    m_dram->mode = PIM_MODE;
  }

  dram_scheduler::update_mode();
}
