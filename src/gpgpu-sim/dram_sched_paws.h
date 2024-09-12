#ifndef __DRAM_SCHED_PAWS_H__
#define __DRAM_SCHED_PAWS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum switch_reason {
    OUT_OF_REQUESTS = 0,
    CAP_EXCEEDED,
    OLDEST_FIRST,
    NUM_SWITCH_REASONS
};

const std::string switch_reason_str[] = {"OutOfRequests", "CapExceeded",
    "OldestFirst"};

class paws_scheduler : public dram_scheduler {
 public:
  paws_scheduler(const memory_config *config, dram_t *dm,
          memory_stats_t *stats);
  void add_req(dram_req_t *req) override;
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  // Stats
  std::vector<unsigned long long> m_bank_pim_stall_time;
  std::vector<unsigned long long> m_bank_pim_waste_time;

  unsigned long long m_mem2pim_switch_ready_timestamp;
  std::vector<unsigned long long> m_mem2pim_switch_latency;

  std::vector<unsigned> m_mem_cap;
  std::vector<switch_reason> m_mem2pim_switch_reason;
  std::vector<switch_reason> m_pim2mem_switch_reason;
  std::vector<unsigned> m_max_mem_requests_issued_at_any_bank;
  std::vector<unsigned> m_pim_requests_issued;

 private:
  std::list<std::list<dram_req_t *>::iterator> *m_pim_queue_it;
  unsigned m_last_pim_row;

  std::vector<unsigned long long> m_bank_pending_mem_requests;
  std::vector<bool> m_bank_switch_to_pim;

  unsigned m_num_exec_pim;
  unsigned m_max_exec_mem_per_bank;
  std::vector<unsigned> m_num_exec_mem_per_bank;
};

#endif
