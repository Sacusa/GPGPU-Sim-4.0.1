#include "dram_sched.h"
#include "dram_sched_gi.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

gi_scheduler::gi_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{}

void gi_scheduler::update_mode() {
  bool have_reads = false, have_writes = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_reads = have_reads || !m_queue[b].empty();
    if (m_config->seperate_write_queue_enabled) {
      have_writes = have_writes || !m_write_queue[b].empty();
    }
  }

  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    if ((m_num_pim_pending < m_config->pim_low_watermark)
        && (have_reads || have_writes)) {
      // Just switch to READ_MODE. The following code sequence will take care
      // of deciding whether we stay in READ_MODE or switch to WRITE_MODE.
      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to non-PIM mode\n");
#endif
    }
  } else {
    if ((m_num_pim_pending >= m_config->pim_high_watermark)
        || (!have_reads && !have_writes && have_pim)) {
      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }

  dram_scheduler::update_mode();
}
