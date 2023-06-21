#include "dram_sched.h"
#include "dram_sched_i1.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

i1_scheduler::i1_scheduler(const memory_config *config, dram_t *dm,
    memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_reqs_per_bank.assign(m_config->nbk, 0);
  m_max_req_per_bank = 0;

  m_pim_curr_transaction_start_time = 0;
  m_pim_last_transaction_dur = 0;

  m_is_last_pim_store = false;
}

void i1_scheduler::update_mode() {
  bool have_reads = false, have_writes = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    have_reads = have_reads || !m_queue[b].empty();
    if (m_config->seperate_write_queue_enabled) {
      have_writes = have_writes || !m_write_queue[b].empty();
    }
  }

  bool have_pim = !m_pim_queue->empty();

  if (m_dram->mode == PIM_MODE) {
    // Transaction is over if the previous request was a store and:
    // 1) there are no more instructions left, or
    // 2) the next instruction is not a store (indicating the start of a new
    //    transaction).
    bool is_transaction_over = m_is_last_pim_store && (!have_pim ||
      !is_pim_req_store(m_pim_queue->front()));

    if (is_transaction_over) {
      m_reqs_per_bank.assign(m_config->nbk, 0);

      m_pim_last_transaction_dur = m_dram->m_dram_cycle -
        m_pim_curr_transaction_start_time;
      m_pim_curr_transaction_start_time = 0;

      m_max_req_per_bank = (m_pim_last_transaction_dur * 2) / m_config->tRC;

      m_is_last_pim_store = false;

      if (have_reads || have_writes) {
        m_dram->mode = READ_MODE;
        m_dram->num_mode_switches++;

#ifdef DRAM_VERIFY
        printf("DRAM: Switching to non-PIM mode\n");
#endif
      }
    }

  } else {
    bool pim_threshold_exceeded = false;

    if (have_pim) {
      for (unsigned b = 0; b < m_config->nbk; b++) {
        if (m_reqs_per_bank[b] > m_max_req_per_bank) {
          pim_threshold_exceeded = true;
          break;
        }
      }
    }

    if (pim_threshold_exceeded || !(have_reads || have_writes)) {
      m_dram->mode = PIM_MODE;
      m_dram->num_mode_switches++;

#ifdef DRAM_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }

  dram_scheduler::update_mode();
}

dram_req_t *i1_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req) {
    m_reqs_per_bank[bank]++;
  }

  return req;
}

dram_req_t *i1_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    if (m_pim_curr_transaction_start_time == 0) {
      m_pim_curr_transaction_start_time = m_dram->m_dram_cycle;
    }

    m_is_last_pim_store = is_pim_req_store(req);
  }

  return req;
}

bool i1_scheduler::is_pim_req_store(dram_req_t *req) {
  return req->nbytes == 16;
}
