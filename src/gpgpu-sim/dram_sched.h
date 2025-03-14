// Copyright (c) 2009-2011, Tor M. Aamodt, Ali Bakhoda, George L. Yuan
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

#ifndef __DRAM_SCHED_H__
#define __DRAM_SCHED_H__

#include <list>
#include <map>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum frfcfs_switch_reason {
  FRFCFS_OLDEST_FIRST = 0,
  FRFCFS_OUT_OF_REQUESTS,
  FRFCFS_CAP_EXCEEDED,
  FRFCFS_NUM_SWITCH_REASONS
};

const std::string frfcfs_switch_reason_str[] = {"OldestFirst", "OutOfRequests",
  "CapExceeded"};

class dram_scheduler {
 public:
  dram_scheduler(const memory_config *config, dram_t *dm,
                   memory_stats_t *stats);
  virtual void add_req(dram_req_t *req);
  void data_collection(unsigned bank);
  bool is_next_req_hit(unsigned bank, unsigned curr_row,
      enum memory_mode mode);

  virtual void update_mode();
  virtual dram_req_t *schedule(unsigned bank, unsigned curr_row);
  virtual dram_req_t *schedule_pim();

  void print(FILE *fp);
  unsigned num_pending() const { return m_num_pending; }
  unsigned num_write_pending() const { return m_num_write_pending; }
  unsigned num_pim_pending() const { return m_num_pim_pending; }

  // Stats
  std::map<frfcfs_switch_reason, unsigned> m_mem2pim_switch_reason;
  std::map<frfcfs_switch_reason, unsigned> m_pim2mem_switch_reason;

 private:
  unsigned m_curr_pim_row;
  std::vector<bool> m_bank_issued_mem_req;
  std::vector<bool> m_bank_ready_to_switch;
  unsigned m_num_bypasses;

 protected:
  void update_rw_mode();

  const memory_config *m_config;
  dram_t *m_dram;
  unsigned m_num_pending;
  unsigned m_num_write_pending;
  unsigned m_num_pim_pending;
  std::list<dram_req_t *> *m_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> > *m_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_last_row;
  unsigned *curr_row_service_time;  // one set of variables for each bank.
  unsigned *row_service_timestamp;  // tracks when scheduler began servicing
                                    // current row

  std::list<dram_req_t *> *m_write_queue;
  std::map<unsigned, std::list<std::list<dram_req_t *>::iterator> >
      *m_write_bins;
  std::list<std::list<dram_req_t *>::iterator> **m_last_write_row;

  std::list<dram_req_t *> *m_pim_queue;

  memory_stats_t *m_stats;

  friend class dram_t;
};

#endif
