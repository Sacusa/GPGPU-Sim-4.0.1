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

  m_requests_served = 0;
  m_prev_request_type = REQ_NONE;

  is_pim_blacklisted = false;
  is_mem_blacklisted = false;

  m_cycles_none_blacklisted = 0;
  m_cycles_both_blacklisted = 0;
  m_cycles_pim_blacklisted = 0;
  m_cycles_mem_blacklisted = 0;

  m_curr_pim_row = 0;
  m_bank_issued_mem_req.resize(m_config->nbk, false);
  m_bank_ready_to_switch.resize(m_config->nbk, false);
  m_num_bypasses = 0;

  assert(m_config->frfcfs_cap == 0);
}

void bliss_scheduler::update_mode() {
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

  if (is_pim_blacklisted != is_mem_blacklisted) {
    if (is_pim_blacklisted) {
      if (m_num_pending > 0)          { m_dram->mode = READ_MODE; }
      else if (m_num_pim_pending > 0) { m_dram->mode = PIM_MODE; }

      m_cycles_pim_blacklisted++;
    }

    else {
      if (m_num_pim_pending > 0)  { m_dram->mode = PIM_MODE; }
      else if (m_num_pending > 0) { m_dram->mode = READ_MODE; }

      m_cycles_mem_blacklisted++;
    }
  }

  else {
    if (is_pim_blacklisted) {
      m_cycles_both_blacklisted++;
    } else {
      m_cycles_none_blacklisted++;
    }
  }

  if (prev_mode != m_dram->mode) {
    if (prev_mode == PIM_MODE) {
      // Reset FR-FCFS state
      m_curr_pim_row = 0;
      m_num_bypasses = 0;

      m_dram->pim2nonpimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to non-PIM mode\n");
#endif
    } else {
      // Reset FR-FCFS state
      std::fill(m_bank_issued_mem_req.begin(), m_bank_issued_mem_req.end(),
          false);
      std::fill(m_bank_ready_to_switch.begin(), m_bank_ready_to_switch.end(),
          false);
      m_num_bypasses = 0;

      m_dram->nonpim2pimswitches++;

#ifdef DRAM_SCHED_VERIFY
      printf("DRAM: Switching to PIM mode\n");
#endif
    }
  }

  // If both/none of the applications are blacklisted, use FR-FCFS
  dram_scheduler::update_mode();
}

dram_req_t *bliss_scheduler::schedule(unsigned bank, unsigned curr_row) {
  dram_req_t *req = dram_scheduler::schedule(bank, curr_row);

  if (req) {
    update_blacklist(REQ_MEM);
  }

  return req;
}

dram_req_t *bliss_scheduler::schedule_pim() {
  dram_req_t *req = dram_scheduler::schedule_pim();

  if (req) {
    update_blacklist(REQ_PIM);
  }

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
