#include "dram_sched.h"
#include "dram_sched_rr_batch_cap.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

rr_batch_cap_scheduler::rr_batch_cap_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_non_pim_to_pim_switch_cycle = 0;

  m_last_pim_row = 0;

  m_pim_batch_start_time = 0;
  m_pim_batch_dur = 0;

  m_mem_batch_start_time = 0;

  m_finished_batches = 0;
  prev_pim_num = 0;
}

void rr_batch_cap_scheduler::update_mode() {
  bool have_reads = false, have_writes = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_reads = have_reads || !m_queue[b].empty();
    if (m_config->seperate_write_queue_enabled) {
      have_writes = have_writes || !m_write_queue[b].empty();
    }
  }

  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    // Transaction is over if the next PIM request will access a new row
    bool is_batch_over = (m_pim_batch_start_time > 0) && (!have_pim ||
                          (m_pim_queue->front()->row != m_last_pim_row));

    if (is_batch_over) {
      unsigned long long batch_exec_time = m_dram->m_dram_cycle - \
                                           m_pim_batch_start_time;
      m_pim_batch_start_time = 0;
      m_pim_batch_exec_time.push_back(batch_exec_time);
      m_pim_batch_dur += batch_exec_time;

#ifdef DRAM_SCHED_VERIFY
      if (have_pim) {
        printf("DRAM (%d): Batch over; row conflict\n", m_dram->id);
      } else {
        printf("DRAM (%d): Batch over; no more requests\n", m_dram->id);
      }
      printf("           Batch execution time = %lld\n", batch_exec_time);
      printf("           Batch size = %d\n", m_dram->pim_num - prev_pim_num);
#endif

      m_finished_batches++;
      prev_pim_num = m_dram->pim_num;

      if (m_finished_batches <= m_config->min_pim_batches) {
        m_non_pim_to_pim_switch_cycle = m_dram->m_dram_cycle + \
            (m_config->max_pim_slowdown * m_pim_batch_dur);
      }
    }

    if (((m_finished_batches >= m_config->min_pim_batches) || !have_pim) && \
            (have_reads || have_writes)) {
#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to non-PIM mode\n", m_dram->id);
      printf("           Executed %d PIM batches\n", m_finished_batches);
      printf("           PIM batch duration = %lld\n", m_pim_batch_dur);
#endif

      m_pim_batch_dur = 0;
      m_finished_batches = 0;

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
    }
  }

  else {
    if (have_pim) {
      if ((m_dram->m_dram_cycle > m_non_pim_to_pim_switch_cycle) || \
          !(have_reads || have_writes)) {
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
#endif
      }
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *rr_batch_cap_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req && (m_mem_batch_start_time == 0)) {
    m_mem_batch_start_time = m_dram->m_dram_cycle;
  }

  return req;
}

dram_req_t *rr_batch_cap_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_last_pim_row = req->row;
  }

  return req;
}

void rr_batch_cap_scheduler::finalize_stats()
{
  if (m_dram->mode == PIM_MODE) {
    unsigned long long batch_exec_time = m_dram->m_dram_cycle - \
                                         m_pim_batch_start_time;
    m_pim_batch_exec_time.push_back(batch_exec_time);
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
