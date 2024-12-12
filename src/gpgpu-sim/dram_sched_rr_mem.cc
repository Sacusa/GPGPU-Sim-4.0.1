#include "dram_sched.h"
#include "dram_sched_rr_mem.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

rr_mem_scheduler::rr_mem_scheduler(const memory_config *config, dram_t *dm,
        memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_num_pim_executed = 0;
  m_pim_cap = 0;

  m_num_mem_executed = 0;
}

void rr_mem_scheduler::update_mode() {
  bool have_reads = false, have_writes = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_reads = have_reads || !m_queue[b].empty();
    if (m_config->seperate_write_queue_enabled) {
      have_writes = have_writes || !m_write_queue[b].empty();
    }
  }

  bool have_mem = have_reads || have_writes;
  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    if (have_mem && ((m_num_pim_executed > m_pim_cap) || !have_pim)) {
      m_num_pim_executed = 0;
      m_pim_cap = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to MEM mode\n", m_dram->id);
#endif

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
    }
  }

  else {
    if (have_pim && ((m_num_mem_executed > m_config->frfcfs_cap) || \
          !have_mem)) {
      // Use the minimum of executed requests and CAP to set the PIM cap
      m_pim_cap = std::min(m_num_mem_executed,
          (unsigned long long) m_config->frfcfs_cap);

      // Round the CAP up to the nearest multiple of PIM_GRANULARITY
      m_pim_cap = ((m_pim_cap + RR_MEM_PIM_GRANULARITY - 1) / \
          RR_MEM_PIM_GRANULARITY) * RR_MEM_PIM_GRANULARITY;

      // Scale the cap according to the QoS specification
      m_pim_cap *= m_config->max_pim_slowdown;

      m_num_mem_executed = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
#endif

      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;
    }
  }

  update_rw_mode();
}

dram_req_t *rr_mem_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req) {
    m_num_mem_executed++;
  }

  return req;
}

dram_req_t *rr_mem_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    m_num_pim_executed++;
  }

  return req;
}
