#include "dram_sched.h"
#include "dram_sched_gi_mem.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

gi_mem_scheduler::gi_mem_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{}

void gi_mem_scheduler::update_mode() {
  bool have_mem = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_mem = have_mem || !m_queue[b].empty() || \
               (m_config->seperate_write_queue_enabled && \
                !m_write_queue[b].empty());
  }

  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    if ((m_num_pending >= m_config->queue_high_watermark) || \
        (m_num_write_pending >= m_config->write_high_watermark) || \
        (!have_pim && have_mem)) {
      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to non-PIM mode\n");
#endif
    }
  } else {
    if (((m_num_pending < m_config->queue_low_watermark) && \
         (m_num_write_pending < m_config->write_high_watermark)) || \
        (have_pim && !have_mem)) {
      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }

  dram_scheduler::update_mode();
}
