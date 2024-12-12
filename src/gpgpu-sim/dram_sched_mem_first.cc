#include "dram_sched.h"
#include "dram_sched_mem_first.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

mem_first_scheduler::mem_first_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{}

void mem_first_scheduler::update_mode() {
  if ((m_num_pending + m_num_write_pending) > 0) {
    if (m_dram->mode == PIM_MODE) {
      m_dram->pim2nonpimswitches++;
    }

    m_dram->mode = READ_MODE;
  }

  update_rw_mode();
}
