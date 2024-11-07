#include "dram_sched.h"
#include "dram_sched_rr_req_cap.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

rr_req_cap_scheduler::rr_req_cap_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_non_pim_to_pim_switch_cycle = 0;

  m_num_pim_executed = 0;
  m_pim_batch_start_time = 0;
  m_pim_batch_dur = 0;

  m_mem_batch_start_time = 0;
}

void rr_req_cap_scheduler::update_mode() {
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
    if (m_num_pim_executed <= m_config->min_pim_batches) {
      m_pim_batch_dur = m_dram->m_dram_cycle - m_pim_batch_start_time;
    }

    if (((m_num_pim_executed >= m_config->min_pim_batches) || !have_pim) && \
        have_mem) {
      unsigned long long tot_pim_exec_time = m_dram->m_dram_cycle - \
                                             m_pim_batch_start_time;

      m_non_pim_to_pim_switch_cycle = m_dram->m_dram_cycle + \
          (m_config->max_pim_slowdown * m_pim_batch_dur);

      m_num_pim_executed = 0;
      m_pim_batch_start_time = 0;
      m_pim_batch_dur = 0;
      m_pim_batch_exec_time.push_back(tot_pim_exec_time);

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to MEM mode; ", m_dram->id);
      if (have_pim) {
        printf("hit cap\n");
      } else {
        printf("no more requests\n");
      }
      printf("           Total execution time = %lld\n", tot_pim_exec_time)
      printf("           Batch execution time = %lld\n", m_pim_batch_dur);
      printf("           Requests executed = %d\n", m_num_pim_executed);
#endif

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
    }
  }

  else {
    if (((m_dram->m_dram_cycle > m_non_pim_to_pim_switch_cycle) || \
         !have_mem) && have_pim) {
      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;

      m_mem_batch_exec_time.push_back(m_dram->m_dram_cycle - \
          m_mem_batch_start_time);
      m_mem_batch_start_time = 0;

      if (m_dram->m_dram_cycle < m_non_pim_to_pim_switch_cycle) {
        m_mem_wasted_cycles.push_back(m_non_pim_to_pim_switch_cycle - \
            m_dram->m_dram_cycle);
      }

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
      printf("           Total execution time = %lld\n",
          m_mem_batch_exec_time.back());
#endif
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *rr_req_cap_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req && (m_mem_batch_start_time == 0)) {
    m_mem_batch_start_time = m_dram->m_dram_cycle;
  }

  return req;
}

dram_req_t *rr_req_cap_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_num_pim_executed++;
  }

  return req;
}

void rr_req_cap_scheduler::finalize_stats()
{
  if (m_dram->mode == PIM_MODE) {
    unsigned long long tot_pim_exec_time = m_dram->m_dram_cycle - \
                                           m_pim_batch_start_time;
    m_pim_batch_exec_time.push_back(tot_pim_exec_time);
  }

  else {
    m_mem_batch_exec_time.push_back(m_dram->m_dram_cycle - \
        m_mem_batch_start_time);

    if (m_dram->m_dram_cycle < m_non_pim_to_pim_switch_cycle) {
      m_mem_wasted_cycles.push_back(m_non_pim_to_pim_switch_cycle - \
          m_dram->m_dram_cycle);
    }
  }
}
