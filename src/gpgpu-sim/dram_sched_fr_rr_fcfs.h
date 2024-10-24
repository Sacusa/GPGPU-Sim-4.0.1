#ifndef __DRAM_SCHED_FR_RR_FCFS_H__
#define __DRAM_SCHED_FR_RR_FCFS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum fr_rr_fcfs_switch_reason {
    FR_RR_FCFS_OUT_OF_REQUESTS = 0,
    FR_RR_FCFS_ROW_BUFFER_CONFLICT,
    FR_RR_FCFS_NUM_SWITCH_REASONS
};

const std::string fr_rr_fcfs_switch_reason_str[] = {"OutOfRequests",
    "RowBufferConflict"};

class fr_rr_fcfs_scheduler : public dram_scheduler {
 public:
  fr_rr_fcfs_scheduler(const memory_config *config, dram_t *dm,
          memory_stats_t *stats);
  void add_req(dram_req_t *req) override;
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  // Stats
  std::vector<fr_rr_fcfs_switch_reason> m_mem2pim_switch_reason;
  std::vector<fr_rr_fcfs_switch_reason> m_pim2mem_switch_reason;
  std::vector<unsigned> m_max_mem_requests_issued_at_any_bank;
  std::vector<unsigned> m_pim_requests_issued;

 private:
  unsigned m_last_pim_row;

  std::vector<unsigned long long> m_bank_pending_mem_requests;
  std::vector<bool> m_bank_switch_to_pim;

  unsigned m_num_exec_pim;
  std::vector<unsigned> m_num_exec_mem_per_bank;
};

#endif
