#include "dram_sched.h"
#include "dram_sched_i2.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

i2_scheduler::i2_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_num_non_pim_reqs.assign(m_config->nbk, 0);
  m_max_non_pim_reqs.assign(m_config->nbk, 0);

  m_non_pim_req_start_time.assign(m_config->nbk, 0);
  m_non_pim_batch_dur.assign(m_config->nbk, 0);

  m_last_pim_row = 0;

  m_pim_batch_start_time = 0;
  m_pim_batch_dur = 0;
}

void i2_scheduler::update_mode() {
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
      m_pim_batch_dur = m_dram->m_dram_cycle - m_pim_batch_start_time;

      for (unsigned b = 0; b < m_config->nbk; b++) {
        unsigned avg_req_latency;
        if (m_num_non_pim_reqs[b] == 0) {
          avg_req_latency = m_config->tCCDL;
        } else {
          avg_req_latency = m_non_pim_batch_dur[b] / m_num_non_pim_reqs[b];
        }
        m_max_non_pim_reqs[b] = (m_pim_batch_dur * \
            (m_config->max_pim_slowdown - 1)) / avg_req_latency;
      }
    }

    if (have_reads || have_writes) {
      m_num_non_pim_reqs.assign(m_config->nbk, 0);

      m_non_pim_req_start_time.assign(m_config->nbk, 0);
      m_non_pim_batch_dur.assign(m_config->nbk, 0);

      m_pim_batch_start_time = 0;

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;
#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to non-PIM mode\n");
#endif
    }
  }

  else {
    if (have_pim) {
      bool pim_threshold_exceeded = false;

      for (unsigned b = 0; b < m_config->nbk; b++) {
        if (m_num_non_pim_reqs[b] > m_max_non_pim_reqs[b]) {
          pim_threshold_exceeded = true;
          break;
        }
      }

      if (pim_threshold_exceeded || !(have_reads || have_writes)) {
        m_dram->mode = PIM_MODE;
        m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
        printf("DRAM: Switching to PIM mode\n");
#endif
      }
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *i2_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (m_non_pim_req_start_time[bank] > 0) {
    m_non_pim_batch_dur[bank] += m_dram->m_dram_cycle -
                                 m_non_pim_req_start_time[bank];
    m_num_non_pim_reqs[bank]++;
  }

  if (req) {
    m_non_pim_req_start_time[bank] = m_dram->m_dram_cycle;
  } else {
    m_non_pim_req_start_time[bank] = 0;
  }

  return req;
}

dram_req_t *i2_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_last_pim_row = req->row;
  }

  return req;
}
