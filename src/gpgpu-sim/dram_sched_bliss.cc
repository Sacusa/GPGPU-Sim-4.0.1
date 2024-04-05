#include "dram_sched.h"
#include "dram_sched_bliss.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

bliss_scheduler::bliss_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_pim_queue_it =
      new std::list<std::list<dram_req_t *>::iterator>[m_config->nbk];
  m_last_pim_row = 0;

  m_requests_served = 0;
  m_prev_request_type = REQ_NONE;

  is_pim_blacklisted = false;
  is_mem_blacklisted = false;
}

void bliss_scheduler::add_req(dram_req_t *req) {
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

    if (m_dram->first_non_pim_insert_timestamp == 0) {
      m_dram->first_non_pim_insert_timestamp = m_dram->m_gpu->gpu_sim_cycle +
                                              m_dram->m_gpu->gpu_tot_sim_cycle;
    }
  }
}

void bliss_scheduler::update_mode() {
  unsigned num_mem_pending = m_num_pending;
  enum memory_mode prev_mode = m_dram->mode;

  if ((m_dram->m_dram_cycle % m_config->bliss_clearing_interval) == 0) {
    m_requests_served = 0;
    m_prev_request_type = REQ_NONE;
    is_pim_blacklisted = false;
    is_mem_blacklisted = false;

#ifdef DRAM_SCHED_VERIFY
    printf("DRAM (%d): Reached clearing interval\n", m_dram->id);
#endif
  }

  if (is_pim_blacklisted == is_mem_blacklisted) {
    /******************
     * FR-FCFS policy *
     ******************/

    // Switch to MEM mode if
    if (m_dram->mode == PIM_MODE) {
      if (m_num_pim_pending == 0) {
        // 1) There are no more PIM requests and there are MEM requests, or
        if (num_mem_pending > 0) {
          m_dram->mode = READ_MODE;
        }
      } else {
        dram_req_t *req = *(m_pim_queue_it[0].back());
        if (req->row != m_last_pim_row) {
          for (unsigned b = 0; b < m_config->nbk; b++) {
            if (!m_queue[b].empty() && !m_queue[b].back()->data->is_pim()) {
              // 2) PIM has row buffer miss and the oldest request is MEM
              m_dram->mode = READ_MODE;
            }
          }
        }
      }
    }

    // Switch to PIM mode if
    else {
      if (num_mem_pending == 0) {
        // 1) There are no more MEM requests and there are PIM requests, or
        if (m_num_pim_pending > 0) {
          m_dram->mode = PIM_MODE;
        }
      } else {
        bool switch_to_pim = true;

        for (unsigned b = 0; b < m_config->nbk; b++) {
          switch_to_pim = switch_to_pim && \
                          !is_next_req_hit(b, m_dram->bk[b]->curr_row,
                                           m_dram->mode) && \
                          !m_queue[b].empty() && \
                          m_queue[b].back()->data->is_pim();
        }

        // 2) Every bank has a row buffer miss and PIM is the oldest request
        if (switch_to_pim) { m_dram->mode = PIM_MODE; }
      }
    }
  }

  else if (is_pim_blacklisted) {
    if (num_mem_pending > 0) { m_dram->mode = READ_MODE; }
    else                     { m_dram->mode = PIM_MODE; }
  }

  else {
    if (m_num_pim_pending > 0) { m_dram->mode = PIM_MODE; }
    else                       { m_dram->mode = READ_MODE; }
  }

  if (m_dram->mode != prev_mode) {
    if (prev_mode == PIM_MODE) {
      m_dram->pim2nonpimswitches++;
#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to non-PIM mode\n", m_dram->id);
#endif
    } else {
      m_dram->nonpim2pimswitches++;
      m_last_pim_row = 0;
#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
#endif
    }
  }
}

dram_req_t *bliss_scheduler::schedule(unsigned bank, unsigned curr_row) {
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

      if (req->data->is_pim()) { return NULL; }

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

  update_blacklist(REQ_MEM);

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
#ifdef DEBUG_FAST_IDEAL_SCHED
  if (req)
    printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n",
           (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row);
#endif

  assert(req != NULL && m_num_pending != 0);
  m_num_pending--;

  return req;
}

dram_req_t *bliss_scheduler::schedule_pim() {
  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (m_pim_queue_it[0].empty()) { return NULL; }

  dram_req_t *req = *(m_pim_queue_it[0].back());

  update_blacklist(REQ_PIM);

  for (unsigned int bank = 0; bank < m_config->nbk; bank++) {
    unsigned curr_row = m_dram->bk[bank]->curr_row;

    std::list<dram_req_t *>::iterator next = m_pim_queue_it[bank].back();
    dram_req_t *bank_req = *(next);

    // TODO: remove this
    assert(req == bank_req);

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

  assert(req != NULL && m_num_pim_pending != 0);
  m_num_pim_pending--;

  return req;
}

void bliss_scheduler::update_blacklist(request_type req_type)
{
  if (m_prev_request_type == req_type) {
    m_requests_served++;
  } else {
    m_requests_served = 0;
  }

  m_prev_request_type = req_type;

  if (m_requests_served > m_config->bliss_blacklisting_threshold) {
    switch (req_type) {
      case REQ_MEM: {
#ifdef DRAM_SCHED_VERIFY
        if (!is_mem_blacklisted) {
          printf("DRAM (%d): Blacklisting MEM\n", m_dram->id);
        }
#endif
        is_mem_blacklisted = true;
        break;
      }
      case REQ_PIM: {
#ifdef DRAM_SCHED_VERIFY
        if (!is_pim_blacklisted) {
          printf("DRAM (%d): Blacklisting PIM\n", m_dram->id);
        }
#endif
        is_pim_blacklisted = true;
        break;
      }
    }

    m_requests_served = 0;
  }
}
