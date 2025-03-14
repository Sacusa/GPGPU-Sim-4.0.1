// Copyright (c) 2019, Mahmoud Khairy
// Purdue University
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

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

#include "local_interconnect.h"
#include "mem_fetch.h"

xbar_router::xbar_router(unsigned router_id, enum Interconnect_type m_type,
                         unsigned n_shader, unsigned n_mem, unsigned n_vcs,
                         const struct icnt_config& m_localicnt_config) {
  m_id = router_id;
  router_type = m_type;
  _n_mem = n_mem;
  _n_shader = n_shader;
  total_nodes = n_shader + n_mem;
  verbose = m_localicnt_config.verbose;
  grant_cycles = m_localicnt_config.grant_cycles;
  grant_cycles_count = m_localicnt_config.grant_cycles;
  next_node.resize(total_nodes, 0);
  arbit_type = m_localicnt_config.arbiter_algo;
  next_node_id = 0;
  num_vcs = n_vcs;

  in_buffer_limit = m_localicnt_config.in_buffer_limit / num_vcs;
  out_buffer_limit = m_localicnt_config.out_buffer_limit / num_vcs;
  in_buffers.resize(total_nodes);
  out_buffers.resize(total_nodes);

  for (unsigned i = 0; i < total_nodes; i++) {
    in_buffers[i].resize(num_vcs);
    out_buffers[i].resize(num_vcs);
  }

  if (m_type == REQ_NET) {
    active_in_buffers = n_shader;
    active_out_buffers = n_mem;
  } else if (m_type == REPLY_NET) {
    active_in_buffers = n_mem;
    active_out_buffers = n_shader;
  }

  prev_vc_advanced.resize(total_nodes);
  prev_vc_popped.resize(total_nodes);

  cycles = 0;
  conflicts = 0;
  out_buffer_full = 0;
  in_buffer_full = 0;
  out_buffer_util_total.resize(num_vcs, 0);
  in_buffer_util_total.resize(num_vcs, 0);
  out_buffer_util_peak.resize(num_vcs, 0);
  in_buffer_util_peak.resize(num_vcs, 0);
  packets_num.resize(num_vcs, 0);
  conflicts_util = 0;
  cycles_util = 0;
  reqs_util = 0;
}

xbar_router::~xbar_router() {}

void xbar_router::Push(unsigned input_deviceID, unsigned output_deviceID,
                       unsigned vc, void* data, unsigned int size) {
  assert((input_deviceID < total_nodes) && (vc < num_vcs));
  in_buffers[input_deviceID][vc].push(Packet(data, output_deviceID));
  packets_num[vc]++;

  if (verbose) {
    printf("%d : cycle %d : pushing new request from id/vc %u/%u. "
        "Occupancy = %u\n", m_id, cycles, input_deviceID, vc,
        in_buffers[input_deviceID][vc].size());
  }
}

void* xbar_router::Pop(unsigned output_deviceID, unsigned vc) {
  assert(output_deviceID < total_nodes);
  void *data = NULL;

  if (!out_buffers[output_deviceID][vc].empty()) {
    data = out_buffers[output_deviceID][vc].front().data;
    out_buffers[output_deviceID][vc].pop();

    if (verbose) {
      printf("%d : cycle %d : popping request for id/vc %u/%u. "
          "Occupancy = %u\n", m_id, cycles, output_deviceID, vc,
          out_buffers[output_deviceID][vc].size());
    }
  }

  return data;
}

bool xbar_router::Has_Buffer_In(unsigned input_deviceID, unsigned vc,
                                unsigned size, bool update_counter) {
  assert((input_deviceID < total_nodes) && (vc < num_vcs));

  bool has_buffer =
      (in_buffers[input_deviceID][vc].size() + size <= in_buffer_limit);
  if (update_counter && !has_buffer) in_buffer_full++;

  return has_buffer;
}

bool xbar_router::Has_Buffer_Out(unsigned output_deviceID, unsigned vc,
                                 unsigned size) {
  return (out_buffers[output_deviceID][vc].size() + size <= out_buffer_limit);
}

void xbar_router::Advance() {
  if (arbit_type == NAIVE_RR)
    RR_Advance();
  else if (arbit_type == iSLIP)
    iSLIP_Advance();
  else
    assert(0);
}

void xbar_router::RR_Advance() {
  bool active = false;
  vector<bool> issued(total_nodes, false);
  unsigned conflict_sub = 0;
  unsigned reqs = 0;

  for (unsigned i = 0; i < total_nodes; ++i) {
    unsigned node_id = (i + next_node_id) % total_nodes;

    for (unsigned vc_iter = 0; vc_iter < num_vcs; vc_iter++) {
      unsigned vc = (prev_vc_advanced[node_id] + vc_iter + 1) % num_vcs;

      if (!in_buffers[node_id][vc].empty()) {
        active = true;
        Packet _packet = in_buffers[node_id][vc].front();

        // Ensure that the outbuffer has space and not issued before in this
        // cycle
        if (Has_Buffer_Out(_packet.output_deviceID, vc, 1)) {
          if (!issued[_packet.output_deviceID]) {
            out_buffers[_packet.output_deviceID][vc].push(_packet);
            in_buffers[node_id][vc].pop();
            prev_vc_advanced[node_id] = vc;

            // Don't need VC here because only one VC per physical link issues
            // every cycle
            issued[_packet.output_deviceID] = true;
            reqs++;

            if (verbose) {
              printf("%d : cycle %d : send req from %d to %d over vc %u\n",
                  m_id, cycles, node_id, i, vc);
            }

            break;
          } else {
            conflict_sub++;
          }
        } else {
          out_buffer_full++;

          if (issued[_packet.output_deviceID]) { conflict_sub++; }
        }
      }
    }
  }

  next_node_id = (++next_node_id % total_nodes);

  conflicts += conflict_sub;
  if (active) {
    conflicts_util += conflict_sub;
    cycles_util++;
    reqs_util += reqs;
  }

  if (verbose) {
    printf("%d : cycle %d : conflicts = %d\n", m_id, cycles, conflict_sub);
    printf("%d : cycle %d : passing reqs = %d\n", m_id, cycles, reqs);
  }

  // collect some stats about buffer util
  for (unsigned vc = 0; vc < num_vcs; vc++) {
    for (unsigned i = 0; i < total_nodes; ++i) {
      in_buffer_util_total[vc] += in_buffers[i][vc].size();
      out_buffer_util_total[vc] += out_buffers[i][vc].size();
      in_buffer_util_peak[vc] = std::max(in_buffer_util_peak[vc],
          (long long unsigned) in_buffers[i][vc].size());
      out_buffer_util_peak[vc] = std::max(out_buffer_util_peak[vc],
          (long long unsigned) out_buffers[i][vc].size());
    }
  }

  cycles++;
}

// iSLIP algorithm
// McKeown, Nick. "The iSLIP scheduling algorithm for input-queued switches."
// IEEE/ACM transactions on networking 2 (1999): 188-201.
// https://www.cs.rutgers.edu/~sn624/552-F18/papers/islip.pdf
void xbar_router::iSLIP_Advance() {
  vector<unsigned> node_tmp;
  bool active = false;

  unsigned conflict_sub = 0;
  unsigned reqs = 0;

  // calcaulte how many conflicts are there for stats
  for (unsigned i = 0; i < total_nodes; ++i) {
    for (unsigned vc = 0; vc < num_vcs; vc++) {
      if (!in_buffers[i][vc].empty()) {
        Packet _packet_tmp = in_buffers[i][vc].front();
        if (!node_tmp.empty()) {
          if (std::find(node_tmp.begin(), node_tmp.end(),
                        _packet_tmp.output_deviceID) != node_tmp.end()) {
            conflict_sub++;
          } else
            node_tmp.push_back(_packet_tmp.output_deviceID);
        } else {
          node_tmp.push_back(_packet_tmp.output_deviceID);
        }
        active = true;
      }
    }
  }

  conflicts += conflict_sub;
  if (active) {
    conflicts_util += conflict_sub;
    cycles_util++;
  }

  // do iSLIP
  for (unsigned i = 0; i < total_nodes; ++i) {
    vector<bool> has_buffer_out;
    bool all_out_buffers_full = true;  // Used to skip if there is no space
    vector<bool> has_pending_request;  // Used to compute out_buffer_full

    for (unsigned vc = 0; vc < num_vcs; vc++) {
      bool vc_has_buffer_out = Has_Buffer_Out(i, vc, 1);
      has_buffer_out.push_back(vc_has_buffer_out);
      all_out_buffers_full = all_out_buffers_full && !vc_has_buffer_out;

      has_pending_request.push_back(false);
    }

    // All output buffers are full; move on
    if (all_out_buffers_full) { continue; }

    bool issued = false;

    for (unsigned j = 0; (j < total_nodes) && !issued; ++j) {
      unsigned node_id = (j + next_node[i]) % total_nodes;

      for (unsigned vc_iter = 0; vc_iter < num_vcs; vc_iter++) {
        unsigned vc = (prev_vc_advanced[node_id] + vc_iter + 1) % num_vcs;

        if (!in_buffers[node_id][vc].empty()) {
          Packet _packet = in_buffers[node_id][vc].front();

          if (_packet.output_deviceID == i) {
            has_pending_request[vc] = true;

            if (!has_buffer_out[vc]) { continue; }

            out_buffers[_packet.output_deviceID][vc].push(_packet);
            in_buffers[node_id][vc].pop();
            prev_vc_advanced[node_id] = vc;

            if (verbose) {
              printf("%d : cycle %d : send req from %d to %d over vc %u\n",
                  m_id, cycles, node_id, i, vc);
            }

            if (grant_cycles_count == 1) {
              next_node[i] = (++node_id % total_nodes);
            }

            if (verbose) {
              for (unsigned k = j + 1; k < total_nodes; ++k) {
                unsigned node_id2 = (k + next_node[i]) % total_nodes;
                if (!in_buffers[node_id2][vc].empty()) {
                  Packet _packet2 = in_buffers[node_id2][vc].front();

                  if (_packet2.output_deviceID == i)
                    printf("%d : cycle %d : cannot send req from %d to %d over"
                        " vc %u\n", m_id, cycles, node_id2, i, vc);
                }
              }
            }

            reqs++;
            issued = true;
            break;
          }
        }
      }
    }

    for (unsigned vc = 0; vc < num_vcs; vc++) {
      if (has_pending_request[vc] && !has_buffer_out[vc]) {
        out_buffer_full++;
      }
    }
  }

  if (active) {
    reqs_util += reqs;
  }

  if (verbose)
    printf("%d : cycle %d : grant_cycles = %d\n", m_id, cycles, grant_cycles);

  if (active && grant_cycles_count == 1)
    grant_cycles_count = grant_cycles;
  else if (active)
    grant_cycles_count--;

  if (verbose) {
    printf("%d : cycle %d : conflicts = %d\n", m_id, cycles, conflict_sub);
    printf("%d : cycle %d : passing reqs = %d\n", m_id, cycles, reqs);
  }

  // collect some stats about buffer util
  for (unsigned vc = 0; vc < num_vcs; vc++) {
    for (unsigned i = 0; i < total_nodes; ++i) {
      in_buffer_util_total[vc] += in_buffers[i][vc].size();
      out_buffer_util_total[vc] += out_buffers[i][vc].size();
      in_buffer_util_peak[vc] = std::max(in_buffer_util_peak[vc],
          (long long unsigned) in_buffers[i][vc].size());
      out_buffer_util_peak[vc] = std::max(out_buffer_util_peak[vc],
          (long long unsigned) out_buffers[i][vc].size());
    }
  }

  cycles++;
}

bool xbar_router::Busy() const {
  for (unsigned i = 0; i < total_nodes; ++i) {
    for (unsigned vc = 0; vc < num_vcs; vc++) {
      if (!in_buffers[i][vc].empty()) return true;
      if (!out_buffers[i][vc].empty()) return true;
    }
  }
  return false;
}

////////////////////////////////////////////////////
/////////////LocalInterconnect/////////////////////

// assume all the packets are one flit
#define LOCAL_INCT_FLIT_SIZE 40

LocalInterconnect* LocalInterconnect::New(
    const struct icnt_config& m_localicnt_config) {
  LocalInterconnect* icnt_interface = \
    new LocalInterconnect(m_localicnt_config);

  return icnt_interface;
}

LocalInterconnect::LocalInterconnect(
    const struct icnt_config& m_localicnt_config)
    : m_icnt_config(m_localicnt_config) {
  n_shader = 0;
  n_mem = 0;
  n_subnets = m_localicnt_config.subnets;
  n_shader_to_mem_vcs = m_localicnt_config.shader_to_mem_vcs;
}

LocalInterconnect::~LocalInterconnect() {
  for (unsigned i = 0; i < m_icnt_config.subnets; ++i) {
    delete net[i];
  }
}

void LocalInterconnect::CreateInterconnect(unsigned m_n_shader,
                                           unsigned m_n_mem) {
  n_shader = m_n_shader;
  n_mem = m_n_mem;

  net.resize(n_subnets);

  for (unsigned i = 0; i < n_subnets; ++i) {
    enum Interconnect_type icnt_type = static_cast<Interconnect_type>(i);

    unsigned num_vcs = 1;
    if (icnt_type == REQ_NET) { num_vcs = n_shader_to_mem_vcs; }

    net[i] = new xbar_router(i, icnt_type, m_n_shader, m_n_mem, num_vcs,
        m_icnt_config);
  }
}

void LocalInterconnect::Init() {
  // empty
  // there is nothing to do
}

void LocalInterconnect::Push(unsigned input_deviceID, unsigned output_deviceID,
                             void* data, unsigned int size, bool is_pim) {
  unsigned subnet, vc;
  if (n_subnets == 1) {
    subnet = 0;
    vc = 0;
  } else {
    if (input_deviceID < n_shader) {
      subnet = 0;
      if (is_pim && (n_shader_to_mem_vcs > 1)) { vc = PIM_VC; }
      else                                     { vc = MEM_VC; }
    } else {
      subnet = 1;
      vc = 0;
    }
  }

  // it should have free buffer
  // assume all the packets have size of one
  // no flits are implemented
  assert(net[subnet]->Has_Buffer_In(input_deviceID, vc, 1));

  net[subnet]->Push(input_deviceID, output_deviceID, vc, data, size);
}

void* LocalInterconnect::Pop(unsigned output_deviceID, unsigned vc) {
  // 0-_n_shader-1 indicates reply(network 1), otherwise request(network 0)
  int subnet = 0;
  if (output_deviceID < n_shader) subnet = 1;

  return net[subnet]->Pop(output_deviceID, vc);
}

void LocalInterconnect::Advance() {
  for (unsigned i = 0; i < n_subnets; ++i) {
    net[i]->Advance();
  }
}

bool LocalInterconnect::Busy() const {
  for (unsigned i = 0; i < n_subnets; ++i) {
    if (net[i]->Busy()) return true;
  }
  return false;
}

bool LocalInterconnect::HasBuffer(unsigned deviceID, unsigned int size,
    bool is_pim) const {
  enum Interconnect_type icnt_type;
  unsigned vc;

  if ((n_subnets > 1) && deviceID >= n_shader) {  // deviceID is memory node
    icnt_type = REPLY_NET;
    vc = 0;
  } else {
    icnt_type = REQ_NET;
    if (is_pim && (n_shader_to_mem_vcs > 1)) { vc = PIM_VC; }
    else                                     { vc = MEM_VC; }
  }

  return net[icnt_type]->Has_Buffer_In(deviceID, vc, 1, true);
}

void LocalInterconnect::DisplayStats() const {
  printf("Req_Network_cycles = %lld\n", net[REQ_NET]->cycles);
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Req_Network_injected_packets_num (vc=%u) = %lld\n",
           vc, net[REQ_NET]->packets_num[vc]);
    printf("Req_Network_injected_packets_per_cycle (vc=%u) = %12.4f \n",
           vc,
           (float)(net[REQ_NET]->packets_num[vc]) / (net[REQ_NET]->cycles));
  }
  printf("Req_Network_conflicts_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->conflicts) / (net[REQ_NET]->cycles));
  printf("Req_Network_conflicts_per_cycle_util = %12.4f\n",
         (float)(net[REQ_NET]->conflicts_util) / (net[REQ_NET]->cycles_util));
  printf("Req_Bank_Level_Parallism = %12.4f\n",
         (float)(net[REQ_NET]->reqs_util) / (net[REQ_NET]->cycles_util));

  printf("Req_Network_in_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->in_buffer_full) / (net[REQ_NET]->cycles));
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Req_Network_in_buffer_avg_util (vc=%u) = %12.4f\n", vc,
           ((float)(net[REQ_NET]->in_buffer_util_total[vc]) /
            (net[REQ_NET]->cycles) / net[REQ_NET]->active_in_buffers));
    printf("Req_Network_in_buffer_peak_util (vc=%u) = %12.4f\n", vc,
           (float)(net[REQ_NET]->in_buffer_util_peak[vc]));
  }

  printf("Req_Network_out_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REQ_NET]->out_buffer_full) / (net[REQ_NET]->cycles));
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Req_Network_out_buffer_avg_util (vc=%u) = %12.4f\n", vc,
           ((float)(net[REQ_NET]->out_buffer_util_total[vc]) /
            (net[REQ_NET]->cycles) / net[REQ_NET]->active_out_buffers));
    printf("Req_Network_out_buffer_peak_util (vc=%u) = %12.4f\n", vc,
           (float)(net[REQ_NET]->out_buffer_util_peak[vc]));
  }

  printf("\n");
  printf("Reply_Network_cycles = %lld\n", net[REPLY_NET]->cycles);
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Reply_Network_injected_packets_num (vc=%u) = %lld\n",
           vc, net[REPLY_NET]->packets_num[vc]);
    printf("Reply_Network_injected_packets_per_cycle (vc=%u) =  %12.4f\n",
           vc,
           (float)(net[REPLY_NET]->packets_num[vc]) / (net[REPLY_NET]->cycles));
  }
  printf("Reply_Network_conflicts_per_cycle =  %12.4f\n",
         (float)(net[REPLY_NET]->conflicts) / (net[REPLY_NET]->cycles));
  printf(
      "Reply_Network_conflicts_per_cycle_util = %12.4f\n",
      (float)(net[REPLY_NET]->conflicts_util) / (net[REPLY_NET]->cycles_util));
  printf("Reply_Bank_Level_Parallism = %12.4f\n",
         (float)(net[REPLY_NET]->reqs_util) / (net[REPLY_NET]->cycles_util));

  printf("Reply_Network_in_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REPLY_NET]->in_buffer_full) / (net[REPLY_NET]->cycles));
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Reply_Network_in_buffer_avg_util (vc=%u) = %12.4f\n", vc,
           ((float)(net[REPLY_NET]->in_buffer_util_total[vc]) /
            (net[REPLY_NET]->cycles) / net[REPLY_NET]->active_in_buffers));
    printf("Reply_Network_in_buffer_peak_util (vc=%u) = %12.4f\n", vc,
           (float)(net[REPLY_NET]->in_buffer_util_peak[vc]));
  }

  printf("Reply_Network_out_buffer_full_per_cycle = %12.4f\n",
         (float)(net[REPLY_NET]->out_buffer_full) / (net[REPLY_NET]->cycles));
  for (unsigned vc = 0; vc < n_shader_to_mem_vcs; vc++) {
    printf("Reply_Network_out_buffer_avg_util (vc=%u) = %12.4f\n", vc,
           ((float)(net[REPLY_NET]->out_buffer_util_total[vc]) /
            (net[REPLY_NET]->cycles) / net[REPLY_NET]->active_out_buffers));
    printf("Reply_Network_out_buffer_peak_util (vc=%u) = %12.4f\n", vc,
           (float)(net[REPLY_NET]->out_buffer_util_peak[vc]));
  }
}

void LocalInterconnect::DisplayOverallStats() const {}

unsigned LocalInterconnect::GetFlitSize() const { return LOCAL_INCT_FLIT_SIZE; }

void LocalInterconnect::DisplayState(FILE* fp) const {
  fprintf(fp, "GPGPU-Sim uArch: ICNT:Display State: Under implementation\n");
}
