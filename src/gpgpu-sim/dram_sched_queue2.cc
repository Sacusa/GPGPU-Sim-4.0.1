#include "dram_sched.h"
#include "dram_sched_queue2.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

queue2_scheduler::queue2_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{
  m_mem_queue_size = m_config->gpgpu_frfcfs_dram_sched_queue_size + \
                     (m_config->seperate_write_queue_enabled ? \
                      m_config->gpgpu_frfcfs_dram_write_queue_size : 0);
  m_pim_queue_size = m_config->gpgpu_frfcfs_dram_pim_queue_size;
  m_prev_ave_mrqs = 0;
  m_prev_ave_pim_mrqs = 0;

  m_last_pim_row = 0;
  m_pim_batch_start_time = 0;
  m_pim_phase_dur = 0;
  m_mem_phase_start_time = 0;
  m_next_update_cycle = DRAM_SCHED_QUEUE2_PHASE_LENGTH;

  m_finished_pim_batches = 0;
  m_min_pim_batches = 1;
  m_mem_time_ratio = 1.0;

  m_min_pim_batches_hi = m_config->dram_sched_queue_max_pim_batches;
  m_mem_time_ratio_hi = m_config->dram_sched_queue_mem_time_ratio_high;
  m_mem_time_ratio_lo = m_config->dram_sched_queue_mem_time_ratio_low;
}

void queue2_scheduler::update_mode() {
  bool have_reads = false, have_writes = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_reads = have_reads || !m_queue[b].empty();
    if (m_config->seperate_write_queue_enabled) {
      have_writes = have_writes || !m_write_queue[b].empty();
    }
  }

  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    // Batch is over if the next PIM request will access a new row
    bool is_batch_over = (m_pim_batch_start_time > 0) && (!have_pim ||
                          (m_pim_queue->front()->row != m_last_pim_row));

    if (is_batch_over) {
      update_switch_conditions();

      unsigned long long batch_exec_time = m_dram->m_dram_cycle - \
                                           m_pim_batch_start_time;
      m_pim_batch_start_time = 0;

      m_finished_pim_batches++;
      if (m_finished_pim_batches <= m_min_pim_batches) {
        m_pim_phase_dur += batch_exec_time;
      }
    }

    if (((m_finished_pim_batches >= m_config->min_pim_batches) || \
          !have_pim) && (have_reads || have_writes)) {
      m_finished_pim_batches = 0;

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("%llu: DRAM (%d): Switching to non-PIM mode\n",
          m_dram->m_dram_cycle, m_dram->id);
#endif
    }
  }

  else {
    update_switch_conditions();

    if (have_pim) {
      unsigned long long switch_cycle = m_mem_phase_start_time + \
                                        (m_pim_phase_dur * m_mem_time_ratio);

      if (((m_mem_phase_start_time > 0) && \
           (m_dram->m_dram_cycle > switch_cycle)) || \
          !(have_reads || have_writes)) {
        m_pim_phase_dur = 0;
        m_mem_phase_start_time = 0;

        m_dram->mode = PIM_MODE;
        m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
        printf("%llu: DRAM (%d): Switching to PIM mode\n",
            m_dram->m_dram_cycle, m_dram->id);
#endif
      }
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *queue2_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req && (m_mem_phase_start_time == 0)) {
    m_mem_phase_start_time = m_dram->m_dram_cycle;
  }

  return req;
}

dram_req_t *queue2_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_last_pim_row = req->row;
  }

  return req;
}

void queue2_scheduler::update_switch_conditions()
{
  if (m_dram->m_dram_cycle >= m_next_update_cycle) {
    // calculate the exact phase length
    unsigned long long phase_length = m_dram->m_dram_cycle - \
                                      m_next_update_cycle + \
                                      DRAM_SCHED_QUEUE2_PHASE_LENGTH;

    // calculate the new queue length averages and objective function
    unsigned pim_pending = m_dram->ave_pim_mrqs;
    unsigned mem_pending = m_dram->ave_mrqs - pim_pending;

    float queue_avg = ((float) (mem_pending - m_prev_ave_mrqs) / \
                       phase_length) / m_mem_queue_size;
    float queue_avg_pim = ((float) (pim_pending - m_prev_ave_pim_mrqs) / \
                           phase_length) / m_pim_queue_size;
    float objective = fabs(queue_avg - queue_avg_pim);
    
#ifdef DRAM_SCHED_VERIFY
    printf("%llu: DRAM (%d): Updating switch conditions\n",
        m_dram->m_dram_cycle, m_dram->id);
    if (queue_avg != queue_avg_pim) {
      if (queue_avg > queue_avg_pim) {
        printf("%llu: DRAM (%d): Delta=%f; queue_avg higher\n",
            m_dram->m_dram_cycle, m_dram->id, objective);
      } else {
        printf("%llu: DRAM (%d): Delta=%f; queue_avg_pim higher\n",
            m_dram->m_dram_cycle, m_dram->id, objective);
      }
    }
#endif

    if (objective > 0.05) {
      if (queue_avg > queue_avg_pim) {
        if (objective <= 0.1) {
          m_mem_time_ratio = std::min(m_mem_time_ratio + 0.2f,
              m_mem_time_ratio_hi);

          if ((m_mem_time_ratio == m_mem_time_ratio_hi) && \
              (m_min_pim_batches > 1)) {
            m_min_pim_batches -= 1;
            m_mem_time_ratio = (m_mem_time_ratio_hi - m_mem_time_ratio_lo) / 2;
          }
        }

        else if (objective <= 0.5) {
          m_min_pim_batches = std::max(m_min_pim_batches - 1, 1u);
          m_mem_time_ratio = std::min(m_mem_time_ratio + 0.5f,
              m_mem_time_ratio_hi);
        }

        else {
          m_min_pim_batches = 1;
          m_mem_time_ratio = std::min(m_mem_time_ratio + 1.0f,
              m_mem_time_ratio_hi);
        }
      }

      else {
        if (objective <= 0.1) {
          m_mem_time_ratio = std::max(m_mem_time_ratio - 0.2f,
              m_mem_time_ratio_lo);

          if ((m_mem_time_ratio == m_mem_time_ratio_lo) && \
              (m_min_pim_batches < m_min_pim_batches_hi)) {
            m_min_pim_batches += 1;
            m_mem_time_ratio = (m_mem_time_ratio_hi - m_mem_time_ratio_lo) / 2;
          }
        }

        else if (objective <= 0.5) {
          m_min_pim_batches = std::min(m_min_pim_batches + 1,
              m_min_pim_batches_hi);
          m_mem_time_ratio = std::max(m_mem_time_ratio - 0.5f,
              m_mem_time_ratio_lo);
        }

        else {
          m_min_pim_batches = std::min(m_min_pim_batches * 2,
              m_min_pim_batches_hi);
          m_mem_time_ratio = m_mem_time_ratio_lo;
        }
      }

#ifdef DRAM_SCHED_VERIFY
      printf("%llu: DRAM (%d): mem_ratio=%f, pim_batches=%d\n",
          m_dram->m_dram_cycle, m_dram->id, m_mem_time_ratio,
          m_min_pim_batches);
#endif
    }

    m_prev_ave_mrqs = mem_pending;
    m_prev_ave_pim_mrqs = pim_pending;
    m_next_update_cycle = m_dram->m_dram_cycle +DRAM_SCHED_QUEUE2_PHASE_LENGTH;
  }
}
