#include "dram_sched.h"
#include "dram_sched_paws.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

paws_scheduler::paws_scheduler(const memory_config *config, dram_t *dm,
        memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_pim_queue_it =
      new std::list<std::list<dram_req_t *>::iterator>[m_config->nbk];
  m_last_pim_row = 0;
  m_bank_switch_to_pim.resize(m_config->nbk, false);

  m_num_exec_pim = 0;
  m_max_exec_mem_per_bank = 0;
  m_num_exec_mem_per_bank.resize(m_config->nbk, 0);

  m_bank_pim_stall_time.resize(m_config->nbk, 0);
  m_bank_pim_waste_time.resize(m_config->nbk, 0);
  m_bank_pending_mem_requests.resize(m_config->nbk, 0);

  m_mem2pim_switch_ready_timestamp = 0;
}

void paws_scheduler::add_req(dram_req_t *req) {
  if (req->data->is_pim()) {
    assert(m_num_pim_pending < m_config->gpgpu_frfcfs_dram_pim_queue_size);
    m_num_pim_pending++;

    for (unsigned int b = 0; b < m_config->nbk; b++) {
      m_queue[b].push_front(req);
      std::list<dram_req_t *>::iterator ptr = m_queue[b].begin();
      m_pim_queue_it[b].push_front(ptr);
    }

    if (m_dram->first_pim_insert_timestamp == 0) {
      m_dram->first_pim_insert_timestamp = m_dram->m_gpu->gpu_sim_cycle +
                                           m_dram->m_gpu->gpu_tot_sim_cycle;
    }
  }

  else {
    assert(m_num_pending < m_config->gpgpu_frfcfs_dram_sched_queue_size);
    m_num_pending++;

    m_queue[req->bk].push_front(req);
    std::list<dram_req_t *>::iterator ptr = m_queue[req->bk].begin();
    m_bins[req->bk][req->row].push_front(ptr);  // newest reqs to the front

    m_bank_pending_mem_requests[req->bk]++;

    if (m_dram->first_non_pim_insert_timestamp == 0) {
      m_dram->first_non_pim_insert_timestamp = m_dram->m_gpu->gpu_sim_cycle +
                                              m_dram->m_gpu->gpu_tot_sim_cycle;
    }
  }
}

void paws_scheduler::update_mode() {
  enum memory_mode prev_mode = m_dram->mode;

  // Switch to MEM mode if
  if (m_dram->mode == PIM_MODE) {
    bool threshold_exceeded = (m_config->frfcfs_cap > 0) && \
                              (m_num_exec_pim > m_config->frfcfs_cap);

    if (threshold_exceeded && (m_num_pending > 0)) {
      // 1) Executed PIM requests have crossed a threshold, or
      m_dram->mode = READ_MODE;
      m_pim2mem_switch_reason.push_back(PAWS_CAP_EXCEEDED);
    } else {
      if (m_num_pim_pending == 0) {
        // 2) There are no more PIM requests and there are MEM requests, or
        if (m_num_pending > 0) {
          m_dram->mode = READ_MODE;
          m_pim2mem_switch_reason.push_back(PAWS_OUT_OF_REQUESTS);
        }
      } else {
        dram_req_t *req = *(m_pim_queue_it[0].back());
        if ((m_last_pim_row != 0) && (req->row != m_last_pim_row)) {
          for (unsigned b = 0; b < m_config->nbk; b++) {
            if (!m_queue[b].empty() && !m_queue[b].back()->data->is_pim()) {
              // 3) PIM has row buffer miss and the oldest request is MEM
              m_dram->mode = READ_MODE;
              m_pim2mem_switch_reason.push_back(PAWS_OLDEST_FIRST);
              break;
            }
          }
        }
      }
    }
  }

  // Switch to PIM mode if
  else {
    bool threshold_exceeded = false;

    if (m_max_exec_mem_per_bank > 0) {
      for (unsigned b = 0; b < m_config->nbk; b++) {
        if (m_num_exec_mem_per_bank[b] > m_max_exec_mem_per_bank) {
          threshold_exceeded = true;
          break;
        }
      }
    }

    if (threshold_exceeded && (m_num_pim_pending > 0)) {
      // 1) Executed MEM requests have crossed a threshold, or
      m_dram->mode = PIM_MODE;
      m_mem2pim_switch_reason.push_back(PAWS_CAP_EXCEEDED);
    } else {
      if (m_num_pending == 0) {
        // 2) There are no more MEM requests and there are PIM requests, or
        if (m_num_pim_pending > 0) {
          m_dram->mode = PIM_MODE;
          m_mem2pim_switch_reason.push_back(PAWS_OUT_OF_REQUESTS);
        }
      } else {
        // 3) Every bank has a row buffer miss and PIM is the oldest request
        bool switch_to_pim = true;

        bool at_least_one_bank_can_switch = false;
        std::vector<bool> has_mem_requests;

        for (unsigned b = 0; b < m_config->nbk; b++) {
          // If there are no pending requests at the bank, we assume it can
          // switch
          bool can_bank_switch = m_bank_pending_mem_requests[b] == 0;

          // Otherwise, we perform the row buffer hit test once a request has
          // been issued
          if ((m_bank_pending_mem_requests[b] > 0) && \
              (m_dram->bk[b]->mrq != NULL)) {
            can_bank_switch = !is_next_req_hit(b, m_dram->bk[b]->curr_row,
                                               m_dram->mode) && \
                              m_queue[b].back()->data->is_pim();
          }

          m_bank_switch_to_pim[b] = m_bank_switch_to_pim[b] || can_bank_switch;
          switch_to_pim = switch_to_pim && m_bank_switch_to_pim[b];
          at_least_one_bank_can_switch = at_least_one_bank_can_switch || \
                                         m_bank_switch_to_pim[b];
        }

        if (switch_to_pim) {
            m_dram->mode = PIM_MODE;
            m_mem2pim_switch_reason.push_back(PAWS_OLDEST_FIRST);
        }

        else if (at_least_one_bank_can_switch) {
          if (m_mem2pim_switch_ready_timestamp == 0) {
            m_mem2pim_switch_ready_timestamp = m_dram->m_gpu->gpu_sim_cycle +
              m_dram->m_gpu->gpu_tot_sim_cycle;
          }

          for (unsigned b = 0; b < m_config->nbk; b++) {
            if (m_bank_switch_to_pim[b]) {
              m_bank_pim_stall_time[b]++;
              if (m_bank_pending_mem_requests[b] > 0) {
                m_bank_pim_waste_time[b]++;
              }
            }
          }
        }
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

      m_max_exec_mem_per_bank = \
          std::min(m_config->frfcfs_cap, m_num_exec_pim) * \
          m_config->max_pim_slowdown;
      std::fill(m_num_exec_mem_per_bank.begin(), m_num_exec_mem_per_bank.end(),
          0);

      m_num_exec_pim = 0;

      m_mem2pim_switch_latency.push_back(m_dram->m_gpu->gpu_sim_cycle +
          m_dram->m_gpu->gpu_tot_sim_cycle - m_mem2pim_switch_ready_timestamp);
      m_mem2pim_switch_ready_timestamp = 0;

      m_mem_cap.push_back(m_max_exec_mem_per_bank);  // Stat

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }
}

dram_req_t *paws_scheduler::schedule(unsigned bank, unsigned curr_row) {
  bool rowhit = true;
  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (m_current_last_row[bank] == NULL) {
    if (m_current_queue[bank].empty()) return NULL;

    std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >::iterator
        bin_ptr = m_current_bins[bank].find(curr_row);
    if (bin_ptr == m_current_bins[bank].end()) {
      dram_req_t *req = m_current_queue[bank].back();

      if (req->data->is_pim()) {
        if (m_bank_pending_mem_requests[bank] == 0) {
          return NULL;
        }

        std::list<dram_req_t*>::reverse_iterator rit;
        for (rit = m_current_queue[bank].rbegin();
                rit != m_current_queue[bank].rend(); rit++){
          if (!(*rit)->data->is_pim()) {
            req = *rit;
            break;
          }
        }
      }

      bin_ptr = m_current_bins[bank].find(req->row);
      assert(bin_ptr !=
             m_current_bins[bank].end());  // where did the request go???
      m_current_last_row[bank] = &(bin_ptr->second);
      data_collection(bank);
      rowhit = false;
    } else {
      m_current_last_row[bank] = &(bin_ptr->second);
      rowhit = true;
    }
  }
  std::list<dram_req_t *>::iterator next = m_current_last_row[bank]->back();
  dram_req_t *req = (*next);

  m_num_exec_mem_per_bank[bank]++;

  // rowblp stats
  m_dram->access_num++;
  bool is_write = req->data->is_write();
  if (is_write)
    m_dram->write_num++;
  else
    m_dram->read_num++;

  if (rowhit) {
    m_dram->hits_num++;
    if (is_write)
      m_dram->hits_write_num++;
    else
      m_dram->hits_read_num++;
  }

  m_stats->concurrent_row_access[m_dram->id][bank]++;
  m_stats->row_access[m_dram->id][bank]++;
  m_current_last_row[bank]->pop_back();

  m_current_queue[bank].erase(next);
  if (m_current_last_row[bank]->empty()) {
    m_current_bins[bank].erase(req->row);
    m_current_last_row[bank] = NULL;
  }
  m_bank_pending_mem_requests[bank]--;
#ifdef DEBUG_FAST_IDEAL_SCHED
  if (req)
    printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n",
           (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row);
#endif

  assert(req != NULL && m_num_pending != 0);
  m_num_pending--;

  return req;
}

dram_req_t *paws_scheduler::schedule_pim() {
  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (m_pim_queue_it[0].empty()) { return NULL; }

  dram_req_t *req = *(m_pim_queue_it[0].back());

  for (unsigned int bank = 0; bank < m_config->nbk; bank++) {
    unsigned curr_row = m_dram->bk[bank]->curr_row;

    std::list<dram_req_t *>::iterator next = m_pim_queue_it[bank].back();
    dram_req_t *bank_req = *(next);

    bool rowhit = curr_row == bank_req->row;
    if (!rowhit) { data_collection(bank); }

    // rowblp stats
    m_dram->access_num++;
    m_dram->pim_num++;

    if (rowhit) {
      m_dram->hits_num++;
      m_dram->hits_pim_num++;
    }

    m_stats->concurrent_row_access[m_dram->id][bank]++;
    m_stats->row_access[m_dram->id][bank]++;
    m_current_queue[bank].erase(next);
    m_pim_queue_it[bank].pop_back();

#ifdef DEBUG_FAST_IDEAL_SCHED
    if (bank_req)
      printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n",
             (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row);
#endif
  }

  m_last_pim_row = req->row;
  m_num_exec_pim++;

  assert(req != NULL && m_num_pim_pending != 0);
  m_num_pim_pending--;

  return req;
}
