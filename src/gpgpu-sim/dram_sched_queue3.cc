#include "dram_sched.h"
#include "dram_sched_queue3.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

queue3_scheduler::queue3_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats)
{
  m_mem_queue_size = m_config->gpgpu_frfcfs_dram_sched_queue_size + \
                     (m_config->seperate_write_queue_enabled ? \
                      m_config->gpgpu_frfcfs_dram_write_queue_size : 0);
  m_pim_queue_size = m_config->gpgpu_frfcfs_dram_pim_queue_size;

  m_last_pim_row = 0;
  m_pim_batch_start_time = 0;
}

void queue3_scheduler::update_mode() {
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

    if (((is_batch_over && queues_imbalanced()) || !have_pim) && \
        (have_reads || have_writes)) {
      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("%llu: DRAM (%d): Switching to non-PIM mode\n",
          m_dram->m_dram_cycle, m_dram->id);
#endif
    }
  }

  else {
    if ((queues_imbalanced() || !(have_reads || have_writes)) && have_pim) {
      m_pim_batch_start_time = 0;

      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("%llu: DRAM (%d): Switching to PIM mode\n",
          m_dram->m_dram_cycle, m_dram->id);
#endif
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *queue3_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_batch_start_time == 0) {
      m_pim_batch_start_time = m_dram->m_dram_cycle;
    }

    m_last_pim_row = req->row;
  }

  return req;
}

bool queue3_scheduler::queues_imbalanced()
{
  float pim_occupancy = m_num_pim_pending / m_pim_queue_size;
  float mem_occupancy = (m_num_pending + m_num_write_pending) / \
                        m_mem_queue_size;

#ifdef DRAM_SCHED_VERIFY
  printf("%llu: DRAM (%d): mem_occupancy=%f\%, pim_occupancy=%f\%\n",
      m_dram->m_dram_cycle, m_dram->id, mem_occupancy * 100,
      pim_occupancy * 100);
#endif

  if (m_dram->mode == PIM_MODE) {
    return ((mem_occupancy-pim_occupancy) > DRAM_SCHED_QUEUE3_MAX_QUEUE_DIFF);
  } else {
    return ((pim_occupancy-mem_occupancy) > DRAM_SCHED_QUEUE3_MAX_QUEUE_DIFF);
  }
}
