// Copyright (c) 2009-2011, Tor M. Aamodt, Ali Bakhoda, George L. Yuan,
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "dram_sched.h"
#include "../abstract_hardware_model.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "mem_latency_stat.h"

dram_scheduler::dram_scheduler(const memory_config *config, dram_t *dm,
                                   memory_stats_t *stats) {
  m_config = config;
  m_stats = stats;
  m_num_pending = 0;
  m_num_write_pending = 0;
  m_num_pim_pending = 0;
  m_dram = dm;
  m_queue = new std::list<dram_req_t *>[m_config->nbk];
  m_bins = new std::map<
      unsigned, std::list<std::list<dram_req_t *>::iterator> >[m_config->nbk];
  m_last_row =
      new std::list<std::list<dram_req_t *>::iterator> *[m_config->nbk];
  curr_row_service_time = new unsigned[m_config->nbk];
  row_service_timestamp = new unsigned[m_config->nbk];
  for (unsigned i = 0; i < m_config->nbk; i++) {
    m_queue[i].clear();
    m_bins[i].clear();
    m_last_row[i] = NULL;
    curr_row_service_time[i] = 0;
    row_service_timestamp[i] = 0;
  }

  if (m_config->seperate_write_queue_enabled) {
    m_write_queue = new std::list<dram_req_t *>[m_config->nbk];
    m_write_bins = new std::map<
        unsigned, std::list<std::list<dram_req_t *>::iterator>>[m_config->nbk];
    m_last_write_row =
        new std::list<std::list<dram_req_t *>::iterator> *[m_config->nbk];

    for (unsigned i = 0; i < m_config->nbk; i++) {
      m_write_queue[i].clear();
      m_write_bins[i].clear();
      m_last_write_row[i] = NULL;
    }
  }

  m_pim_queue = new std::list<dram_req_t *>;
  m_pim_queue->clear();

  for (int r = 0; r < FRFCFS_NUM_SWITCH_REASONS; r++) {
    m_mem2pim_switch_reason[static_cast<frfcfs_switch_reason>(r)] = 0;
    m_pim2mem_switch_reason[static_cast<frfcfs_switch_reason>(r)] = 0;
  }

  m_curr_pim_row = 0;
  m_bank_issued_mem_req.resize(m_config->nbk, false);
  m_bank_ready_to_switch.resize(m_config->nbk, false);
  m_num_bypasses = 0;
}

void dram_scheduler::add_req(dram_req_t *req) {
  if (req->data->is_pim()) {
    assert(m_num_pim_pending < m_config->gpgpu_frfcfs_dram_pim_queue_size);
    m_num_pim_pending++;
    m_pim_queue->push_back(req);

    if (m_dram->first_pim_insert_timestamp == 0) {
      m_dram->first_pim_insert_timestamp = m_dram->m_gpu->gpu_sim_cycle +
                                           m_dram->m_gpu->gpu_tot_sim_cycle;
    }
  } else if (m_config->seperate_write_queue_enabled && req->data->is_write()) {
    assert(m_num_write_pending < m_config->gpgpu_frfcfs_dram_write_queue_size);
    m_num_write_pending++;
    m_write_queue[req->bk].push_front(req);
    std::list<dram_req_t *>::iterator ptr = m_write_queue[req->bk].begin();
    m_write_bins[req->bk][req->row].push_front(ptr);  // newest reqs to the
                                                      // front

    if (m_dram->first_non_pim_insert_timestamp == 0) {
      m_dram->first_non_pim_insert_timestamp = m_dram->m_gpu->gpu_sim_cycle +
                                              m_dram->m_gpu->gpu_tot_sim_cycle;
    }
  } else {
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

void dram_scheduler::data_collection(unsigned int bank) {
  if (m_dram->m_gpu->gpu_sim_cycle > row_service_timestamp[bank]) {
    curr_row_service_time[bank] =
        m_dram->m_gpu->gpu_sim_cycle - row_service_timestamp[bank];
    if (curr_row_service_time[bank] >
        m_stats->max_servicetime2samerow[m_dram->id][bank])
      m_stats->max_servicetime2samerow[m_dram->id][bank] =
          curr_row_service_time[bank];
  }
  curr_row_service_time[bank] = 0;
  row_service_timestamp[bank] = m_dram->m_gpu->gpu_sim_cycle;
  if (m_stats->concurrent_row_access[m_dram->id][bank] >
      m_stats->max_conc_access2samerow[m_dram->id][bank]) {
    m_stats->max_conc_access2samerow[m_dram->id][bank] =
        m_stats->concurrent_row_access[m_dram->id][bank];
  }
  m_stats->concurrent_row_access[m_dram->id][bank] = 0;
  m_stats->num_activates[m_dram->id][bank]++;
}

bool dram_scheduler::is_next_req_hit(unsigned bank, unsigned curr_row,
    enum memory_mode mode) {
  bool rowhit = true;

  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (mode == WRITE_MODE) {
    m_current_queue = m_write_queue;
    m_current_bins = m_write_bins;
    m_current_last_row = m_last_write_row;
  }

  if (m_current_last_row[bank] == NULL) {
    if (m_current_queue[bank].empty()) { return false; }

    std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >::iterator
        bin_ptr = m_current_bins[bank].find(curr_row);

    if (bin_ptr == m_current_bins[bank].end()) {
      rowhit = false;
    } else {
      rowhit = true;
    }
  }

  return rowhit;
}

void dram_scheduler::update_mode() {
  bool have_reads = m_num_pending > 0;
  bool have_writes = m_num_write_pending > 0;
  bool have_mem = have_reads || have_writes;
  bool have_pim = m_num_pim_pending > 0;

  if (m_dram->mode == PIM_MODE) {
    bool switch_to_mem = false;

    if (have_mem) {
      if (have_pim) {
        if (m_curr_pim_row != 0) {
          bool is_pim_oldest = true;

          for (unsigned int b = 0; b < m_config->nbk; b++) {
            if (m_queue[b].size() == 0) { continue; }

            if (m_queue[b].back()->timestamp < \
                    m_pim_queue->front()->timestamp){
              // There is *at least* one MEM request older than the oldest
              // PIM request
              is_pim_oldest = false;
              break;
            }
          }

          if (!is_pim_oldest) { m_num_bypasses++; }

          // Switch to MEM, if:
          // 1) PIM has a row buffer conflict and is not the oldest request
          if ((m_pim_queue->front()->row != m_curr_pim_row) && \
              !is_pim_oldest) {
            switch_to_mem = true;
            m_pim2mem_switch_reason[FRFCFS_OLDEST_FIRST]++;
          }

          // 2) CAP is enabled and has been exceeded
          else if ((m_config->frfcfs_cap > 0) && \
                   (m_num_bypasses > m_config->frfcfs_cap)) {
            switch_to_mem = true;
            m_pim2mem_switch_reason[FRFCFS_CAP_EXCEEDED]++;
          }
        }
      }

      else {
        switch_to_mem = true;
        m_pim2mem_switch_reason[FRFCFS_OUT_OF_REQUESTS]++;
      }
    }

    if (switch_to_mem) {
      m_curr_pim_row = 0;
      m_num_bypasses = 0;

      m_dram->mode = READ_MODE;
      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM %d: Switching to non-PIM mode\n", m_dram->id);
#endif
    }
  }

  else {
    bool switch_to_pim = false;

    if (have_pim) {
      if (have_mem) {
        bool is_pim_oldest = true;
        switch_to_pim = true;

        for (unsigned int b = 0; b < m_config->nbk; b++) {
          if ((m_queue[b].size() > 0) && (m_queue[b].back()->timestamp < \
                  m_pim_queue->front()->timestamp)) {
            // There is *at least* one MEM request older than the oldest
            // PIM request
            is_pim_oldest = false;
          }

          if (!m_bank_ready_to_switch[b]) {
            // A bank is ready to switch if:
            // 1) it has no pending requests, or
            // 2) the next request conflicts and the oldest request is a PIM
            //    request.
            //
            // Setting this flag also signals the scheduler to stop issuing
            // requests to the bank.
            m_bank_ready_to_switch[b] = (m_queue[b].size() == 0) || \
                (m_bank_issued_mem_req[b] && \
                 !is_next_req_hit(b, m_dram->bk[b]->curr_row,
                                  m_dram->mode) && \
                 (m_queue[b].back()->timestamp > \
                  m_pim_queue->front()->timestamp));
          }

          switch_to_pim = switch_to_pim && m_bank_ready_to_switch[b];
        }

        if (switch_to_pim) {
          m_mem2pim_switch_reason[FRFCFS_OLDEST_FIRST]++;
        }

        if (is_pim_oldest) { m_num_bypasses++; }

        if ((m_config->frfcfs_cap > 0) && \
            (m_num_bypasses > m_config->frfcfs_cap)) {
          switch_to_pim = true;
          m_mem2pim_switch_reason[FRFCFS_CAP_EXCEEDED]++;
        }
      }

      else {
        switch_to_pim = true;
        m_mem2pim_switch_reason[FRFCFS_OUT_OF_REQUESTS]++;
      }
    }

    if (switch_to_pim) {
      std::fill(m_bank_issued_mem_req.begin(), m_bank_issued_mem_req.end(),
          false);
      std::fill(m_bank_ready_to_switch.begin(), m_bank_ready_to_switch.end(),
          false);
      m_num_bypasses = 0;

      m_dram->mode = PIM_MODE;
      m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM %d: Switching to PIM mode\n", m_dram->id);
#endif
    }
  }

  update_rw_mode();
}

void dram_scheduler::update_rw_mode() {
  bool have_reads = m_num_pending > 0;
  bool have_writes = m_num_write_pending > 0;
  bool have_mem = have_reads || have_writes;

  if (m_dram->mode != PIM_MODE) {
    if (m_config->seperate_write_queue_enabled) {
      if (m_dram->mode == READ_MODE &&
          ((m_num_write_pending >= m_config->write_high_watermark)
           || (!have_reads && have_writes)
           )) {
        m_dram->mode = WRITE_MODE;
      } else if (m_dram->mode == WRITE_MODE &&
                 ((m_num_write_pending < m_config->write_low_watermark)
                  || (have_reads && !have_writes)
                  )) {
        m_dram->mode = READ_MODE;
      }
    }
  }
}

dram_req_t *dram_scheduler::schedule(unsigned bank, unsigned curr_row) {
  // row
  bool rowhit = true;
  std::list<dram_req_t *> *m_current_queue = m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_current_bins = m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_current_last_row =
      m_last_row;

  if (m_dram->mode == WRITE_MODE) {
    m_current_queue = m_write_queue;
    m_current_bins = m_write_bins;
    m_current_last_row = m_last_write_row;
  }

  if ((m_config->scheduler_type == DRAM_FRFCFS) || \
      (m_config->scheduler_type == DRAM_BLISS)) {
    m_bank_issued_mem_req[bank] = true;

    if (m_bank_ready_to_switch[bank]) {
      return NULL;
    }
  }

  if (m_current_last_row[bank] == NULL) {
    if (m_current_queue[bank].empty()) return NULL;

    std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >::iterator
        bin_ptr = m_current_bins[bank].find(curr_row);
    if (bin_ptr == m_current_bins[bank].end()) {
      dram_req_t *req = m_current_queue[bank].back();
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
#ifdef DEBUG_FAST_IDEAL_SCHED
  if (req)
    printf("%08u : DRAM(%u) scheduling memory request to bank=%u, row=%u\n",
           (unsigned)gpu_sim_cycle, m_dram->id, req->bk, req->row);
#endif

  if (m_config->seperate_write_queue_enabled && req->data->is_write()) {
    assert(req != NULL && m_num_write_pending != 0);
    m_num_write_pending--;
  } else {
    assert(req != NULL && m_num_pending != 0);
    m_num_pending--;
  }

  return req;
}

dram_req_t *dram_scheduler::schedule_pim() {
  if (m_num_pim_pending == 0) { return NULL; }

  dram_req_t *req = m_pim_queue->front();

  m_pim_queue->pop_front();
  m_num_pim_pending--;

  m_dram->access_num++;
  m_dram->pim_num++;

  for (unsigned int b = 0; b < m_config->nbk; b++) {
    bool rowhit = m_dram->bk[b]->curr_row == req->row;

    if (rowhit) {
      m_dram->hits_num++;
      m_dram->hits_pim_num++;
    } else {
      data_collection(b);
    }

    m_stats->concurrent_row_access[m_dram->id][b]++;
    m_stats->row_access[m_dram->id][b]++;
  }

  if ((m_config->scheduler_type == DRAM_FRFCFS) || \
      (m_config->scheduler_type == DRAM_BLISS)) {
    m_curr_pim_row = req->row;
  }

  return req;
}

void dram_scheduler::print(FILE *fp) {
  for (unsigned b = 0; b < m_config->nbk; b++) {
    printf(" %u: queue length = %u\n", b, (unsigned)m_queue[b].size());
  }
}

void dram_t::scheduler_frfcfs() {
  dram_req_t *req = NULL;
  dram_scheduler *sched = m_scheduler;

  while (!mrqq->empty()) {
    dram_req_t *req = mrqq->pop();

    // Power stats
    // if(req->data->get_type() != READ_REPLY && req->data->get_type() !=
    // WRITE_ACK)
    m_stats->total_n_access++;

    if (req->data->is_pim()) {
      m_stats->total_n_pim++;
    } else if (req->data->get_type() == WRITE_REQUEST) {
      m_stats->total_n_writes++;
    } else if (req->data->get_type() == READ_REQUEST) {
      m_stats->total_n_reads++;
    }

    req->data->set_status(IN_PARTITION_MC_INPUT_QUEUE,
                          m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
    sched->add_req(req);
  }

  enum memory_mode prev_mode = mode;

  sched->update_mode();

  if ((prev_mode != PIM_MODE) && (mode == PIM_MODE)) {
    for (unsigned b = 0; b < m_config->nbk; b++) {
      if (sched->is_next_req_hit(b, bk[b]->curr_row, prev_mode)) {
        nonpim2pimswitchconflicts++;
      }

      // Ensure we do not record row buffer hits after we switch back from PIM
      // to non-PIM
      sched->m_last_row[b] = NULL;
    }
  }

  if (mode == PIM_MODE) {
    bool can_schedule = true;
    bool waiting_for_nonpim = false;

    for (unsigned b = 0; b < m_config->nbk; b++) {
      if (bk[b]->mrq) {
        can_schedule = false;
        waiting_for_nonpim = waiting_for_nonpim || !bk[b]->mrq->data->is_pim();
        break;
      }
    }

    if (waiting_for_nonpim) { nonpim2pimswitchlatency++; }

    if (can_schedule) {
      req = sched->schedule_pim();

      if (req) {
        for (unsigned b = 0; b < m_config->nbk; b++) {
          bk[b]->mrq = req;
        }
      }
    }

    if ((sched->num_pending() + sched->num_write_pending()) > 0) {
      non_pim_queueing_delay++;
    }
  }

  else {
    unsigned i;
    for (i = 0; i < m_config->nbk; i++) {
      unsigned b = (i + prio) % m_config->nbk;
      if (!bk[b]->mrq) {
        req = sched->schedule(b, bk[b]->curr_row);

        if (req) {
          prio = (prio + 1) % m_config->nbk;
          bk[b]->mrq = req;

          break;
        }
      }
    }

    if (sched->num_pim_pending() > 0) {
      pim_queueing_delay++;
    }
  }

  if (req) {
    req->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,
                          m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);

    if (m_config->gpgpu_memlatency_stat) {
      unsigned mrq_latency = m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle -
                             req->timestamp;
      m_stats->mrq_latency.push_back(mrq_latency);
      m_stats->tot_mrq_num++;

      if (req->data->is_pim()) {
        m_stats->pim_mrq_latency.push_back(mrq_latency);
        m_stats->tot_pim_mrq_num++;
      } else {
        m_stats->non_pim_mrq_latency.push_back(mrq_latency);
        m_stats->tot_non_pim_mrq_num++;
      }

      req->timestamp = m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle;
      m_stats->mrq_lat_table[LOGB2(mrq_latency)]++;
    }
  }
}
