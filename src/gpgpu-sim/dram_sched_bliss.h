#ifndef __DRAM_SCHED_BLISS_H__
#define __DRAM_SCHED_BLISS_H__

#include <list>
#include <map>
#include "dram.h"
#include "dram_sched.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

enum request_type { REQ_NONE = 0, REQ_MEM, REQ_PIM};

class bliss_scheduler : public dram_scheduler {
 public:
  bliss_scheduler(const memory_config *config, dram_t *dm,
                   memory_stats_t *stats);
  void update_mode() override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  // Statistics
  unsigned long long m_cycles_none_blacklisted;
  unsigned long long m_cycles_both_blacklisted;
  unsigned long long m_cycles_pim_blacklisted;
  unsigned long long m_cycles_mem_blacklisted;

 private:
  // State for FR-FCFS
  unsigned m_curr_pim_row;
  std::vector<bool> m_bank_issued_mem_req;
  std::vector<bool> m_bank_ready_to_switch;
  unsigned m_num_bypasses;

  std::list<std::list<dram_req_t *>::iterator> *m_pim_queue_it;

  unsigned m_requests_served;
  enum request_type m_prev_request_type;

  bool is_pim_blacklisted;
  bool is_mem_blacklisted;

  void update_blacklist(request_type req_type);
};

#endif
