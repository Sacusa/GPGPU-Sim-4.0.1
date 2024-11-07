#include "dram_sched.h"
#include "dram_sched_paws_new.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

paws_new_scheduler::paws_new_scheduler(const memory_config *config, dram_t *dm,
        memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_num_pim_executed = 0;
  m_pim_batch_start_time = 0;
  m_prev_pim_batch_boundary = 0;
  m_last_pim_row = 0;

  m_base_mem_duration = 0;
  m_mem_batch_start_time = 0;
  m_mem_to_pim_switch_cycle = 0;
}

void paws_new_scheduler::update_mode() {
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
    // Update the base MEM duration as long as we are under the PIM cap
    if (m_num_pim_executed <= m_config->frfcfs_cap) {
      m_base_mem_duration = m_dram->m_dram_cycle - m_pim_batch_start_time;
    }

    bool is_batch_over = (m_pim_batch_start_time > 0) && (!have_pim ||
                          (m_pim_queue->front()->row != m_last_pim_row));
    bool can_switch = false;

    if (is_batch_over) {
      unsigned long long batch_size = m_num_pim_executed - \
                                      m_prev_pim_batch_boundary;
      m_prev_pim_batch_boundary = m_num_pim_executed;

      // We should switch if the next PIM batch is expected to exceed the cap
      can_switch = can_switch || \
                   (m_num_pim_executed + batch_size) > m_config->frfcfs_cap;
    }

    can_switch = can_switch || (m_num_pim_executed > m_config->frfcfs_cap);

    if (can_switch && have_mem) {
      unsigned long long tot_pim_exec_time = m_dram->m_dram_cycle - \
                                             m_pim_batch_start_time;
      m_pim_batch_exec_time.push_back(tot_pim_exec_time);

      m_mem_to_pim_switch_cycle = m_dram->m_dram_cycle + \
          (m_config->max_pim_slowdown * m_base_mem_duration);

      m_num_pim_executed = 0;
      m_pim_batch_start_time = 0;
      m_prev_pim_batch_boundary = 0;
      m_last_pim_row = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to MEM mode\n", m_dram->id);
      printf("          PIM phase execution time = %lld\n", tot_pim_exec_time);
      printf("          PIM phase size = %d\n", m_num_pim_executed);
#endif

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
    }
  }

  else {
    if (((m_dram->m_dram_cycle > m_mem_to_pim_switch_cycle) || \
         !have_mem) && have_pim) {
      m_mem_batch_exec_time.push_back(m_dram->m_dram_cycle - \
          m_mem_batch_start_time);

      if (m_dram->m_dram_cycle < m_mem_to_pim_switch_cycle) {
        m_mem_wasted_cycles.push_back(m_mem_to_pim_switch_cycle - \
            m_dram->m_dram_cycle);
      }

      m_base_mem_duration = 0;
      m_mem_batch_start_time = 0;
      m_mem_to_pim_switch_cycle = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
#endif

      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *paws_new_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req && (m_mem_batch_start_time == 0)) {
    m_mem_batch_start_time = m_dram->m_dram_cycle;
  }

  return req;
}

dram_req_t *paws_new_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_num_pim_executed++;
    m_last_pim_row = req->row;
  }

  return req;
}

void paws_new_scheduler::finalize_stats()
{
  if (m_dram->mode == PIM_MODE) {
    unsigned long long tot_pim_exec_time = m_dram->m_dram_cycle - \
                                           m_pim_batch_start_time;
    m_pim_batch_exec_time.push_back(tot_pim_exec_time);
  }

  else {
    m_mem_batch_exec_time.push_back(m_dram->m_dram_cycle - \
        m_mem_batch_start_time);

    if (m_dram->m_dram_cycle < m_mem_to_pim_switch_cycle) {
      m_mem_wasted_cycles.push_back(m_mem_to_pim_switch_cycle - \
          m_dram->m_dram_cycle);
    }
  }
}
