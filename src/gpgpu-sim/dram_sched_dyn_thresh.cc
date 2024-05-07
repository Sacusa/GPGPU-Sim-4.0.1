#include "dram_sched.h"
#include "dram_sched_dyn_thresh.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

dyn_thresh_scheduler::dyn_thresh_scheduler(const memory_config *config,
    dram_t *dm, memory_stats_t *stats) : dram_scheduler(config, dm, stats) {
  m_pim_queue_it =
      new std::list<std::list<dram_req_t *>::iterator>[m_config->nbk];
  m_last_pim_row = 0;

  m_bank_pending_mem_requests.resize(m_config->nbk, 0);

  m_mem_stall_time = 0;
  m_pim_stall_time = 0;
  m_max_pim_stall_time = m_config->queue_high_watermark * 3;
  m_num_times_mem_hit_occupancy = 0;

  m_max_mem_occupancy = m_config->queue_high_watermark;
}

void dyn_thresh_scheduler::add_req(dram_req_t *req) {
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

void dyn_thresh_scheduler::update_mode() {
  enum memory_mode prev_mode = m_dram->mode;

  // Switch to MEM mode if
  if (m_dram->mode == PIM_MODE) {
    bool mem_stall_time_exceeded = m_mem_stall_time > m_config->frfcfs_cap;
    bool mem_occupancy_exceeded = m_num_pending > m_max_mem_occupancy;

    if (mem_stall_time_exceeded) {
      // 1) MEM requests have waited too long
      m_dram->mode = READ_MODE;

#ifdef DRAM_SCHED_VERIFY
      unsigned prev_max_mem_occupancy = m_max_mem_occupancy;
#endif

      m_num_times_mem_hit_occupancy = 0;

      // 1a) Set occupancy threshold to 80% occupancy to reduce stall time.
      //     Also ensure: 1 <= thresh <= queue_high_watermark
      m_max_mem_occupancy = std::max(std::min(m_num_pending * 0.8,
            (double) m_config->queue_high_watermark), 1.0);

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to MEM mode\n", m_dram->id);
      printf("      MEM hit stall time threshold\n");
      if (prev_max_mem_occupancy != m_max_mem_occupancy) {
        printf("      Updating threshold %u -> %u\n", prev_max_mem_occupancy,
            m_max_mem_occupancy);
      }
#endif
    } else if (mem_occupancy_exceeded) {
      // 2) MEM requests have crossed the occupancy threshold
      m_dram->mode = READ_MODE;

#ifdef DRAM_SCHED_VERIFY
      unsigned prev_max_mem_occupancy = m_max_mem_occupancy;
#endif

      // 2a) Reset occupancy threshold if occupancy > 10% more than threshold.
      //     Doing this will reduce the switching frequency.
      //     The threshold setting process is similar to (1a) above.
      //if (m_num_pending > (1.1 * m_max_mem_occupancy)) {
      //  m_max_mem_occupancy = std::max(std::min(m_num_pending * 0.8,
      //        (double) m_config->queue_high_watermark), 1.0);
      //}

      m_num_times_mem_hit_occupancy++;

      if (m_num_times_mem_hit_occupancy > DYN_THRESH_MAX_OCCUPANCY_HIT) {
        m_max_mem_occupancy = std::min(m_max_mem_occupancy * 2,
            m_config->queue_high_watermark);
        m_num_times_mem_hit_occupancy = 0;
      }

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to MEM mode\n", m_dram->id);
      printf("      MEM hit occupancy threshold\n");
      if (prev_max_mem_occupancy != m_max_mem_occupancy) {
        printf("      Updating threshold %u -> %u\n", prev_max_mem_occupancy,
            m_max_mem_occupancy);
      }
#endif
    } else if (m_num_pim_pending == 0) {
      // 3) There are no more PIM requests and there are MEM requests, or
      if (m_num_pending > 0) {
        m_dram->mode = READ_MODE;

        m_num_times_mem_hit_occupancy = 0;

#ifdef DRAM_SCHED_VERIFY
        printf("DRAM (%d): Switching to MEM mode\n", m_dram->id);
        printf("      Out of PIM requests\n");
#endif
      }
    }
  }

  // Switch to PIM mode if
  else {
    bool threshold_exceeded = m_pim_stall_time > m_max_pim_stall_time;

    if (threshold_exceeded) {
      // 1) PIM requests have waited too long
      m_dram->mode = PIM_MODE;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
      printf("      PIM hit stall time threshold\n");
#endif
    } else if (m_num_pending == 0) {
      // 2) There are no more MEM requests and there are PIM requests, or
      if (m_num_pim_pending > 0) {
        m_dram->mode = PIM_MODE;

#ifdef DRAM_SCHED_VERIFY
        printf("DRAM (%d): Switching to PIM mode\n", m_dram->id);
        printf("      Out of MEM requests\n");
#endif
      }
    }
  }

  if (m_dram->mode != prev_mode) {
    if (prev_mode == PIM_MODE) {
      m_dram->pim2nonpimswitches++;

      m_last_pim_row = 0;
      m_pim_stall_time = 0;
    } else {
      m_dram->nonpim2pimswitches++;

      m_mem_stall_time = 0;
    }
  }
}

dram_req_t *dyn_thresh_scheduler::schedule(unsigned bank,
    unsigned curr_row) {
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

  if (m_num_pim_pending > 0) { m_pim_stall_time++; }

  assert(req != NULL && m_num_pending != 0);
  m_num_pending--;

  return req;
}

dram_req_t *dyn_thresh_scheduler::schedule_pim() {
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
  if (m_num_pending > 0) { m_mem_stall_time++; }

  assert(req != NULL && m_num_pim_pending != 0);
  m_num_pim_pending--;

  return req;
}
