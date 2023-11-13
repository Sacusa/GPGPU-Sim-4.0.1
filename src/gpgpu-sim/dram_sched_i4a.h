#ifndef __DRAM_SCHED_I4A__
#define __DRAM_SCHED_I4A__

#include <list>
#include <map>
#include <math.h>
#include "dram.h"
#include "gpu-misc.h"
#include "gpu-sim.h"
#include "shader.h"

#define I4A_INTERVAL_LENGTH 10000
#define I4A_MAX_MEM_REQ_OCCUPANCY 0.8
#define I4A_MAX_PIM_BATCH_SIZE 64

class i4a_scheduler : public dram_scheduler {
 public:
  i4a_scheduler(const memory_config *config, dram_t *dm, memory_stats_t *stats);
  void update_mode() override;
  void add_req(dram_req_t *req) override;
  dram_req_t *schedule(unsigned bank, unsigned curr_row) override;
  dram_req_t *schedule_pim() override;

  void end_exploratory_phase();

 private:
  std::vector<unsigned> m_num_non_pim_reqs;
  std::vector<unsigned> m_max_non_pim_reqs;

  std::vector<unsigned long long> m_non_pim_req_start_time;
  std::vector<unsigned long long> m_non_pim_batch_dur;

  unsigned m_last_pim_row;
 
  unsigned long long m_pim_batch_start_time;
  unsigned long long m_pim_batch_dur;

  unsigned m_finished_pim_batches;
  unsigned m_max_pim_batches;
  unsigned m_prev_max_pim_batches;

  bool m_stable_state;
  unsigned long long m_next_update_cycle;
  unsigned long long m_prev_update_cycle;
  unsigned long long m_exploratory_phase_start_cycle;
  
  float m_stable_non_pim_arrival_rate;
  float m_curr_non_pim_arrival_rate;
  float m_non_pim_arrival_rate_tolerance;

  float m_curr_non_pim_completion_rate;
  std::vector<float> m_non_pim_completion_rate;
};

#endif
