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
  bool have_mem = (m_num_pending + m_num_write_pending) > 0;
  bool have_pim = m_num_pim_pending > 0;

  if (m_dram->mode == PIM_MODE) {
    if (have_mem) {
      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
      update_rw_mode();
    }
  }

  else {
    if (!have_mem && have_pim) {
      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;
    }
  }
}
