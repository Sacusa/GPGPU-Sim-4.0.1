#include "dram_sched.h"
#include "dram_sched_i4a.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

i4a_scheduler::i4a_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_num_non_pim_reqs.assign(m_config->nbk, 0);
  m_max_non_pim_reqs.assign(m_config->nbk, 0);

  m_non_pim_req_start_time.assign(m_config->nbk, 0);
  m_non_pim_batch_dur.assign(m_config->nbk, 0);

  m_last_pim_row = 0;

  m_pim_batch_start_time = 0;
  m_pim_batch_dur = 0;

  m_finished_pim_batches = 0;
  m_max_pim_batches = 1;
  m_prev_max_pim_batches = 1;

  m_stable_state = true;
  m_next_update_cycle = I4A_INTERVAL_LENGTH;
  m_prev_update_cycle = 0;
  m_exploratory_phase_start_cycle = 0;

  m_stable_non_pim_arrival_rate = 0;
  m_curr_non_pim_arrival_rate = 0;
  m_non_pim_arrival_rate_tolerance = 0.05;

  m_curr_non_pim_completion_rate = 0;
}

void i4a_scheduler::update_mode() {
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

      m_finished_pim_batches++;
    }

    if ((m_finished_pim_batches >= m_max_pim_batches) || !have_pim) {
      if (have_reads || have_writes) {
        m_num_non_pim_reqs.assign(m_config->nbk, 0);

        m_non_pim_req_start_time.assign(m_config->nbk, 0);
        m_non_pim_batch_dur.assign(m_config->nbk, 0);

        m_pim_batch_start_time = 0;

        m_dram->mode = READ_MODE;
        m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
        printf("DRAM (%d): Switching to non-PIM mode\n", m_dram->id);
        printf("           Executed %lld PIM batches\n",
            m_finished_pim_batches);
#endif
      }

      else if (m_finished_pim_batches == m_max_pim_batches) {
        end_exploratory_phase();
      }
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

        m_finished_pim_batches = 0;

#ifdef DRAM_SCHED_VERIFY
        printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
#endif

        end_exploratory_phase();

        if (m_dram->m_dram_cycle > m_next_update_cycle) {
#ifdef DRAM_SCHED_VERIFY
        printf("DRAM (%d): Updating at cycle %lld\n", m_dram->id,
            m_dram->m_dram_cycle);
#endif
          m_curr_non_pim_arrival_rate /= (m_dram->m_dram_cycle - \
              m_prev_update_cycle);

          float non_pim_arrival_rate_change = \
              fabs(m_curr_non_pim_arrival_rate - \
                  m_stable_non_pim_arrival_rate) / \
              m_stable_non_pim_arrival_rate;

          if (non_pim_arrival_rate_change > m_non_pim_arrival_rate_tolerance) {
#ifdef DRAM_SCHED_VERIFY
            printf("           Unstable phase. Arrival rate change / "
                "tolerance = %f/%f\n", non_pim_arrival_rate_change,
                m_non_pim_arrival_rate_tolerance);
#endif
            m_stable_state = false;

            m_prev_max_pim_batches = m_max_pim_batches;
            m_max_pim_batches = 1;
            m_stable_non_pim_arrival_rate = m_curr_non_pim_arrival_rate;

            m_non_pim_completion_rate.clear();
          }

          else {
            m_non_pim_arrival_rate_tolerance -= 0.00125;
          }

          m_prev_update_cycle = m_dram->m_dram_cycle;
          m_curr_non_pim_arrival_rate = 0;
          m_next_update_cycle = m_dram->m_dram_cycle + I4A_INTERVAL_LENGTH;
        }
      }
    }
  }

  dram_scheduler::update_mode();
}

void i4a_scheduler::add_req(dram_req_t *req) {
  dram_scheduler::add_req(req);

  if (!req->data->is_pim()) {
    m_curr_non_pim_arrival_rate++;
  }

  if (((m_num_pending >= (m_config->gpgpu_frfcfs_dram_sched_queue_size * \
            I4A_MAX_MEM_REQ_OCCUPANCY)) || \
       (m_num_write_pending >= (m_config->gpgpu_frfcfs_dram_write_queue_size *\
            I4A_MAX_MEM_REQ_OCCUPANCY))) && !m_stable_state) {
    m_stable_state = true;

    float max_completion_rate = 0;

    for (int i = 0; i < m_non_pim_completion_rate.size(); i++) {
      if (m_non_pim_completion_rate[i] > max_completion_rate) {
        max_completion_rate = m_non_pim_completion_rate[i];
        m_max_pim_batches = (unsigned) pow(2, i);
      }
    }

#ifdef DRAM_SCHED_VERIFY
    printf("DRAM (%d): Exploration finished at cycle %lld\n", m_dram->id,
        m_dram->m_dram_cycle);
    printf("           Stable config = %d batches\n", m_max_pim_batches);
#endif

    if (m_max_pim_batches == m_prev_max_pim_batches) {
      m_non_pim_arrival_rate_tolerance += 0.02;

#ifdef DRAM_SCHED_VERIFY
      printf("           Increasing tolerance to %f\n",
          m_non_pim_arrival_rate_tolerance);
#endif
    }
  }
}

dram_req_t *i4a_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (m_non_pim_req_start_time[bank] > 0) {
    m_non_pim_batch_dur[bank] += m_dram->m_dram_cycle -
                                 m_non_pim_req_start_time[bank];
    m_num_non_pim_reqs[bank]++;
  }

  if (req) {
    m_non_pim_req_start_time[bank] = m_dram->m_dram_cycle;
    m_curr_non_pim_completion_rate++;
  } else {
    m_non_pim_req_start_time[bank] = 0;
  }

  return req;
}

dram_req_t *i4a_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_last_pim_row = req->row;
  }

  return req;
}

void i4a_scheduler::end_exploratory_phase() {
  m_curr_non_pim_completion_rate /= (m_dram->m_dram_cycle - \
      m_exploratory_phase_start_cycle);
  m_exploratory_phase_start_cycle = m_dram->m_dram_cycle;

  if (!m_stable_state) {
    m_non_pim_completion_rate.push_back(m_curr_non_pim_completion_rate);
    m_max_pim_batches *= 2;

    if (m_max_pim_batches > I4A_MAX_PIM_BATCH_SIZE) {
      m_stable_state = true;

      float max_completion_rate = 0;

      for (int i = 0; i < m_non_pim_completion_rate.size(); i++) {
        if (m_non_pim_completion_rate[i] > max_completion_rate) {
          max_completion_rate = m_non_pim_completion_rate[i];
          m_max_pim_batches = (unsigned) pow(2, i);
        }
      }

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Exploration finished at cycle %lld\n", m_dram->id,
          m_dram->m_dram_cycle);
      printf("           Stable config = %d batches\n", m_max_pim_batches);
#endif

      if (m_max_pim_batches == m_prev_max_pim_batches) {
        m_non_pim_arrival_rate_tolerance += 0.02;

#ifdef DRAM_SCHED_VERIFY
        printf("           Increasing tolerance to %f\n",
            m_non_pim_arrival_rate_tolerance);
#endif
      }
    }
  }

  m_curr_non_pim_completion_rate = 0;
}
