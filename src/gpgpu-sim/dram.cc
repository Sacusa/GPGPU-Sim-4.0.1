// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ali Bakhoda,
// Ivan Sham, George L. Yuan,
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

#include "dram.h"
#include "dram_sched.h"
#include "dram_sched_gi.h"
#include "dram_sched_i1.h"
#include "dram_sched_i2.h"
#include "dram_sched_i2a.h"
#include "dram_sched_i3.h"
#include "dram_sched_i3_timer.h"
#include "dram_sched_i4a.h"
#include "dram_sched_i4a_no_cap.h"
#include "dram_sched_i4b.h"
#include "dram_sched_i4b_no_cap.h"
#include "dram_sched_hill_climbing.h"
#include "dram_sched_pim_frfcfs.h"
#include "dram_sched_gi_mem.h"
#include "dram_sched_bliss.h"
#include "dram_sched_queue.h"
#include "dram_sched_queue2.h"
#include "dram_sched_queue3.h"
#include "dram_sched_queue4.h"
#include "dram_sched_pim_frfcfs_util.h"
#include "dram_sched_mem_first.h"
#include "dram_sched_pim_first.h"
#include "dram_sched_dyn_thresh.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "hashing.h"
#include "l2cache.h"
#include "mem_fetch.h"
#include "mem_latency_stat.h"

#ifdef DRAM_VERIFY
int PRINT_CYCLE = 0;
#endif

template class fifo_pipeline<mem_fetch>;
template class fifo_pipeline<dram_req_t>;

dram_t::dram_t(unsigned int partition_id, const memory_config *config,
               memory_stats_t *stats, memory_partition_unit *mp,
               gpgpu_sim *gpu) {
  id = partition_id;
  m_memory_partition_unit = mp;
  m_stats = stats;
  m_config = config;
  m_gpu = gpu;

  m_dram_cycle = 0;
  last_non_pim_req_insert_cycle = 0;
  last_pim_req_insert_cycle = 0;

  // rowblp
  access_num = 0;
  hits_num = 0;
  read_num = 0;
  write_num = 0;
  hits_read_num = 0;
  hits_write_num = 0;
  banks_1time = 0;
  banks_access_total = 0;
  banks_access_total_after = 0;
  banks_time_rw = 0;
  banks_access_rw_total = 0;
  banks_time_ready = 0;
  banks_access_ready_total = 0;
  issued_two = 0;
  issued_total = 0;
  issued_total_row = 0;
  issued_total_col = 0;

  banks_1time_mem_only = 0;
  banks_access_total_mem_only = 0;
  banks_time_rw_mem_only = 0;
  banks_access_rw_total_mem_only = 0;
  banks_time_ready_mem_only = 0;
  banks_access_ready_total_mem_only = 0;

  CCDc = 0;
  RRDc = 0;
  RTWc = 0;
  WTRc = 0;

  wasted_bw_row = 0;
  wasted_bw_col = 0;
  util_bw = 0;
  idle_bw = 0;
  RCDc_limit = 0;
  CCDLc_limit = 0;
  CCDLc_limit_alone = 0;
  CCDc_limit = 0;
  WTRc_limit = 0;
  WTRc_limit_alone = 0;
  RCDWRc_limit = 0;
  RTWc_limit = 0;
  RTWc_limit_alone = 0;
  rwq_limit = 0;
  write_to_read_ratio_blp_rw_average = 0;
  bkgrp_parallsim_rw = 0;

  write_to_read_ratio_blp_rw_average_mem_only = 0;
  bkgrp_parallsim_rw_mem_only = 0;

  rw = READ;  // read mode is default

  bkgrp = (bankgrp_t **)calloc(sizeof(bankgrp_t *), m_config->nbkgrp);
  bkgrp[0] = (bankgrp_t *)calloc(sizeof(bank_t), m_config->nbkgrp);
  for (unsigned i = 1; i < m_config->nbkgrp; i++) {
    bkgrp[i] = bkgrp[0] + i;
  }
  for (unsigned i = 0; i < m_config->nbkgrp; i++) {
    bkgrp[i]->CCDLc = 0;
    bkgrp[i]->RTPLc = 0;
  }

  bk = (bank_t **)calloc(sizeof(bank_t *), m_config->nbk);
  bk[0] = (bank_t *)calloc(sizeof(bank_t), m_config->nbk);
  for (unsigned i = 1; i < m_config->nbk; i++) bk[i] = bk[0] + i;
  for (unsigned i = 0; i < m_config->nbk; i++) {
    bk[i]->state = BANK_IDLE;
    bk[i]->bkgrpindex = i / (m_config->nbk / m_config->nbkgrp);
  }
  prio = 0;

  unsigned max_mrqq = 2;
  if (m_config->scheduler_type == DRAM_FIFO) {
    max_mrqq = m_config->gpgpu_frfcfs_dram_sched_queue_size + \
               m_config->gpgpu_frfcfs_dram_pim_queue_size;
  }

  m_num_pending = 0;
  m_num_pim_pending = 0;

  rwq = new fifo_pipeline<dram_req_t>("rwq", m_config->CL, m_config->CL + 1);
  mrqq = new fifo_pipeline<dram_req_t>("mrqq", 0, max_mrqq);
  returnq = new fifo_pipeline<mem_fetch>(
      "dramreturnq", 0,
      m_config->gpgpu_dram_return_queue_size == 0
          ? 1024
          : m_config->gpgpu_dram_return_queue_size);

  switch (m_config->scheduler_type) {
    case DRAM_FIFO:
      m_scheduler = NULL;
      break;
    case DRAM_FRFCFS:
      m_scheduler = new dram_scheduler(m_config, this, stats);
      break;
    case DRAM_GI:
      m_scheduler = new gi_scheduler(m_config, this, stats);
      break;
    case DRAM_I1:
      m_scheduler = new i1_scheduler(m_config, this, stats);
      break;
    case DRAM_I2:
      m_scheduler = new i2_scheduler(m_config, this, stats);
      break;
    case DRAM_I2A:
      m_scheduler = new i2a_scheduler(m_config, this, stats);
      break;
    case DRAM_I3:
      m_scheduler = new i3_scheduler(m_config, this, stats);
      break;
    case DRAM_I3_TIMER:
      m_scheduler = new i3_timer_scheduler(m_config, this, stats);
      break;
    case DRAM_I4A:
      m_scheduler = new i4a_scheduler(m_config, this, stats);
      break;
    case DRAM_I4A_NO_CAP:
      m_scheduler = new i4a_no_cap_scheduler(m_config, this, stats);
      break;
    case DRAM_HILL_CLIMBING:
      m_scheduler = new hill_climbing_scheduler(m_config, this, stats);
      break;
    case DRAM_I4B:
      m_scheduler = new i4b_scheduler(m_config, this, stats);
      break;
    case DRAM_I4B_NO_CAP:
      m_scheduler = new i4b_no_cap_scheduler(m_config, this, stats);
      break;
    case DRAM_PIM_FRFCFS:
      m_scheduler = new pim_frfcfs_scheduler(m_config, this, stats);
      break;
    case DRAM_GI_MEM:
      m_scheduler = new gi_mem_scheduler(m_config, this, stats);
      break;
    case DRAM_BLISS:
      m_scheduler = new bliss_scheduler(m_config, this, stats);
      break;
    case DRAM_QUEUE:
      m_scheduler = new queue_scheduler(m_config, this, stats);
      break;
    case DRAM_QUEUE2:
      m_scheduler = new queue2_scheduler(m_config, this, stats);
      break;
    case DRAM_QUEUE3:
      m_scheduler = new queue3_scheduler(m_config, this, stats);
      break;
    case DRAM_QUEUE4:
      m_scheduler = new queue4_scheduler(m_config, this, stats);
      break;
    case DRAM_PIM_FRFCFS_UTIL:
      m_scheduler = new pim_frfcfs_util_scheduler(m_config, this, stats);
      break;
    case DRAM_MEM_FIRST:
      m_scheduler = new mem_first_scheduler(m_config, this, stats);
      break;
    case DRAM_PIM_FIRST:
      m_scheduler = new pim_first_scheduler(m_config, this, stats);
      break;
    case DRAM_DYN_THRESH:
      m_scheduler = new dyn_thresh_scheduler(m_config, this, stats);
      break;
    default:
      printf("Error: Unknown DRAM scheduler type\n");
      assert(0);
  }

  n_cmd = 0;
  n_activity = 0;
  n_nop = 0;
  n_act = 0;
  n_pre = 0;
  n_rd = 0;
  n_wr = 0;
  n_wr_WB = 0;
  n_rd_L2_A = 0;
  n_req = 0;
  max_mrqs_temp = 0;
  n_pim = 0;
  bwutil = 0;
  max_mrqs = 0;
  ave_mrqs = 0;

  for (unsigned i = 0; i < 10; i++) {
    dram_util_bins[i] = 0;
    dram_eff_bins[i] = 0;
  }
  last_n_cmd = last_n_activity = last_bwutil = 0;

  n_cmd_partial = 0;
  n_activity_partial = 0;
  n_nop_partial = 0;
  n_act_partial = 0;
  n_pre_partial = 0;
  n_req_partial = 0;
  ave_mrqs_partial = 0;
  bwutil_partial = 0;

  if (queue_limit())
    mrqq_Dist = StatCreate("mrqq_length", 1, queue_limit());
  else                                             // queue length is unlimited;
    mrqq_Dist = StatCreate("mrqq_length", 1, 64);  // track up to 64 entries

  mode = READ_MODE;

  pim2nonpimswitches = 0;
  nonpim2pimswitches = 0;
  nonpim2pimswitchlatency = 0;
  nonpim2pimswitchconflicts = 0;
  first_non_pim_insert_timestamp = 0;
  first_pim_insert_timestamp = 0;
  last_non_pim_finish_timestamp = 0;
  last_pim_finish_timestamp = 0;
  pim_queueing_delay = 0;
  non_pim_queueing_delay = 0;
  max_pim_mrqs = 0;
  max_pim_mrqs_temp = 0;
  ave_pim_mrqs = 0;
  ave_pim_mrqs_partial = 0;

  // num_phases = 10  ==>  max_phase_length = 5120K ~= 5M
  int num_phases = 10;
  for (int i = 0; i < num_phases; i++) {
    phase_length.push_back(10000 * ((int) pow(2, i)));
    num_total_phases.push_back(0);
    num_unstable_phases.push_back(0);
    phase_requests.push_back(0);
    stable_phase_requests.push_back(0);
  }
  phase_arr_rate_percent_change.resize(num_phases);
}

bool dram_t::full(bool is_write, bool is_pim) const {
  if (m_config->scheduler_type == DRAM_FIFO) {
    if (is_pim) {
      return m_num_pim_pending >= m_config->gpgpu_frfcfs_dram_pim_queue_size;
    } else {
      return m_num_pending >= m_config->gpgpu_frfcfs_dram_sched_queue_size;
    }
  }

  else {
    if (m_config->gpgpu_frfcfs_dram_sched_queue_size == 0) return false;

    if (is_pim) {
      return m_scheduler->num_pim_pending() >=
             m_config->gpgpu_frfcfs_dram_pim_queue_size;
    } else if (is_write && m_config->seperate_write_queue_enabled) {
      return m_scheduler->num_write_pending() >=
             m_config->gpgpu_frfcfs_dram_write_queue_size;
    } else {
      return m_scheduler->num_pending() >=
             m_config->gpgpu_frfcfs_dram_sched_queue_size;
    }
  }
}

unsigned dram_t::que_length() const {
  unsigned nreqs = 0;
  if (m_config->scheduler_type == DRAM_FIFO) {
    nreqs = mrqq->get_length();
  } else {
    nreqs = m_scheduler->num_pending();
  }
  return nreqs;
}

bool dram_t::returnq_full() const { return returnq->full(); }

unsigned int dram_t::queue_limit() const {
  return m_config->gpgpu_frfcfs_dram_sched_queue_size;
}

dram_req_t::dram_req_t(class mem_fetch *mf, unsigned banks,
                       unsigned dram_bnk_indexing_policy,
                       class gpgpu_sim *gpu) {
  txbytes = 0;
  dqbytes = 0;
  data = mf;
  m_gpu = gpu;

  const addrdec_t &tlx = mf->get_tlx_addr();

  switch (dram_bnk_indexing_policy) {
    case LINEAR_BK_INDEX: {
      bk = tlx.bk;
      break;
    }
    case BITWISE_XORING_BK_INDEX: {
      // xoring bank bits with lower bits of the page
      bk = bitwise_hash_function(tlx.row, tlx.bk, banks);
      assert(bk < banks);
      break;
    }
    case IPOLY_BK_INDEX: {
      /*IPOLY for bank indexing function from "Pseudo-randomly interleaved
       * memory." Rau, B. R et al. ISCA 1991
       * http://citeseerx.ist.psu.edu/viewdoc/download;jsessionid=348DEA37A3E440473B3C075EAABC63B6?doi=10.1.1.12.7149&rep=rep1&type=pdf
       */
      // xoring bank bits with lower bits of the page
      bk = ipoly_hash_function(tlx.row, tlx.bk, banks);
      assert(bk < banks);
      break;
    }
    case CUSTOM_BK_INDEX:
      /* No custom set function implemented */
      // Do you custom index here
      break;
    default:
      assert("\nUndefined bank index function.\n" && 0);
      break;
  }

  row = tlx.row;
  col = tlx.col;
  nbytes = mf->get_data_size();

  timestamp = m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle;
  addr = mf->get_addr();
  insertion_time = (unsigned)m_gpu->gpu_sim_cycle;
  rw = data->get_is_write() ? WRITE : READ;
}

void dram_t::push(class mem_fetch *data) {
  assert(id == data->get_tlx_addr()
                   .chip);  // Ensure request is in correct memory partition

  dram_req_t *mrq =
      new dram_req_t(data, m_config->nbk, m_config->dram_bnk_indexing_policy,
                     m_memory_partition_unit->get_mgpu());

  data->set_status(IN_PARTITION_MC_INTERFACE_QUEUE,
                   m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
  mrqq->push(mrq);

  if (data->is_pim()) {
    if (last_pim_req_insert_cycle > 0) {
      pim_req_arrival_latency.push_back(m_dram_cycle -
          last_pim_req_insert_cycle);
    }
    last_pim_req_insert_cycle = m_dram_cycle;

    n_pim++;
    m_num_pim_pending++;

#ifdef DRAM_VERIFY_MEM_PIM_EXCLUSIVITY
    m_pim_rows.insert(mrq->row);
    assert(m_mem_rows.find(mrq->row) == m_mem_rows.end());
#endif

    //warp_inst_t inst = data->get_inst();
    //shd_warp_t *warp = inst.get_warp();
    //printf("PIM WARP %d: IPC = %lf\n", inst.warp_id(), warp->get_ipc());
  } else {
    if (last_non_pim_req_insert_cycle > 0) {
      non_pim_req_arrival_latency.push_back(m_dram_cycle -
          last_non_pim_req_insert_cycle);
    }
    last_non_pim_req_insert_cycle = m_dram_cycle;

    m_num_pending++;

    for (int i = 0; i < phase_requests.size(); i++) {
      phase_requests[i]++;
    }

#ifdef DRAM_VERIFY_MEM_PIM_EXCLUSIVITY
    m_mem_rows.insert(mrq->row);
    assert(m_pim_rows.find(mrq->row) == m_pim_rows.end());
#endif

    //warp_inst_t inst = data->get_inst();
    //shd_warp_t *warp = inst.get_warp();
    //printf("NON-PIM WARP %d: IPC = %lf\n", inst.warp_id(), warp->get_ipc());
  }

  // stats...
  n_req += 1;
  n_req_partial += 1;
  if (m_config->scheduler_type == DRAM_FIFO) {
    max_mrqs_temp = (max_mrqs_temp > mrqq->get_length()) ? max_mrqs_temp
                                                         : mrqq->get_length();
    max_pim_mrqs_temp = (max_pim_mrqs_temp > m_num_pim_pending) ? \
                        max_pim_mrqs_temp : \
                        m_num_pim_pending;
  } else {
    unsigned nreqs = m_scheduler->num_pending() + \
                     m_scheduler->num_write_pending();
    unsigned int npimreqs = m_scheduler->num_pim_pending();

    nreqs += npimreqs;

    if (nreqs > max_mrqs_temp) max_mrqs_temp = nreqs;
    if (npimreqs > max_pim_mrqs_temp) { max_pim_mrqs_temp = npimreqs; }
  }
  m_stats->memlatstat_dram_access(data);
}

void dram_t::scheduler_fifo() {
  if (!mrqq->empty()) {
    dram_req_t *head_mrqq = mrqq->top();
    head_mrqq->data->set_status(
        IN_PARTITION_MC_BANK_ARB_QUEUE,
        m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);

    bool request_issued = false;

    if (head_mrqq->data->is_pim()) {
      if (first_pim_insert_timestamp == 0) {
        first_pim_insert_timestamp = m_gpu->gpu_sim_cycle +
                                     m_gpu->gpu_tot_sim_cycle;
      }

      bool can_schedule = true;

      for (unsigned int b = 0; b < m_config->nbk; b++) {
        if (bk[b]->mrq) {
          can_schedule = false;

          if (!bk[b]->mrq->data->is_pim()) {
            nonpim2pimswitchlatency++;
          }

          break;
        }
      }

      if (can_schedule) {
        head_mrqq = mrqq->pop();
        for (unsigned int b = 0; b < m_config->nbk; b++) {
          access_num++;
          pim_num++;
          if (bk[b]->curr_row == head_mrqq->row) {
            hits_num++;
            hits_pim_num++;
          }

          bk[b]->mrq = head_mrqq;
        }

        m_num_pim_pending--;
        request_issued = true;
      }

      if (mode != PIM_MODE) {
        nonpim2pimswitches++;

        if (!mrqq->empty()) {
          std::vector<bool> first_req_found(m_config->nbk, false);
          unsigned num_first_reqs_found = 0;

          fifo_data<dram_req_t>* fifo_it = mrqq->get_head();

          while ((fifo_it != NULL) && (num_first_reqs_found < m_config->nbk)) {
            dram_req_t *req = fifo_it->m_data;
            unsigned bkn = req->bk;

            if (!first_req_found[bkn] && !req->data->is_pim()) {
              if (bk[bkn]->curr_row == req->row) {
                nonpim2pimswitchconflicts++;
              }

              first_req_found[bkn] = true;
              num_first_reqs_found++;
            }

            fifo_it = fifo_it->m_next;
          }
        }
      }

      mode = PIM_MODE;

      non_pim_queueing_delay++;
    }

    else {
      if (first_non_pim_insert_timestamp == 0) {
        first_non_pim_insert_timestamp = m_gpu->gpu_sim_cycle +
                                         m_gpu->gpu_tot_sim_cycle;
      }

      unsigned bkn = head_mrqq->bk;

      if (!bk[bkn]->mrq) {
        access_num++;
        if (head_mrqq->data->is_write()) {
          write_num++;
        } else {
          read_num++;
        }

        if (bk[bkn]->curr_row == head_mrqq->row) {
          hits_num++;

          if (head_mrqq->data->is_write()) {
            hits_write_num++;
          }
          else {
            hits_read_num++;
          }
        }

        bk[bkn]->mrq = mrqq->pop();

        m_num_pending--;
        request_issued = true;
      }

      if (mode == PIM_MODE) {
        pim2nonpimswitches++;
      }

      mode = READ_MODE;  // Doesn't matter what mode we set to

      pim_queueing_delay++;
    }

    if (request_issued && m_config->gpgpu_memlatency_stat) {
      unsigned mrq_latency = m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle -
                             head_mrqq->timestamp;
      m_stats->mrq_latency.push_back(mrq_latency);
      m_stats->tot_mrq_num++;

      if (head_mrqq->data->is_pim()) {
        m_stats->pim_mrq_latency.push_back(mrq_latency);
        m_stats->tot_pim_mrq_num++;
      } else {
        m_stats->non_pim_mrq_latency.push_back(mrq_latency);
        m_stats->tot_non_pim_mrq_num++;
      }

      head_mrqq->timestamp = m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle;
      m_stats->mrq_lat_table[LOGB2(mrq_latency)]++;
    }
  }
}

#define DEC2ZERO(x) x = (x) ? (x - 1) : 0;
#define SWAP(a, b) \
  a ^= b;          \
  b ^= a;          \
  a ^= b;

void dram_t::cycle() {
  if (!returnq->full()) {
    dram_req_t *cmd = rwq->pop();
    if (cmd) {
#ifdef DRAM_VIEWCMD
      printf("\tDQ: BK%d Row:%03x Col:%03x", cmd->bk, cmd->row,
             cmd->col + cmd->dqbytes);
#endif
      cmd->dqbytes += m_config->dram_atom_size;

      if (cmd->dqbytes >= cmd->nbytes) {
        if (cmd->data->is_pim()) {
          last_pim_finish_timestamp = m_gpu->gpu_sim_cycle +
                                      m_gpu->gpu_tot_sim_cycle;
        } else {
          last_non_pim_finish_timestamp = m_gpu->gpu_sim_cycle +
                                          m_gpu->gpu_tot_sim_cycle;
        }

        mem_fetch *data = cmd->data;
        data->set_status(IN_PARTITION_MC_RETURNQ,
                         m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
        if (data->get_access_type() != L1_WRBK_ACC &&
            data->get_access_type() != L2_WRBK_ACC) {
          data->set_reply();
          returnq->push(data);
        } else {
          m_memory_partition_unit->set_done(data);
          delete data;
        }
        delete cmd;
      }
#ifdef DRAM_VIEWCMD
      printf("\n");
#endif
    }
  }

  /* check if the upcoming request is on an idle bank */
  /* Should we modify this so that multiple requests are checked? */

  switch (m_config->scheduler_type) {
    case DRAM_FIFO:
      scheduler_fifo();
      break;
    default:
      scheduler_frfcfs();
  }
  if (m_config->scheduler_type == DRAM_FIFO) {
    if (mrqq->get_length() > max_mrqs) {
      max_mrqs = mrqq->get_length();
    }
    ave_mrqs += mrqq->get_length();
    ave_mrqs_partial += mrqq->get_length();
    ave_pim_mrqs += m_num_pim_pending;
    ave_pim_mrqs_partial += m_num_pim_pending;
  } else {
    unsigned nreqs = m_scheduler->num_pending() + \
                     m_scheduler->num_write_pending();
    unsigned int npimreqs = m_scheduler->num_pim_pending();

    nreqs += npimreqs;

    if (nreqs > max_mrqs) {
      max_mrqs = nreqs;
    }
    if (npimreqs > max_pim_mrqs) {
      max_pim_mrqs = npimreqs;
    }

    ave_mrqs += nreqs;
    ave_mrqs_partial += nreqs;
    ave_pim_mrqs += npimreqs;
    ave_pim_mrqs_partial += npimreqs;
  }

  unsigned k = m_config->nbk;
  bool issued = false;

  // collect row buffer locality, BLP and other statistics
  /////////////////////////////////////////////////////////////////////////
  unsigned int memory_pending = 0;
  unsigned int memory_pending_mem_only = 0;
  for (unsigned i = 0; i < m_config->nbk; i++) {
    if (bk[i]->mrq) {
      memory_pending++;
      if (!bk[i]->mrq->data->is_pim()) { memory_pending_mem_only++; }
    }
  }
  banks_1time += memory_pending;
  banks_1time_mem_only += memory_pending_mem_only;
  if (memory_pending > 0) { banks_access_total++; }
  if (memory_pending_mem_only > 0) { banks_access_total_mem_only++; }

  unsigned int memory_pending_rw = 0;
  unsigned int memory_pending_rw_mem_only = 0;
  unsigned read_blp_rw = 0;
  unsigned read_blp_rw_mem_only = 0;
  unsigned write_blp_rw = 0;
  unsigned write_blp_rw_mem_only = 0;
  std::bitset<8> bnkgrp_rw_found;  // assume max we have 8 bank groups
  std::bitset<8> bnkgrp_rw_found_mem_only;

  for (unsigned j = 0; j < m_config->nbk; j++) {
    unsigned grp = get_bankgrp_number(j);
    if (bk[j]->mrq &&
        (((bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == READ) &&
          (bk[j]->state == BANK_ACTIVE)))) {
      memory_pending_rw++;
      read_blp_rw++;
      bnkgrp_rw_found.set(grp);
      if (!bk[j]->mrq->data->is_pim()) {
        memory_pending_rw_mem_only++;
        read_blp_rw++;
        bnkgrp_rw_found_mem_only.set(grp);
      }
    } else if (bk[j]->mrq &&
               (((bk[j]->curr_row == bk[j]->mrq->row) &&
                 (bk[j]->mrq->rw == WRITE) && (bk[j]->state == BANK_ACTIVE)))) {
      memory_pending_rw++;
      write_blp_rw++;
      bnkgrp_rw_found.set(grp);
      if (!bk[j]->mrq->data->is_pim()) {
        memory_pending_rw_mem_only++;
        write_blp_rw_mem_only++;
        bnkgrp_rw_found_mem_only.set(grp);
      }
    }
  }

  banks_time_rw += memory_pending_rw;
  banks_time_rw_mem_only += memory_pending_rw_mem_only;
  bkgrp_parallsim_rw += bnkgrp_rw_found.count();
  bkgrp_parallsim_rw_mem_only += bnkgrp_rw_found_mem_only.count();
  if (memory_pending_rw > 0) {
    write_to_read_ratio_blp_rw_average +=
        (double)write_blp_rw / (write_blp_rw + read_blp_rw);
    banks_access_rw_total++;
  }
  if (memory_pending_rw_mem_only > 0) {
    write_to_read_ratio_blp_rw_average_mem_only +=
        (double)write_blp_rw_mem_only / (write_blp_rw_mem_only + \
            read_blp_rw_mem_only);
    banks_access_rw_total_mem_only++;
  }

  unsigned int memory_Pending_ready = 0;
  unsigned int memory_Pending_ready_mem_only = 0;
  for (unsigned j = 0; j < m_config->nbk; j++) {
    unsigned grp = get_bankgrp_number(j);
    if (bk[j]->mrq &&
        ((!CCDc && !bk[j]->RCDc && !(bkgrp[grp]->CCDLc) &&
          (bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == READ) &&
          (WTRc == 0) && (bk[j]->state == BANK_ACTIVE) && !rwq->full()) ||
         (!CCDc && !bk[j]->RCDWRc && !(bkgrp[grp]->CCDLc) &&
          (bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == WRITE) &&
          (RTWc == 0) && (bk[j]->state == BANK_ACTIVE) && !rwq->full()))) {
      memory_Pending_ready++;
      if (!bk[j]->mrq->data->is_pim()) { memory_Pending_ready_mem_only++; }
    }
  }
  banks_time_ready += memory_Pending_ready;
  banks_time_ready_mem_only += memory_Pending_ready_mem_only;
  if (memory_Pending_ready > 0) { banks_access_ready_total++; }
  if (memory_Pending_ready_mem_only > 0) {banks_access_ready_total_mem_only++;}
  ///////////////////////////////////////////////////////////////////////////////////

  bool issued_col_cmd = false;
  bool issued_row_cmd = false;

  bool in_pim_mode = false;

  for (unsigned b = 0; b < m_config->nbk; b++) {
    if (bk[b]->mrq && bk[b]->mrq->data->is_pim()) {
      in_pim_mode = true;
      break;
    }
  }

  if (in_pim_mode) {
    issued_col_cmd = issue_pim_col_command();
    issued_row_cmd = issue_pim_row_command();
  }

  else {
    if (m_config->dual_bus_interface) {
      // dual bus interface
      // issue one row command and one column command
      for (unsigned i = 0; i < m_config->nbk; i++) {
        unsigned j = (i + prio) % m_config->nbk;
        issued_col_cmd = issue_col_command(j);
        if (issued_col_cmd) break;
      }
      for (unsigned i = 0; i < m_config->nbk; i++) {
        unsigned j = (i + prio) % m_config->nbk;
        issued_row_cmd = issue_row_command(j);
        if (issued_row_cmd) break;
      }
      for (unsigned i = 0; i < m_config->nbk; i++) {
        unsigned j = (i + prio) % m_config->nbk;
        if (!bk[j]->mrq) {
          if (!CCDc && !RRDc && !RTWc && !WTRc && !bk[j]->RCDc &&
              !bk[j]->RASc && !bk[j]->RCc && !bk[j]->RPc && !bk[j]->RCDWRc)
            k--;
          bk[j]->n_idle++;
        }
      }
    } else {
      // single bus interface
      // issue only one row/column command
      for (unsigned i = 0; i < m_config->nbk; i++) {
        unsigned j = (i + prio) % m_config->nbk;
        if (!issued_col_cmd) issued_col_cmd = issue_col_command(j);

        if (!issued_col_cmd && !issued_row_cmd)
          issued_row_cmd = issue_row_command(j);

        if (!bk[j]->mrq) {
          if (!CCDc && !RRDc && !RTWc && !WTRc && !bk[j]->RCDc &&
              !bk[j]->RASc && !bk[j]->RCc && !bk[j]->RPc && !bk[j]->RCDWRc)
            k--;
          bk[j]->n_idle++;
        }
      }
    }
  }

  issued = issued_row_cmd || issued_col_cmd;
  if (!issued) {
    n_nop++;
    n_nop_partial++;
#ifdef DRAM_VIEWCMD
    printf("\tNOP                        ");
#endif
  }
  if (k) {
    n_activity++;
    n_activity_partial++;
  }
  n_cmd++;
  n_cmd_partial++;
  if (issued) {
    issued_total++;
    if (issued_col_cmd && issued_row_cmd) issued_two++;
  }
  if (issued_col_cmd) issued_total_col++;
  if (issued_row_cmd) issued_total_row++;

  // Collect some statistics
  // check the limitation, see where BW is wasted?
  /////////////////////////////////////////////////////////
  unsigned int memory_pending_found = 0;
  for (unsigned i = 0; i < m_config->nbk; i++) {
    if (bk[i]->mrq) memory_pending_found++;
  }
  if (memory_pending_found > 0) banks_access_total_after++;

  bool memory_pending_rw_found = false;
  for (unsigned j = 0; j < m_config->nbk; j++) {
    if (bk[j]->mrq &&
        (((bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == READ) &&
          (bk[j]->state == BANK_ACTIVE)) ||
         ((bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == WRITE) &&
          (bk[j]->state == BANK_ACTIVE))))
      memory_pending_rw_found = true;
  }

  if (issued_col_cmd || CCDc)
    util_bw++;
  else if (memory_pending_rw_found) {
    wasted_bw_col++;
    for (unsigned j = 0; j < m_config->nbk; j++) {
      unsigned grp = get_bankgrp_number(j);
      // read
      if (bk[j]->mrq &&
          (((bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == READ) &&
            (bk[j]->state == BANK_ACTIVE)))) {
        if (bk[j]->RCDc) RCDc_limit++;
        if (bkgrp[grp]->CCDLc) CCDLc_limit++;
        if (WTRc) WTRc_limit++;
        if (CCDc) CCDc_limit++;
        if (rwq->full()) rwq_limit++;
        if (bkgrp[grp]->CCDLc && !WTRc) CCDLc_limit_alone++;
        if (!bkgrp[grp]->CCDLc && WTRc) WTRc_limit_alone++;
      }
      // write
      else if (bk[j]->mrq &&
               ((bk[j]->curr_row == bk[j]->mrq->row) &&
                (bk[j]->mrq->rw == WRITE) && (bk[j]->state == BANK_ACTIVE))) {
        if (bk[j]->RCDWRc) RCDWRc_limit++;
        if (bkgrp[grp]->CCDLc) CCDLc_limit++;
        if (RTWc) RTWc_limit++;
        if (CCDc) CCDc_limit++;
        if (rwq->full()) rwq_limit++;
        if (bkgrp[grp]->CCDLc && !RTWc) CCDLc_limit_alone++;
        if (!bkgrp[grp]->CCDLc && RTWc) RTWc_limit_alone++;
      }
    }
  } else if (memory_pending_found)
    wasted_bw_row++;
  else if (!memory_pending_found)
    idle_bw++;
  else
    assert(1);

  /////////////////////////////////////////////////////////

  // decrements counters once for each time dram_issueCMD is called
  m_dram_cycle++;
  DEC2ZERO(RRDc);
  DEC2ZERO(CCDc);
  DEC2ZERO(RTWc);
  DEC2ZERO(WTRc);
  for (unsigned j = 0; j < m_config->nbk; j++) {
    DEC2ZERO(bk[j]->RCDc);
    DEC2ZERO(bk[j]->RASc);
    DEC2ZERO(bk[j]->RCc);
    DEC2ZERO(bk[j]->RPc);
    DEC2ZERO(bk[j]->RCDWRc);
    DEC2ZERO(bk[j]->WTPc);
    DEC2ZERO(bk[j]->RTPc);
  }
  for (unsigned j = 0; j < m_config->nbkgrp; j++) {
    DEC2ZERO(bkgrp[j]->CCDLc);
    DEC2ZERO(bkgrp[j]->RTPLc);
  }

  for (int i = 0; i < phase_length.size(); i++) {
    if ((m_dram_cycle % phase_length[i]) == 0) {
      num_total_phases[i]++;

      if (num_total_phases[i] == 1) {
        stable_phase_requests[i] = phase_requests[i];
      }

      else {
        if (stable_phase_requests[i] == 0) {
          if (phase_requests[i] != 0) {
            num_unstable_phases[i]++;
            stable_phase_requests[i] = phase_requests[i];
          }
        }
        else {
          float arr_rate_change = (float) abs((long long int)
              (phase_requests[i] - stable_phase_requests[i])) / \
            stable_phase_requests[i];

          phase_arr_rate_percent_change[i].push_back(arr_rate_change);

          if (arr_rate_change > 0.05) {
            num_unstable_phases[i]++;
            stable_phase_requests[i] = phase_requests[i];
          }
        }
      }

      phase_requests[i] = 0;
    }
  }

#ifdef DRAM_VISUALIZE
  visualize();
#endif
}

bool dram_t::issue_col_command(int j) {
  bool issued = false;
  unsigned grp = get_bankgrp_number(j);
  if (bk[j]->mrq) {  // if currently servicing a memory request
    bk[j]->mrq->data->set_status(
        IN_PARTITION_DRAM, m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
    // correct row activated for a READ
    if (!issued && !CCDc && !bk[j]->RCDc && !(bkgrp[grp]->CCDLc) &&
        (bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == READ) &&
        (WTRc == 0) && (bk[j]->state == BANK_ACTIVE) && !rwq->full()) {
      if (rw == WRITE) {
        rw = READ;
        rwq->set_min_length(m_config->CL);
      }
      rwq->push(bk[j]->mrq);
      bk[j]->mrq->txbytes += m_config->dram_atom_size;
      CCDc = m_config->tCCD;
      bkgrp[grp]->CCDLc = m_config->tCCDL;
      RTWc = m_config->tRTW;
      bk[j]->RTPc = m_config->BL / m_config->data_command_freq_ratio;
      bkgrp[grp]->RTPLc = m_config->tRTPL;
      issued = true;
      if (bk[j]->mrq->data->get_access_type() == L2_WR_ALLOC_R)
        n_rd_L2_A++;
      else
        n_rd++;

      bwutil += m_config->BL / m_config->data_command_freq_ratio;
      bwutil_partial += m_config->BL / m_config->data_command_freq_ratio;
      bk[j]->n_access++;

#ifdef DRAM_VERIFY
      PRINT_CYCLE = 1;
      printf("\tRD  Ch:%d Bk:%d Row:%03x Col:%03x \n", id, j, bk[j]->curr_row,
             bk[j]->mrq->col + bk[j]->mrq->txbytes - m_config->dram_atom_size);
#endif
      // transfer done
      if (!(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes)) {
        update_service_latency_stats(bk[j]->mrq);
        bk[j]->mrq = NULL;
      }
    } else
        // correct row activated for a WRITE
        if (!issued && !CCDc && !bk[j]->RCDWRc && !(bkgrp[grp]->CCDLc) &&
            (bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == WRITE) &&
            (RTWc == 0) && (bk[j]->state == BANK_ACTIVE) && !rwq->full()) {
      if (rw == READ) {
        rw = WRITE;
        rwq->set_min_length(m_config->WL);
      }
      rwq->push(bk[j]->mrq);

      bk[j]->mrq->txbytes += m_config->dram_atom_size;
      CCDc = m_config->tCCD;
      bkgrp[grp]->CCDLc = m_config->tCCDL;
      WTRc = m_config->tWTR;
      bk[j]->WTPc = m_config->tWTP;
      issued = true;

      if (bk[j]->mrq->data->get_access_type() == L2_WRBK_ACC)
        n_wr_WB++;
      else
        n_wr++;

      bwutil += m_config->BL / m_config->data_command_freq_ratio;
      bwutil_partial += m_config->BL / m_config->data_command_freq_ratio;
#ifdef DRAM_VERIFY
      PRINT_CYCLE = 1;
      printf("\tWR  Ch:%d Bk:%d Row:%03x Col:%03x \n", id, j,
             bk[j]->curr_row, bk[j]->mrq->col + bk[j]->mrq->txbytes - \
             m_config->dram_atom_size);
#endif
      // transfer done
      if (!(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes)) {
        update_service_latency_stats(bk[j]->mrq);
        bk[j]->mrq = NULL;
      }
    }
  }

  return issued;
}

bool dram_t::issue_row_command(int j) {
  bool issued = false;
  unsigned grp = get_bankgrp_number(j);
  if (bk[j]->mrq) {  // if currently servicing a memory request
    bk[j]->mrq->data->set_status(
        IN_PARTITION_DRAM, m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
    //     bank is idle
    // else
    if (!issued && !RRDc && (bk[j]->state == BANK_IDLE) && !bk[j]->RPc &&
        !bk[j]->RCc) {  //
#ifdef DRAM_VERIFY
      PRINT_CYCLE = 1;
      printf("\tACT Ch:%d Bk:%d NewRow:%03x From:%03x \n", id, j,
          bk[j]->mrq->row, bk[j]->curr_row);
#endif
      // activate the row with current memory request
      bk[j]->curr_row = bk[j]->mrq->row;
      bk[j]->state = BANK_ACTIVE;
      RRDc = m_config->tRRD;
      bk[j]->RCDc = m_config->tRCD;
      bk[j]->RCDWRc = m_config->tRCDWR;
      bk[j]->RASc = m_config->tRAS;
      bk[j]->RCc = m_config->tRC;
      prio = (j + 1) % m_config->nbk;
      issued = true;
      n_act_partial++;
      n_act++;
    }

    else
        // different row activated
        if ((!issued) && (bk[j]->curr_row != bk[j]->mrq->row) &&
            (bk[j]->state == BANK_ACTIVE) &&
            (!bk[j]->RASc && !bk[j]->WTPc && !bk[j]->RTPc &&
             !bkgrp[grp]->RTPLc)) {
#ifdef DRAM_VERIFY
      PRINT_CYCLE = 1;
      printf("\tPRE Ch:%d Bk:%d Row:%03x \n", id, j, bk[j]->curr_row);
#endif
      // make the bank idle again
      bk[j]->state = BANK_IDLE;
      bk[j]->RPc = m_config->tRP;
      prio = (j + 1) % m_config->nbk;
      issued = true;
      n_pre++;
      n_pre_partial++;
    }
  }
  return issued;
}

bool dram_t::issue_pim_col_command() {
  bool can_issue = true;

  for (int j = 0; j < m_config->nbk; j++) {
    unsigned grp = get_bankgrp_number(j);

    can_issue = can_issue && bk[j]->mrq &&
      !CCDc && !bk[j]->RCDWRc && !(bkgrp[grp]->CCDLc) &&
      (bk[j]->curr_row == bk[j]->mrq->row) && (bk[j]->mrq->rw == WRITE) &&
      (RTWc == 0) && (bk[j]->state == BANK_ACTIVE) && !rwq->full();

    if (!can_issue) { break; }
  }

  if (can_issue) {
    if (rw == READ) {
      rw = WRITE;
      rwq->set_min_length(m_config->WL);
    }
    rwq->push(bk[0]->mrq);

    update_service_latency_stats(bk[0]->mrq);

    for (unsigned j = 0; j < m_config->nbk; j++) {
      unsigned grp = get_bankgrp_number(j);

      bk[j]->mrq->data->set_status(
          IN_PARTITION_DRAM, m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);

      bk[j]->mrq->txbytes += m_config->dram_atom_size;
      bkgrp[grp]->CCDLc = m_config->tCCDL;
      bk[j]->WTPc = m_config->tWTP;

      // TODO: should the following two statistics be disabled?
      bwutil += m_config->BL / m_config->data_command_freq_ratio;
      bwutil_partial += m_config->BL / m_config->data_command_freq_ratio;

#ifdef DRAM_VERIFY
      PRINT_CYCLE = 1;
      printf("\tPIM Ch:%d Bk:%d Row:%03x Col:%03x \n", id, j,
             bk[j]->curr_row, bk[j]->mrq->col);
#endif

      bk[j]->mrq = NULL;
    }

    CCDc = m_config->tCCD;
    WTRc = m_config->tWTR;
  }

  return can_issue;
}

bool dram_t::issue_pim_row_command() {
  bool can_issue = false;

  std::vector<unsigned> precharge_banks;
  std::vector<unsigned> activate_banks;

  for (unsigned j = 0; j < m_config->nbk; j++) {
    if (bk[j]->mrq) {
      if ((bk[j]->state == BANK_ACTIVE) &&
          (bk[j]->curr_row != bk[j]->mrq->row)) {
        precharge_banks.push_back(j);
      }
      else if (bk[j]->state == BANK_IDLE) {
        activate_banks.push_back(j);
      }
    }
  }

  if (precharge_banks.size() > 0) {
    can_issue = true;

    for (unsigned j : precharge_banks) {
      unsigned grp = get_bankgrp_number(j);

      can_issue = can_issue && !bk[j]->RASc && !bk[j]->WTPc && !bk[j]->RTPc &&
           !bkgrp[grp]->RTPLc;

      if (!can_issue) { break; }
    }

    if (can_issue) {
      for (unsigned j : precharge_banks) {
#ifdef DRAM_VERIFY
        PRINT_CYCLE = 1;
        printf("\tPRE Ch:%d Bk:%d Row:%03x \n", id, j, bk[j]->curr_row);
#endif
        bk[j]->state = BANK_IDLE;
        bk[j]->RPc = m_config->tRP;
      }

      prio = 0;
      n_pre++;
      n_pre_partial++;
    }
  }

  else if (activate_banks.size() > 0) {
    can_issue = true;

    for (unsigned j : activate_banks) {
      can_issue = can_issue && !RRDc && !bk[j]->RPc && !bk[j]->RCc;

      if (!can_issue) { break; }
    }

    if (can_issue) {
      for (unsigned j : activate_banks) {
#ifdef DRAM_VERIFY
        PRINT_CYCLE = 1;
        printf("\tACT Ch:%d Bk:%d NewRow:%03x From:%03x \n", id, j,
            bk[j]->mrq->row, bk[j]->curr_row);
#endif
        bk[j]->curr_row = bk[j]->mrq->row;
        bk[j]->state = BANK_ACTIVE;
        bk[j]->RCDc = m_config->tRCD;
        bk[j]->RCDWRc = m_config->tRCDWR;
        bk[j]->RASc = m_config->tRAS;
        bk[j]->RCc = m_config->tRC;
      }

      RRDc = m_config->tRRD;
      prio = 0;
      n_act_partial++;
      n_act++;
    }
  }

  return can_issue;
}

void dram_t::update_service_latency_stats(dram_req_t *req) {
  unsigned service_latency = m_gpu->gpu_tot_sim_cycle + m_gpu->gpu_sim_cycle -\
                             req->timestamp;
  bool is_pim = req->data->is_pim();

  m_stats->dram_service_latency.push_back(service_latency);

  if (is_pim) {
    m_stats->pim_dram_service_latency.push_back(service_latency);
  } else {
    m_stats->non_pim_dram_service_latency.push_back(service_latency);
  }
}

// if mrq is being serviced by dram, gets popped after CL latency fulfilled
class mem_fetch *dram_t::return_queue_pop() {
  return returnq->pop();
}

class mem_fetch *dram_t::return_queue_top() {
  return returnq->top();
}

void dram_t::print(FILE *simFile) const {
  unsigned i;
  fprintf(simFile, "DRAM[%d]: %d bks, busW=%d BL=%d CL=%d, ", id, m_config->nbk,
          m_config->busW, m_config->BL, m_config->CL);
  fprintf(simFile, "tRRD=%d tCCD=%d, tRCD=%d tRAS=%d tRP=%d tRC=%d\n",
          m_config->tRRD, m_config->tCCD, m_config->tRCD, m_config->tRAS,
          m_config->tRP, m_config->tRC);
  fprintf(
      simFile,
      "n_cmd=%llu n_nop=%llu n_act=%llu n_pre=%llu n_ref_event=%llu n_req=%llu "
      "n_rd=%llu n_rd_L2_A=%llu n_write=%llu n_wr_bk=%llu bw_util=%.4g\n",
      n_cmd, n_nop, n_act, n_pre, n_ref, n_req, n_rd, n_rd_L2_A, n_wr, n_wr_WB,
      (float)bwutil / n_cmd);
  fprintf(simFile, "n_activity=%llu dram_eff=%.4g\n", n_activity,
          (float)bwutil / n_activity);
  for (i = 0; i < m_config->nbk; i++) {
    fprintf(simFile, "bk%d: %da %di ", i, bk[i]->n_access, bk[i]->n_idle);
  }
  fprintf(simFile, "\n");
  fprintf(simFile,
          "\n------------------------------------------------------------------"
          "------\n");

  printf("\nRow_Buffer_Locality = %.6f", (float)hits_num / access_num);
  printf("\nRow_Buffer_Locality_read = %.6f", (float)hits_read_num / read_num);
  printf("\nRow_Buffer_Locality_write = %.6f",
         (float)hits_write_num / write_num);
  printf("\nRow_Buffer_Locality_pim = %.6f",
         (float)hits_pim_num / pim_num);

  printf("\nBank_Level_Parallism = %.6f",
         (float)banks_1time / banks_access_total);
  printf("\nBank_Level_Parallism_Col = %.6f",
         (float)banks_time_rw / banks_access_rw_total);
  printf("\nBank_Level_Parallism_Ready = %.6f",
         (float)banks_time_ready / banks_access_ready_total);
  printf("\nwrite_to_read_ratio_blp_rw_average = %.6f",
         write_to_read_ratio_blp_rw_average / banks_access_rw_total);
  printf("\nGrpLevelPara = %.6f \n",
         (float)bkgrp_parallsim_rw / banks_access_rw_total);

  printf("\nBank_Level_Parallism_MEM_only = %.6f",
         (float)banks_1time_mem_only / banks_access_total_mem_only);
  printf("\nBank_Level_Parallism_Col_MEM_only = %.6f",
         (float)banks_time_rw_mem_only / banks_access_rw_total_mem_only);
  printf("\nBank_Level_Parallism_Ready_MEM_only = %.6f",
         (float)banks_time_ready_mem_only / banks_access_ready_total_mem_only);
  printf("\nwrite_to_read_ratio_blp_rw_average_MEM_only = %.6f",
         write_to_read_ratio_blp_rw_average_mem_only / \
         banks_access_rw_total_mem_only);
  printf("\nGrpLevelPara_MEM_only = %.6f \n",
         (float)bkgrp_parallsim_rw_mem_only / banks_access_rw_total_mem_only);

  // Request arrival rate stats
  double sum_non_pim = 0;
  double mean_non_pim = 0;
  double sq_sum_non_pim = 0;
  double stdev_non_pim = 0;
  double max_non_pim = 0;
  unsigned long long n_non_pim = n_req - n_pim;

  if (n_non_pim > 1) {
    sum_non_pim = std::accumulate(non_pim_req_arrival_latency.begin(),
        non_pim_req_arrival_latency.end(), 0.0);
    mean_non_pim = sum_non_pim / n_non_pim;
    sq_sum_non_pim = std::inner_product(non_pim_req_arrival_latency.begin(),
        non_pim_req_arrival_latency.end(), non_pim_req_arrival_latency.begin(),
        0.0);
    stdev_non_pim = std::sqrt(sq_sum_non_pim / n_non_pim -
        mean_non_pim * mean_non_pim);
    max_non_pim = *std::max_element(std::begin(non_pim_req_arrival_latency),
        std::end(non_pim_req_arrival_latency));
  }

  printf("\nAvgNonPimReqArrivalLatency = %.6f", mean_non_pim);
  printf("\nMaxNonPimReqArrivalLatency = %.6f", max_non_pim);
  printf("\nStDevNonPimReqArrivalLatency = %.6f \n", stdev_non_pim);

  double sum_pim = 0;
  double mean_pim = 0;
  double sq_sum_pim = 0;
  double stdev_pim = 0;
  double max_pim = 0;

  if (n_pim > 0) {
    sum_pim = std::accumulate(pim_req_arrival_latency.begin(),
        pim_req_arrival_latency.end(), 0.0);
    mean_pim = sum_pim / n_pim;
    sq_sum_pim = std::inner_product(pim_req_arrival_latency.begin(),
        pim_req_arrival_latency.end(), pim_req_arrival_latency.begin(), 0.0);
    stdev_pim = std::sqrt(sq_sum_pim / n_pim - mean_pim * mean_pim);
    max_pim = *std::max_element(std::begin(pim_req_arrival_latency),
        std::end(pim_req_arrival_latency));
  }

  printf("\nAvgPimReqArrivalLatency = %.6f", mean_pim);
  printf("\nMaxPimReqArrivalLatency = %.6f", max_pim);
  printf("\nStDevPimReqArrivalLatency = %.6f \n", stdev_pim);

  printf("\nBW Util details:\n");
  printf("bwutil = %.6f \n", (float)bwutil / n_cmd);
  printf("total_CMD = %llu \n", n_cmd);
  printf("util_bw = %llu \n", util_bw);
  printf("Wasted_Col = %llu \n", wasted_bw_col);
  printf("Wasted_Row = %llu \n", wasted_bw_row);
  printf("Idle = %llu \n", idle_bw);

  printf("\nBW Util Bottlenecks: \n");
  printf("RCDc_limit = %llu \n", RCDc_limit);
  printf("RCDWRc_limit = %llu \n", RCDWRc_limit);
  printf("WTRc_limit = %llu \n", WTRc_limit);
  printf("RTWc_limit = %llu \n", RTWc_limit);
  printf("CCDLc_limit = %llu \n", CCDLc_limit);
  printf("rwq = %llu \n", rwq_limit);
  printf("CCDLc_limit_alone = %llu \n", CCDLc_limit_alone);
  printf("WTRc_limit_alone = %llu \n", WTRc_limit_alone);
  printf("RTWc_limit_alone = %llu \n", RTWc_limit_alone);

  printf("\nCommands details: \n");
  printf("total_CMD = %llu \n", n_cmd);
  printf("n_nop = %llu \n", n_nop);
  printf("Read = %llu \n", n_rd);
  printf("Write = %llu \n", n_wr);
  printf("PIM = %llu \n", n_pim);
  printf("L2_Alloc = %llu \n", n_rd_L2_A);
  printf("L2_WB = %llu \n", n_wr_WB);
  printf("n_act = %llu \n", n_act);
  printf("n_pre = %llu \n", n_pre);
  printf("n_ref = %llu \n", n_ref);
  printf("n_req = %llu \n", n_req);
  printf("total_req = %llu \n", n_rd + n_wr + n_rd_L2_A + n_wr_WB + n_pim);

  printf("\nPIM statistics:\n");
  printf("pim2nonpimswitches = %llu\n", pim2nonpimswitches);
  printf("nonpim2pimswitches = %llu\n", nonpim2pimswitches);
  printf("nonpim2pimswitchlatency = %llu\n", nonpim2pimswitchlatency);
  printf("nonpim2pimswitchconflicts = %llu\n", nonpim2pimswitchconflicts);
  printf("first_non_pim_insert = %llu\n", first_non_pim_insert_timestamp);
  printf("first_pim_insert = %llu\n", first_pim_insert_timestamp);
  printf("last_non_pim_finish = %llu\n", last_non_pim_finish_timestamp);
  printf("last_pim_finish = %llu\n", last_pim_finish_timestamp);
  printf("avg_pim_queuing_delay = %lf\n", (double)pim_queueing_delay / n_pim);
  printf("avg_non_pim_queuing_delay = %lf\n", (double)non_pim_queueing_delay /
      (n_rd + n_wr + n_rd_L2_A + n_wr_WB));

  if (m_config->scheduler_type == DRAM_I3_TIMER) {
    i3_timer_scheduler *sched = (i3_timer_scheduler*) m_scheduler;

    sched->finalize_stats();

    double sum = 0;
    double mean = 0;
    double sq = 0;
    double stdev = 0;
    double max = 0;
    unsigned long long len = sched->m_pim_batch_exec_time.size();

    if (len > 0) {
      sum = std::accumulate(sched->m_pim_batch_exec_time.begin(),
          sched->m_pim_batch_exec_time.end(), 0.0);
      mean = sum / len;
      sq = std::inner_product(sched->m_pim_batch_exec_time.begin(),
          sched->m_pim_batch_exec_time.end(),
          sched->m_pim_batch_exec_time.begin(), 0.0);
      stdev = std::sqrt(sq / len - mean * mean);
      max = *std::max_element(std::begin(sched->m_pim_batch_exec_time),
          std::end(sched->m_pim_batch_exec_time));
    }

    printf("\nAvgPimBatchExecTime = %.6f", mean);
    printf("\nMaxPimBatchExecTime = %.6f", max);
    printf("\nStDevPimBatchExecTime = %.6f", stdev);

    double avg_batch_size = 0;
    if (len > 0) { avg_batch_size = n_pim / len; }
    printf("\nAvgPimBatchSize = %.6f\n", avg_batch_size);

    // MEM batch execution time
    sum = 0;
    mean = 0;
    sq = 0;
    stdev = 0;
    max = 0;
    len = sched->m_mem_batch_exec_time.size();

    if (len > 0) {
      sum = std::accumulate(sched->m_mem_batch_exec_time.begin(),
          sched->m_mem_batch_exec_time.end(), 0.0);
      mean = sum / len;
      sq = std::inner_product(sched->m_mem_batch_exec_time.begin(),
          sched->m_mem_batch_exec_time.end(),
          sched->m_mem_batch_exec_time.begin(), 0.0);
      stdev = std::sqrt(sq / len - mean * mean);
      max = *std::max_element(std::begin(sched->m_mem_batch_exec_time),
          std::end(sched->m_mem_batch_exec_time));
    }

    printf("\nAvgMemBatchExecTime = %.6f", mean);
    printf("\nMaxMemBatchExecTime = %.6f", max);
    printf("\nStDevMemBatchExecTime = %.6f\n", stdev);

    // Wasted MEM batch cycles
    sum = 0;
    mean = 0;
    sq = 0;
    stdev = 0;
    max = 0;
    // we reuse 'len' because we need average across all MEM batches

    if (sched->m_mem_wasted_cycles.size()) {
      sum = std::accumulate(sched->m_mem_wasted_cycles.begin(),
          sched->m_mem_wasted_cycles.end(), 0.0);
      mean = sum / len;
      sq = std::inner_product(sched->m_mem_wasted_cycles.begin(),
          sched->m_mem_wasted_cycles.end(),
          sched->m_mem_wasted_cycles.begin(), 0.0);
      stdev = std::sqrt(sq / len - mean * mean);
      max = *std::max_element(std::begin(sched->m_mem_wasted_cycles),
          std::end(sched->m_mem_wasted_cycles));
    }

    printf("\nAvgMemWastedCycles = %.6f", mean);
    printf("\nMaxMemWastedCycles = %.6f", max);
    printf("\nStDevMemWastedCycles = %.6f\n", stdev);
  }

  if (m_config->scheduler_type == DRAM_PIM_FRFCFS) {
    pim_frfcfs_scheduler *sched = (pim_frfcfs_scheduler*) m_scheduler;

    printf("\nBank stall time for PIM:\n");
    for (unsigned b = 0; b < m_config->nbk; b++) {
      printf("Bank_%d_stall_time = %llu\n", b,sched->m_bank_pim_stall_time[b]);
    }

    printf("\nBank waste time for PIM:\n");
    for (unsigned b = 0; b < m_config->nbk; b++) {
      printf("Bank_%d_waste_time = %llu\n", b,sched->m_bank_pim_waste_time[b]);
    }

    printf("\nMEM2PIM switch readiness latency:\n");

    double sum = 0;
    double mean = 0;
    double sq = 0;
    double stdev = 0;
    double max = 0;
    unsigned long long len = sched->m_mem2pim_switch_latency.size();

    if (len > 0) {
      sum = std::accumulate(sched->m_mem2pim_switch_latency.begin(),
          sched->m_mem2pim_switch_latency.end(), 0.0);
      mean = sum / len;
      sq = std::inner_product(sched->m_mem2pim_switch_latency.begin(),
          sched->m_mem2pim_switch_latency.end(),
          sched->m_mem2pim_switch_latency.begin(), 0.0);
      stdev = std::sqrt(sq / len - mean * mean);
      max = *std::max_element(std::begin(sched->m_mem2pim_switch_latency),
          std::end(sched->m_mem2pim_switch_latency));
    }

    printf("\nAvgSwitchReadinessLatency = %.6f", mean);
    printf("\nMaxSwitchReadinessLatency = %.6f", max);
    printf("\nStDevSwitchReadinessLatency = %.6f", stdev);

    unsigned long long len_non_zeros = len -
        std::count(sched->m_mem2pim_switch_latency.begin(),
                sched->m_mem2pim_switch_latency.end(), 0);

    if (len_non_zeros > 0) {
      mean = sum / len_non_zeros;
      stdev = std::sqrt(sq / len_non_zeros - mean * mean);
    }

    printf("\nAvgNonZeroSwitchReadinessLatency = %.6f", mean);
    printf("\nStDevNonZeroSwitchReadinessLatency = %.6f\n", stdev);
  }

  if (m_config->scheduler_type == DRAM_PIM_FRFCFS_UTIL) {
    pim_frfcfs_util_scheduler *sched = (pim_frfcfs_util_scheduler*)m_scheduler;

    printf("\nBank stall time for PIM:\n");
    for (unsigned b = 0; b < m_config->nbk; b++) {
      printf("Bank_%d_stall_time = %llu\n", b,sched->m_bank_pim_stall_time[b]);
    }

    printf("\nBank waste time for PIM:\n");
    for (unsigned b = 0; b < m_config->nbk; b++) {
      printf("Bank_%d_waste_time = %llu\n", b,sched->m_bank_pim_waste_time[b]);
    }

    printf("\nMEM2PIM switch readiness latency:\n");

    double sum = 0;
    double mean = 0;
    double sq = 0;
    double stdev = 0;
    double max = 0;
    unsigned long long len = sched->m_mem2pim_switch_latency.size();

    if (len > 0) {
      sum = std::accumulate(sched->m_mem2pim_switch_latency.begin(),
          sched->m_mem2pim_switch_latency.end(), 0.0);
      mean = sum / len;
      sq = std::inner_product(sched->m_mem2pim_switch_latency.begin(),
          sched->m_mem2pim_switch_latency.end(),
          sched->m_mem2pim_switch_latency.begin(), 0.0);
      stdev = std::sqrt(sq / len - mean * mean);
      max = *std::max_element(std::begin(sched->m_mem2pim_switch_latency),
          std::end(sched->m_mem2pim_switch_latency));
    }

    printf("\nAvgSwitchReadinessLatency = %.6f", mean);
    printf("\nMaxSwitchReadinessLatency = %.6f", max);
    printf("\nStDevSwitchReadinessLatency = %.6f", stdev);

    unsigned long long len_non_zeros = len -
        std::count(sched->m_mem2pim_switch_latency.begin(),
                sched->m_mem2pim_switch_latency.end(), 0);

    if (len_non_zeros > 0) {
      mean = sum / len_non_zeros;
      stdev = std::sqrt(sq / len_non_zeros - mean * mean);
    }

    printf("\nAvgNonZeroSwitchReadinessLatency = %.6f", mean);
    printf("\nStDevNonZeroSwitchReadinessLatency = %.6f\n", stdev);
  }

  if (m_config->scheduler_type == DRAM_BLISS) {
    bliss_scheduler *sched = (bliss_scheduler*) m_scheduler;
    printf("\nBlacklist statistics\n");
    printf("Total cycles = %ull\n", m_dram_cycle);
    printf("Cycles none blacklisted = %ull\n",
        sched->m_cycles_none_blacklisted);
    printf("Cycles both blacklisted = %ull\n",
        sched->m_cycles_both_blacklisted);
    printf("Cycles PIM blacklisted = %ull\n",
        sched->m_cycles_pim_blacklisted);
    printf("Cycles MEM blacklisted = %ull\n",
        sched->m_cycles_mem_blacklisted);
  }

  printf("\nDual Bus Interface Util: \n");
  printf("issued_total_row = %llu \n", issued_total_row);
  printf("issued_total_col = %llu \n", issued_total_col);
  printf("Row_Bus_Util =  %.6f \n", (float)issued_total_row / n_cmd);
  printf("CoL_Bus_Util = %.6f \n", (float)issued_total_col / n_cmd);
  printf("Either_Row_CoL_Bus_Util = %.6f \n", (float)issued_total / n_cmd);
  printf("Issued_on_Two_Bus_Simul_Util = %.6f \n", (float)issued_two / n_cmd);
  printf("issued_two_Eff = %.6f \n", (float)issued_two / issued_total);
  printf("queue_avg = %.6f \n\n", (float)ave_mrqs / n_cmd);
  printf("queue_avg_pim = %.6f \n\n", (float)ave_pim_mrqs / n_cmd);

  fprintf(simFile, "\n");
  fprintf(simFile, "dram_util_bins:");
  for (i = 0; i < 10; i++) fprintf(simFile, " %d", dram_util_bins[i]);
  fprintf(simFile, "\ndram_eff_bins:");
  for (i = 0; i < 10; i++) fprintf(simFile, " %d", dram_eff_bins[i]);
  fprintf(simFile, "\n");
  if (m_config->scheduler_type != DRAM_FIFO) {
    fprintf(simFile, "mrqq: max=%d avg=%g\n", max_mrqs,
            (float)ave_mrqs / n_cmd);
    fprintf(simFile, "mrqq_pim: max=%d avg=%g\n", max_pim_mrqs,
            (float)ave_pim_mrqs / n_cmd);
  }

  printf("\nPhase statistics:\n");
  for (int i = 0; i < phase_length.size(); i++) {
    printf("%dK (total/unstable) = %llu / %llu", 10 * ((int) pow(2, i)),
        num_total_phases[i], num_unstable_phases[i]);

    int size = phase_arr_rate_percent_change[i].size();
    float min = -1, max = -1, median = -1;

    if (size > 0) {
      std::vector<float> arr_rate_change(phase_arr_rate_percent_change[i]);
      std::sort(arr_rate_change.begin(), arr_rate_change.end());

      min = *(arr_rate_change.begin());
      max = *(arr_rate_change.end() - 1);
      if (size % 2 == 0) {
        median = (arr_rate_change[size / 2] + \
            arr_rate_change[(size / 2) - 1]) / 2;
      } else {
        median = arr_rate_change[(size - 1) / 2];
      }
    }

    printf("; arr_rate_change (min/median/max) = %f / %f / %f\n", min,
        median, max);
  }
}

void dram_t::visualize() const {
  printf("RRDc=%d CCDc=%d mrqq.Length=%d rwq.Length=%d\n", RRDc, CCDc,
         mrqq->get_length(), rwq->get_length());
  for (unsigned i = 0; i < m_config->nbk; i++) {
    printf("BK%d: state=%c curr_row=%03x, %2d %2d %2d %2d %p ", i, bk[i]->state,
           bk[i]->curr_row, bk[i]->RCDc, bk[i]->RASc, bk[i]->RPc, bk[i]->RCc,
           bk[i]->mrq);
    if (bk[i]->mrq)
      printf("txf: %d %d", bk[i]->mrq->nbytes, bk[i]->mrq->txbytes);
    printf("\n");
  }
  if (m_scheduler) m_scheduler->print(stdout);
}

void dram_t::print_stat(FILE *simFile) {
  fprintf(simFile,
          "DRAM (%u): n_cmd=%llu n_nop=%llu n_act=%llu n_pre=%llu n_ref=%llu "
          "n_req=%llu n_rd=%llu n_write=%llu bw_util=%.4g ",
          id, n_cmd, n_nop, n_act, n_pre, n_ref, n_req, n_rd, n_wr,
          (float)bwutil / n_cmd);
  fprintf(simFile, "mrqq: %d %.4g mrqsmax=%llu ", max_mrqs,
          (float)ave_mrqs / n_cmd, max_mrqs_temp);
  fprintf(simFile, "mrqq_pim: %d %.4g mrqsmax_pim=%llu ", max_pim_mrqs,
          (float)ave_pim_mrqs / n_cmd, max_pim_mrqs_temp);
  fprintf(simFile, "\n");
  fprintf(simFile, "dram_util_bins:");
  for (unsigned i = 0; i < 10; i++) fprintf(simFile, " %d", dram_util_bins[i]);
  fprintf(simFile, "\ndram_eff_bins:");
  for (unsigned i = 0; i < 10; i++) fprintf(simFile, " %d", dram_eff_bins[i]);
  fprintf(simFile, "\n");
  max_pim_mrqs_temp = 0;
}

void dram_t::visualizer_print(gzFile visualizer_file) {
  // dram specific statistics
  gzprintf(visualizer_file, "dramncmd: %u %u\n", id, n_cmd_partial);
  gzprintf(visualizer_file, "dramnop: %u %u\n", id, n_nop_partial);
  gzprintf(visualizer_file, "dramnact: %u %u\n", id, n_act_partial);
  gzprintf(visualizer_file, "dramnpre: %u %u\n", id, n_pre_partial);
  gzprintf(visualizer_file, "dramnreq: %u %u\n", id, n_req_partial);
  gzprintf(visualizer_file, "dramavemrqs: %u %u\n", id,
           n_cmd_partial ? (ave_mrqs_partial / n_cmd_partial) : 0);
  gzprintf(visualizer_file, "dramavepimmrqs: %u %u\n", id,
           n_cmd_partial ? (ave_pim_mrqs_partial / n_cmd_partial) : 0);

  // utilization and efficiency
  gzprintf(visualizer_file, "dramutil: %u %u\n", id,
           n_cmd_partial ? 100 * bwutil_partial / n_cmd_partial : 0);
  gzprintf(visualizer_file, "drameff: %u %u\n", id,
           n_activity_partial ? 100 * bwutil_partial / n_activity_partial : 0);

  // reset for next interval
  bwutil_partial = 0;
  n_activity_partial = 0;
  ave_mrqs_partial = 0;
  ave_pim_mrqs_partial = 0;
  n_cmd_partial = 0;
  n_nop_partial = 0;
  n_act_partial = 0;
  n_pre_partial = 0;
  n_req_partial = 0;

  // dram access type classification
  for (unsigned j = 0; j < m_config->nbk; j++) {
    gzprintf(visualizer_file, "dramglobal_acc_r: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[GLOBAL_ACC_R][id][j]);
    gzprintf(visualizer_file, "dramglobal_acc_w: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[GLOBAL_ACC_W][id][j]);
    gzprintf(visualizer_file, "dramlocal_acc_r: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[LOCAL_ACC_R][id][j]);
    gzprintf(visualizer_file, "dramlocal_acc_w: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[LOCAL_ACC_W][id][j]);
    gzprintf(visualizer_file, "dramconst_acc_r: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[CONST_ACC_R][id][j]);
    gzprintf(visualizer_file, "dramtexture_acc_r: %u %u %u\n", id, j,
             m_stats->mem_access_type_stats[TEXTURE_ACC_R][id][j]);
  }
}

void dram_t::set_dram_power_stats(unsigned &cmd, unsigned &activity,
                                  unsigned &nop, unsigned &act, unsigned &pre,
                                  unsigned &rd, unsigned &wr,
                                  unsigned &req) const {
  // Point power performance counters to low-level DRAM counters
  cmd = n_cmd;
  activity = n_activity;
  nop = n_nop;
  act = n_act;
  pre = n_pre;
  rd = n_rd;
  wr = n_wr + n_pim;
  req = n_req;
}

unsigned dram_t::get_bankgrp_number(unsigned i) {
  if (m_config->dram_bnkgrp_indexing_policy == HIGHER_BITS) {  // higher bits
    return i >> m_config->bk_tag_length;
  } else if (m_config->dram_bnkgrp_indexing_policy ==
             LOWER_BITS) {  // lower bits
    return i & ((m_config->nbkgrp - 1));
  } else {
    assert(1);
  }
}
