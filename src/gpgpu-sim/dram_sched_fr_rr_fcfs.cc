#include "dram_sched.h"
#include "dram_sched_fr_rr_fcfs.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

fr_rr_fcfs_scheduler::fr_rr_fcfs_scheduler(const memory_config *config,
        dram_t *dm, memory_stats_t *stats) :
    dram_scheduler(config, dm, stats) {
  m_last_pim_row = 0;
  m_bank_switch_to_pim.resize(m_config->nbk, false);

  m_num_exec_pim = 0;
  m_num_exec_mem_per_bank.resize(m_config->nbk, 0);

  m_bank_pending_mem_requests.resize(m_config->nbk, 0);
}

void fr_rr_fcfs_scheduler::add_req(dram_req_t *req) {
  dram_scheduler::add_req(req);

  if (!req->data->is_pim()) {
    m_bank_pending_mem_requests[req->bk]++;
  }
}

void fr_rr_fcfs_scheduler::update_mode() {
  enum memory_mode prev_mode = m_dram->mode;

  // Switch to MEM mode if
  if ((m_dram->mode == PIM_MODE) && (m_num_pending > 0)) {
    if (m_num_pim_pending == 0) {
      // 1) There are no more PIM requests
      m_dram->mode = READ_MODE;
      m_pim2mem_switch_reason.push_back(FR_RR_FCFS_OUT_OF_REQUESTS);
    } else {
      // 2) PIM has row buffer conflict
      dram_req_t *req = m_pim_queue->front();
      if ((m_last_pim_row != 0) && (req->row != m_last_pim_row)) {
        m_dram->mode = READ_MODE;
        m_pim2mem_switch_reason.push_back(FR_RR_FCFS_ROW_BUFFER_CONFLICT);
      }
    }
  }

  // Switch to PIM mode if
  else if ((m_dram->mode != PIM_MODE) && (m_num_pim_pending > 0)) {
    if (m_num_pending == 0) {
      // 1) There are no more MEM requests
      m_dram->mode = PIM_MODE;
      m_mem2pim_switch_reason.push_back(FR_RR_FCFS_OUT_OF_REQUESTS);
    } else {
      // 2) Every bank has had a row buffer conflict
      bool switch_to_pim = true;

      for (unsigned b = 0; b < m_config->nbk; b++) {
        if (!m_bank_switch_to_pim[b]) {
          // If there are no pending requests at the bank, we assume it can
          // switch
          bool can_bank_switch = m_bank_pending_mem_requests[b] == 0;

          // Otherwise, we perform the row buffer hit test once a request has
          // been issued
          if ((m_bank_pending_mem_requests[b] > 0) && \
              (m_dram->bk[b]->mrq != NULL)) {
            can_bank_switch = !is_next_req_hit(b, m_dram->bk[b]->curr_row,
                                               m_dram->mode);
          }

          m_bank_switch_to_pim[b] = can_bank_switch;
        }

        switch_to_pim = switch_to_pim && m_bank_switch_to_pim[b];
      }

      if (switch_to_pim) {
        m_dram->mode = PIM_MODE;
        m_mem2pim_switch_reason.push_back(FR_RR_FCFS_ROW_BUFFER_CONFLICT);
      }
    }
  }

  if (m_dram->mode != prev_mode) {
    if (prev_mode == PIM_MODE) {
      m_dram->pim2nonpimswitches++;

      m_pim_requests_issued.push_back(m_num_exec_pim);  // Stat

      m_last_pim_row = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to non-PIM mode\n");
#endif
    } else {
      m_dram->nonpim2pimswitches++;

      m_max_mem_requests_issued_at_any_bank.push_back(*max_element(
                  m_num_exec_mem_per_bank.begin(),
                  m_num_exec_mem_per_bank.end()));  // Stat

      std::fill(m_bank_switch_to_pim.begin(), m_bank_switch_to_pim.end(),
          false);
      std::fill(m_num_exec_mem_per_bank.begin(), m_num_exec_mem_per_bank.end(),
          0);
      m_num_exec_pim = 0;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }
}

dram_req_t *fr_rr_fcfs_scheduler::schedule(unsigned bank, unsigned curr_row) {
  if (m_bank_switch_to_pim[bank]) {
    // If the bank has alread had a row buffer conflict, then it just waits
    // until the mode is switched to PIM
    return NULL;
  }

  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req) {
    m_num_exec_mem_per_bank[bank]++;
    m_bank_pending_mem_requests[bank]--;
  }

  return req;
}

dram_req_t *fr_rr_fcfs_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    m_last_pim_row = req->row;
    m_num_exec_pim++;
  }

  return req;
}
