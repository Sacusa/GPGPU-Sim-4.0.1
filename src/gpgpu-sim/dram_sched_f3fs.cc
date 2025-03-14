#include "dram_sched.h"
#include "dram_sched_f3fs.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

f3fs_scheduler::f3fs_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{
  m_num_bypasses = 0;
  m_pim_cap = m_config->frfcfs_cap * m_config->max_pim_slowdown;
}

void f3fs_scheduler::update_mode() {
  bool have_reads = m_num_pending > 0;
  bool have_writes = m_num_write_pending > 0;
  bool have_mem = have_reads || have_writes;
  bool have_pim = m_num_pim_pending > 0;

  if (m_dram->mode == PIM_MODE) {
    bool cap_exceeded = false;

    if ((m_pim_cap > 0) && have_mem && have_pim) {
      for (unsigned int b = 0; b < m_config->nbk; b++) {
        if (m_queue[b].size() == 0) { continue; }

        if (m_queue[b].back()->timestamp < \
                m_pim_queue->front()->timestamp) {
          // There is *at least* one MEM request older than the oldest PIM
          // request; PIM will be bypassing this MEM request so increment
          // the counter
          m_num_bypasses++;
          break;
        }
      }

      cap_exceeded = m_num_bypasses > m_pim_cap;
    }

    if ((have_mem && !have_pim) || cap_exceeded) {
      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
      m_num_bypasses = 0;

      if (cap_exceeded) {
        m_pim2mem_switch_reason[F3FS_CAP_EXCEEDED]++;
      } else {
        m_pim2mem_switch_reason[F3FS_OUT_OF_REQUESTS]++;
      }

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM %d: Switching to non-PIM mode\n", m_dram->id);
#endif
    }
  }

  else {
    bool cap_exceeded = false;

    if ((m_config->frfcfs_cap > 0) && have_mem && have_pim) {
      bool is_pim_oldest = true;

      for (unsigned int b = 0; b < m_config->nbk; b++) {
        if (m_queue[b].size() == 0) { continue; }

        if (m_queue[b].back()->timestamp < \
                m_pim_queue->front()->timestamp){
          // There is *at least* one MEM request older than the oldest
          // PIM request; this means that we do not need to increment the
          // counter
          is_pim_oldest = false;
          break;
        }
      }

      if (is_pim_oldest) { m_num_bypasses++; }

      cap_exceeded = m_num_bypasses > m_config->frfcfs_cap;
    }

    if ((!have_mem && have_pim) || cap_exceeded) {
      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;
      m_num_bypasses = 0;

      if (cap_exceeded) {
        m_mem2pim_switch_reason[F3FS_CAP_EXCEEDED]++;
      } else {
        m_mem2pim_switch_reason[F3FS_OUT_OF_REQUESTS]++;
      }

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM %d: Switching to PIM mode\n", m_dram->id);
#endif
    }
  }

  update_rw_mode();
}
