// See LICENSE for license details

#include "simplenic.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <iostream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/mman.h>

char simplenic_t::KIND;

// DO NOT MODIFY PARAMS BELOW THIS LINE
#define TOKENS_PER_BIGTOKEN 7

#define SIMLATENCY_BT (this->LINKLATENCY / TOKENS_PER_BIGTOKEN)

#define BUFWIDTH streaming_bridge_driver_t::STREAM_WIDTH_BYTES
#define BUFBYTES (SIMLATENCY_BT * BUFWIDTH)
#define EXTRABYTES 1

#define FLIT_BITS 64
#define PACKET_MAX_FLITS 190
#define BITTIME_PER_QUANTA 512
#define CYCLES_PER_QUANTA (BITTIME_PER_QUANTA / FLIT_BITS)

static constexpr uint64_t EMPTY_SHMEM_ROUND_MARKER = 0xDEADBEEFDEADBEEFULL;

static void simplify_frac(int n, int d, int *nn, int *dd) {
  int a = n, b = d;

  // compute GCD
  while (b > 0) {
    int t = b;
    b = a % b;
    a = t;
  }

  *nn = n / a;
  *dd = d / a;
}

static void print_blocked_reason(const char *context,
                                 const char *name,
                                 uint32_t reason) {
  printf("SIMPLENIC DEBUG REGS [%s] %s=0x%02x "
         "{no_to_host_token:%u from_host_channels_not_ready:%u "
         "to_host_payload_queue_full:%u no_from_host_token:%u "
         "to_payload_blocked:%u from_token_blocked:%u "
         "pcie_in_backpressured:%u pcie_out_backpressured:%u}\n",
         context,
         name,
         reason,
         (reason >> 0) & 1u,
         (reason >> 1) & 1u,
         (reason >> 2) & 1u,
         (reason >> 3) & 1u,
         (reason >> 4) & 1u,
         (reason >> 5) & 1u,
         (reason >> 6) & 1u,
         (reason >> 7) & 1u);
}

static void print_queue_snapshot(const char *context,
                                 const char *name,
                                 uint32_t snapshot) {
  printf("SIMPLENIC DEBUG REGS [%s] %s=0x%08x "
         "{htnt_count:%u ntht_count:%u capture_ready:%u current_valid:%u "
         "pending_valid:%u htnt_enq(v=%u r=%u) htnt_deq(v=%u r=%u) "
         "ntht_enq(v=%u r=%u) ntht_deq(v=%u r=%u)}\n",
         context,
         name,
         snapshot,
         snapshot & 0xfu,
         (snapshot >> 4) & 0xfu,
         (snapshot >> 8) & 1u,
         (snapshot >> 9) & 1u,
         (snapshot >> 10) & 1u,
         (snapshot >> 12) & 1u,
         (snapshot >> 11) & 1u,
         (snapshot >> 13) & 1u,
         (snapshot >> 14) & 1u,
         (snapshot >> 16) & 1u,
         (snapshot >> 15) & 1u,
         (snapshot >> 17) & 1u,
         (snapshot >> 18) & 1u);
}

static void print_adapter_snapshot(const char *context,
                                   const char *name,
                                   uint32_t snapshot) {
  printf("SIMPLENIC DEBUG REGS [%s] %s=0x%08x "
         "{loop_iter_low:%u nicbig_out_q:%u htnt(v=%u r=%u) "
         "pcie_in(v=%u r=%u) ntht(v=%u r=%u) pcie_out(v=%u r=%u)}\n",
         context,
         name,
         snapshot,
         snapshot & 0x3u,
         (snapshot >> 2) & 0x3u,
         (snapshot >> 4) & 1u,
         (snapshot >> 5) & 1u,
         (snapshot >> 6) & 1u,
         (snapshot >> 7) & 1u,
         (snapshot >> 8) & 1u,
         (snapshot >> 9) & 1u,
         (snapshot >> 10) & 1u,
         (snapshot >> 11) & 1u);
}

static bool should_log_coop_wait(uint64_t count) {
  return count <= 4 || ((count & (count - 1)) == 0);
}

static uint32_t count_valid_flits(const char *buf, uint32_t bigtokens) {
  uint32_t valid = 0;
  const auto *words = reinterpret_cast<const uint64_t *>(buf);
  for (uint32_t bigtoken = 0; bigtoken < bigtokens; bigtoken++) {
    const uint64_t lrv = words[bigtoken * 8];
    for (int token = 0; token < TOKENS_PER_BIGTOKEN; token++) {
      valid += (lrv >> (43 + token * 3)) & 1L;
    }
  }
  return valid;
}

static void clear_bigtoken_headers(char *buf, uint32_t bigtokens) {
  auto *words = reinterpret_cast<uint64_t *>(buf);
  for (uint32_t bigtoken = 0; bigtoken < bigtokens; bigtoken++) {
    words[bigtoken * 8] = 0;
  }
}

static void print_valid_flit_sample(const char *context,
                                    const char *buf,
                                    uint32_t bigtokens,
                                    uint32_t max_samples) {
  const auto *words = reinterpret_cast<const uint64_t *>(buf);
  uint32_t samples = 0;
  for (uint32_t bigtoken = 0; bigtoken < bigtokens; bigtoken++) {
    const uint64_t lrv = words[bigtoken * 8];
    for (int token = 0; token < TOKENS_PER_BIGTOKEN; token++) {
      const bool valid = (lrv >> (43 + token * 3)) & 1L;
      if (!valid) {
        continue;
      }
      const bool last = (lrv >> (45 + token * 3)) & 1L;
      const uint64_t data = words[bigtoken * 8 + 1 + token];
      printf("SIMPLENIC DEBUG [%s] peer_flit_sample sample=%u bigtoken=%u "
             "token=%d last=%u data=0x%016lx\n",
             context,
             samples,
             bigtoken,
             token,
             last ? 1u : 0u,
             data);
      samples++;
      if (samples >= max_samples) {
        return;
      }
    }
  }
}

#define niclog_printf(...)                                                     \
  if (this->niclog) {                                                          \
    fprintf(this->niclog, __VA_ARGS__);                                        \
    fflush(this->niclog);                                                      \
  }

simplenic_t::simplenic_t(simif_t &sim,
                         StreamEngine &stream,
                         const SIMPLENICBRIDGEMODULE_struct &mmio_addrs,
                         int simplenicno,
                         const std::vector<std::string> &args,
                         const int stream_to_cpu_idx,
                         const int stream_to_cpu_depth,
                         const int stream_from_cpu_idx,
                         const int stream_from_cpu_depth)
    : streaming_bridge_driver_t(sim, stream, &KIND), mmio_addrs(mmio_addrs),
      stream_to_cpu_idx(stream_to_cpu_idx),
      stream_from_cpu_idx(stream_from_cpu_idx) {
  const char *niclogfile = nullptr;
  const char *shmemportname = nullptr;
  int netbw = MAX_BANDWIDTH, netburst = 8;

  this->loopback = false;
  this->niclog = nullptr;
  this->mac_lendian = 0;
  this->LINKLATENCY = 0;

  // construct arg parsing strings here. We basically append the bridge_driver
  // number to each of these base strings, to get args like +blkdev0 etc.
  std::string num_equals = std::to_string(simplenicno) + std::string("=");
  std::string niclog_arg = std::string("+niclog") + num_equals;
  std::string nicloopback_arg =
      std::string("+nic-loopback") + std::to_string(simplenicno);
  std::string macaddr_arg = std::string("+macaddr") + num_equals;
  std::string netbw_arg = std::string("+netbw") + num_equals;
  std::string netburst_arg = std::string("+netburst") + num_equals;
  std::string linklatency_arg = std::string("+linklatency") + num_equals;
  std::string shmemportname_arg = std::string("+shmemportname") + num_equals;
  std::string round_debug_arg =
      std::string("+simplenic-round-debug") + num_equals;
  std::string round_debug_global_arg = std::string("+simplenic-round-debug=");
  std::string token_debug_arg =
      std::string("+simplenic-token-debug") + num_equals;
  std::string token_debug_global_arg = std::string("+simplenic-token-debug=");
  std::string relaxed_required_arg =
      std::string("+simplenic-relaxed-required-bytes") + num_equals;
  std::string relaxed_required_global_arg =
      std::string("+simplenic-relaxed-required-bytes=");
  std::string relaxed_required_flag =
      std::string("+simplenic-relaxed-required-bytes") +
      std::to_string(simplenicno);
  std::string relaxed_required_global_flag =
      std::string("+simplenic-relaxed-required-bytes");
  std::string empty_poll_arg =
      std::string("+simplenic-empty-switch-poll-interval") + num_equals;
  std::string empty_poll_global_arg =
      std::string("+simplenic-empty-switch-poll-interval=");

  for (auto &arg : args) {
    if (arg.find(niclog_arg) == 0) {
      niclogfile = const_cast<char *>(arg.c_str()) + niclog_arg.length();
    }
    if (arg.find(nicloopback_arg) == 0) {
      this->loopback = true;
    }
    if (arg.find(macaddr_arg) == 0) {
      int mac_octets[6];
      char *macstring = nullptr;
      macstring = const_cast<char *>(arg.c_str()) + macaddr_arg.length();
      char trailingjunk;

      // convert mac address from string to 48 bit int
      if (6 == sscanf(macstring,
                      "%x:%x:%x:%x:%x:%x%c",
                      &mac_octets[0],
                      &mac_octets[1],
                      &mac_octets[2],
                      &mac_octets[3],
                      &mac_octets[4],
                      &mac_octets[5],
                      &trailingjunk)) {

        for (int i = 0; i < 6; i++) {
          mac_lendian |= (((uint64_t)(uint8_t)mac_octets[i]) << (8 * i));
        }
      } else {
        fprintf(stderr, "INVALID MAC ADDRESS SUPPLIED WITH +macaddrN=\n");
      }
    }
    if (arg.find(netbw_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + netbw_arg.length();
      netbw = atoi(str);
    }
    if (arg.find(netburst_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + netburst_arg.length();
      netburst = atoi(str);
    }
    if (arg.find(linklatency_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + linklatency_arg.length();
      this->LINKLATENCY = atoi(str);
    }
    if (arg.find(shmemportname_arg) == 0) {
      shmemportname =
          const_cast<char *>(arg.c_str()) + shmemportname_arg.length();
    }
    if (arg.find(round_debug_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + round_debug_arg.length();
      this->round_debug_limit = atoi(str);
    }
    if (arg.find(round_debug_global_arg) == 0) {
      char *str =
          const_cast<char *>(arg.c_str()) + round_debug_global_arg.length();
      this->round_debug_limit = atoi(str);
    }
    if (arg.find(token_debug_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + token_debug_arg.length();
      this->token_debug_limit = atoi(str);
    }
    if (arg.find(token_debug_global_arg) == 0) {
      char *str =
          const_cast<char *>(arg.c_str()) + token_debug_global_arg.length();
      this->token_debug_limit = atoi(str);
    }
    if (arg.find(relaxed_required_arg) == 0) {
      char *str =
          const_cast<char *>(arg.c_str()) + relaxed_required_arg.length();
      this->relaxed_required_bytes = atoi(str) != 0;
    }
    if (arg.find(relaxed_required_global_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) +
                  relaxed_required_global_arg.length();
      this->relaxed_required_bytes = atoi(str) != 0;
    }
    if (arg == relaxed_required_flag ||
        arg == relaxed_required_global_flag) {
      this->relaxed_required_bytes = true;
    }
    if (arg.find(empty_poll_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + empty_poll_arg.length();
      this->empty_switch_poll_interval = atoi(str);
    }
    if (arg.find(empty_poll_global_arg) == 0) {
      char *str =
          const_cast<char *>(arg.c_str()) + empty_poll_global_arg.length();
      this->empty_switch_poll_interval = atoi(str);
    }
  }

  if (stream_from_cpu_depth < SIMLATENCY_BT) {
    // Workaround: pick a smaller latency, or up-size the queue.
    std::cerr << "CPU-to-FPGA stream undersized for requested link latency."
              << " Available: " << stream_from_cpu_depth
              << " Required: " << SIMLATENCY_BT << std::endl;
    exit(1);
  }

  if (stream_to_cpu_depth < SIMLATENCY_BT) {
    // Workaround: pick a smaller latency, or up-size the queue.
    std::cerr << "FPGA-to-CPU stream undersized for requested link latency."
              << " Available: " << stream_to_cpu_depth
              << " Required: " << SIMLATENCY_BT << std::endl;
    exit(1);
  }

  assert(this->LINKLATENCY > 0);
  assert(this->LINKLATENCY % TOKENS_PER_BIGTOKEN == 0);
  assert(netbw <= MAX_BANDWIDTH);
  assert(netburst < 256);
  simplify_frac(netbw, MAX_BANDWIDTH, &rlimit_inc, &rlimit_period);
  rlimit_size = netburst;
  pause_threshold = PACKET_MAX_FLITS + this->LINKLATENCY;
  pause_quanta = pause_threshold / CYCLES_PER_QUANTA;
  pause_refresh = this->LINKLATENCY;

  printf("using link latency: %d cycles\n", this->LINKLATENCY);
  printf("using netbw: %d\n", netbw);
  printf("using netburst: %d\n", netburst);
  printf("using simplenic round debug: %u successful rounds\n",
         this->round_debug_limit);
  printf("using simplenic relaxed required bytes: %d\n",
         this->relaxed_required_bytes ? 1 : 0);
  printf("using simplenic empty switch poll interval: %u empty rounds\n",
         this->empty_switch_poll_interval);
  printf("using simplenic token debug: %u events\n",
         this->token_debug_limit);

  pending_peer_buf = (char *)malloc(BUFBYTES + EXTRABYTES);
  if (!pending_peer_buf) {
    fprintf(stderr, "Could not allocate SimpleNIC pending peer buffer\n");
    abort();
  }
  memset(pending_peer_buf, 0, BUFBYTES + EXTRABYTES);

  if (niclogfile) {
    this->niclog = fopen(niclogfile, "w");
    if (!this->niclog) {
      fprintf(stderr, "Could not open NIC log file: %s\n", niclogfile);
      abort();
    }
  }

  char name[257];
  int shmemfd;

  if (!loopback) {
    assert(shmemportname != nullptr);
    for (int j = 0; j < 2; j++) {
      printf("Using non-slot-id associated shmemportname:\n");
      sprintf(name, "/port_nts%s_%d", shmemportname, j);

      printf("opening/creating shmem region\n%s\n", name);
      shmemfd = shm_open(name, O_RDWR | O_CREAT, S_IRWXU);
      ftruncate(shmemfd, BUFBYTES + EXTRABYTES);
      pcis_read_bufs[j] = (char *)mmap(nullptr,
                                       BUFBYTES + EXTRABYTES,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED,
                                       shmemfd,
                                       0);

      printf("Using non-slot-id associated shmemportname:\n");
      sprintf(name, "/port_stn%s_%d", shmemportname, j);

      printf("opening/creating shmem region\n%s\n", name);
      shmemfd = shm_open(name, O_RDWR | O_CREAT, S_IRWXU);
      ftruncate(shmemfd, BUFBYTES + EXTRABYTES);
      pcis_write_bufs[j] = (char *)mmap(nullptr,
                                        BUFBYTES + EXTRABYTES,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED,
                                        shmemfd,
                                        0);
    }
  } else {
    for (int j = 0; j < 2; j++) {
      pcis_read_bufs[j] = (char *)malloc(BUFBYTES + EXTRABYTES);
      pcis_write_bufs[j] = pcis_read_bufs[j];
    }
  }

  printf("BUFBYTES %d\n", BUFBYTES);
}

simplenic_t::~simplenic_t() {
  if (this->niclog)
    fclose(this->niclog);
  if (pending_peer_buf)
    free(pending_peer_buf);
  if (loopback) {
    for (auto &pcis_read_buf : pcis_read_bufs)
      if (pcis_read_buf)
        free(pcis_read_buf);
  } else {
    for (int j = 0; j < 2; j++) {
      if (pcis_read_bufs[j])
        munmap(pcis_read_bufs[j], BUFBYTES + EXTRABYTES);
      if (pcis_write_bufs[j])
        munmap(pcis_write_bufs[j], BUFBYTES + EXTRABYTES);
    }
  }
}

void simplenic_t::dump_host_debug(const char *context,
                                  uint32_t requested_bytes,
                                  uint32_t actual_bytes) {
  auto read_u64 = [this](uint64_t lo_addr, uint64_t hi_addr) -> uint64_t {
    const uint64_t lo = read(lo_addr);
    const uint64_t hi = read(hi_addr);
    return (hi << 32) | lo;
  };

  const auto done = read(mmio_addrs.done);
  const auto minobs_build_marker = read(mmio_addrs.minobs_build_marker);
  const auto htnt_queue_count = read(mmio_addrs.htnt_queue_count);
  const auto ntht_queue_count = read(mmio_addrs.ntht_queue_count);
  const auto t_fire = read(mmio_addrs.t_fire);
  const auto to_host_fire = read(mmio_addrs.to_host_fire);
  const auto from_host_fire = read(mmio_addrs.from_host_fire);
  const auto hport_to_host_valid = read(mmio_addrs.hport_to_host_valid);
  const auto hport_to_host_ready = read(mmio_addrs.hport_to_host_ready);
  const auto hport_from_host_valid = read(mmio_addrs.hport_from_host_valid);
  const auto hport_from_host_ready = read(mmio_addrs.hport_from_host_ready);
  const auto target_in_valid = read(mmio_addrs.target_in_valid);
  const auto target_out_valid = read(mmio_addrs.target_out_valid);
  const auto target_cycle_ready = read(mmio_addrs.target_cycle_ready);
  const auto to_host_ready_drive = read(mmio_addrs.to_host_ready_drive);
  const auto from_host_all_ready = read(mmio_addrs.from_host_all_ready);
  const auto from_host_token_available =
      read(mmio_addrs.from_host_token_available);
  const auto from_host_payload_token_available =
      read(mmio_addrs.from_host_payload_token_available);
  const auto from_host_empty_token_available =
      read(mmio_addrs.from_host_empty_token_available);
  const auto from_host_empty_drop = read(mmio_addrs.from_host_empty_drop);
  const auto from_host_current_payload_valid =
      read(mmio_addrs.from_host_current_payload_valid);
  const auto from_host_pending_payload_valid =
      read(mmio_addrs.from_host_pending_payload_valid);
  const auto from_host_payload_capture =
      read(mmio_addrs.from_host_payload_capture);
  const auto from_host_payload_capture_ready =
      read(mmio_addrs.from_host_payload_capture_ready);
  const auto from_host_payload_deq =
      read(mmio_addrs.from_host_payload_deq);
  const auto from_host_empty_drop_fire =
      read(mmio_addrs.from_host_empty_drop_fire);
  const auto to_host_payload_valid = read(mmio_addrs.to_host_payload_valid);
  const auto to_host_empty_valid = read(mmio_addrs.to_host_empty_valid);
  const auto to_host_empty_suppressed =
      read(mmio_addrs.to_host_empty_suppressed);
  const auto to_host_payload_blocked =
      read(mmio_addrs.to_host_payload_blocked);
  const auto htnt_queue_enq_valid = read(mmio_addrs.htnt_queue_enq_valid);
  const auto htnt_queue_enq_ready = read(mmio_addrs.htnt_queue_enq_ready);
  const auto htnt_queue_deq_valid = read(mmio_addrs.htnt_queue_deq_valid);
  const auto htnt_queue_deq_ready = read(mmio_addrs.htnt_queue_deq_ready);
  const auto ntht_queue_enq_valid = read(mmio_addrs.ntht_queue_enq_valid);
  const auto ntht_queue_enq_ready = read(mmio_addrs.ntht_queue_enq_ready);
  const auto ntht_queue_deq_valid = read(mmio_addrs.ntht_queue_deq_valid);
  const auto ntht_queue_deq_ready = read(mmio_addrs.ntht_queue_deq_ready);
  const auto bigtoken_pcie_in_valid = read(mmio_addrs.bigtoken_pcie_in_valid);
  const auto bigtoken_pcie_in_ready = read(mmio_addrs.bigtoken_pcie_in_ready);
  const auto nicbig_pcie_out_valid = read(mmio_addrs.nicbig_pcie_out_valid);
  const auto nicbig_pcie_out_ready = read(mmio_addrs.nicbig_pcie_out_ready);
  const auto nicbig_output_queue_count =
      read(mmio_addrs.nicbig_output_queue_count);
  const auto current_blocked_reason =
      read(mmio_addrs.current_blocked_reason);
  const auto target_blocked_no_to_host_token =
      read(mmio_addrs.target_blocked_no_to_host_token);
  const auto target_blocked_from_host_channels =
      read(mmio_addrs.target_blocked_from_host_channels);
  const auto target_blocked_to_host_payload_queue =
      read(mmio_addrs.target_blocked_to_host_payload_queue);
  const auto debug_cycle =
      read_u64(mmio_addrs.debug_cycle_0, mmio_addrs.debug_cycle_1);
  const auto target_cycle_fire_count64 =
      read_u64(mmio_addrs.target_cycle_fire_count64_0,
               mmio_addrs.target_cycle_fire_count64_1);
  const auto target_cycle_ready_count64 =
      read_u64(mmio_addrs.target_cycle_ready_count64_0,
               mmio_addrs.target_cycle_ready_count64_1);
  const auto target_cycle_not_ready_count64 =
      read_u64(mmio_addrs.target_cycle_not_ready_count64_0,
               mmio_addrs.target_cycle_not_ready_count64_1);
  const auto from_host_payload_capture_count64 =
      read_u64(mmio_addrs.from_host_payload_capture_count64_0,
               mmio_addrs.from_host_payload_capture_count64_1);
  const auto from_host_empty_drop_count64 =
      read_u64(mmio_addrs.from_host_empty_drop_count64_0,
               mmio_addrs.from_host_empty_drop_count64_1);
  const auto to_host_payload_fire_count64 =
      read_u64(mmio_addrs.to_host_payload_fire_count64_0,
               mmio_addrs.to_host_payload_fire_count64_1);
  const auto to_host_empty_fire_count64 =
      read_u64(mmio_addrs.to_host_empty_fire_count64_0,
               mmio_addrs.to_host_empty_fire_count64_1);
  const auto pcie_in_fire_count = read(mmio_addrs.pcie_in_fire_count);
  const auto pcie_out_fire_count = read(mmio_addrs.pcie_out_fire_count);
  const auto write_poll = static_cast<unsigned>(
      static_cast<uint8_t>(pcis_write_bufs[currentround][BUFBYTES]));
  const auto read_poll = static_cast<unsigned>(
      static_cast<uint8_t>(pcis_read_bufs[currentround][BUFBYTES]));

  printf("SIMPLENIC DEBUG [%s] requested_bytes=%u actual_bytes=%u "
         "currentround=%d linklatency=%d stream_to_cpu_idx=%d "
         "stream_from_cpu_idx=%d bufbytes=%d loopback=%d done=%u "
         "minobs_marker=0x%08x write_poll=%u read_poll=%u "
         "successful_rounds=%llu round_debug_limit=%u "
         "relaxed_required_bytes=%d empty_poll_interval=%u "
         "round_pending=%d pending_token_bytes=%u "
         "pending_push_offset_bytes=%u pending_peer_snapshot=%d "
         "pending_peer_valid_flits=%u peer_wait_ticks=%llu "
         "push_blocked_ticks=%llu pull_empty_ticks=%llu\n",
         context,
         requested_bytes,
         actual_bytes,
         currentround,
         LINKLATENCY,
         stream_to_cpu_idx,
         stream_from_cpu_idx,
         BUFBYTES,
         loopback ? 1 : 0,
         done,
         minobs_build_marker,
         write_poll,
         read_poll,
         static_cast<unsigned long long>(successful_rounds),
         round_debug_limit,
         relaxed_required_bytes ? 1 : 0,
         empty_switch_poll_interval,
         round_pending ? 1 : 0,
         pending_token_bytes,
         pending_push_offset_bytes,
         pending_peer_snapshot_valid ? 1 : 0,
         pending_peer_valid_flits,
         static_cast<unsigned long long>(peer_wait_ticks),
         static_cast<unsigned long long>(push_blocked_ticks),
         static_cast<unsigned long long>(pull_empty_ticks));
  printf("SIMPLENIC DEBUG [%s] round_counters pulled=%llu partial=%llu "
         "empty=%llu empty_skipped_switch=%llu peer_input=%llu "
         "peer_empty=%llu last_peer_valid_flits=%u "
         "last_peer_input_round=%llu empty_pull_round=%d\n",
         context,
         static_cast<unsigned long long>(pulled_rounds_started),
         static_cast<unsigned long long>(partial_pull_rounds_started),
         static_cast<unsigned long long>(empty_rounds_started),
         static_cast<unsigned long long>(empty_rounds_skipped_switch),
         static_cast<unsigned long long>(peer_input_rounds),
         static_cast<unsigned long long>(peer_empty_rounds),
         last_peer_valid_flits,
         static_cast<unsigned long long>(last_peer_input_round),
         pending_round_from_empty_pull ? 1 : 0);
  printf("SIMPLENIC DEBUG REGS [%s] queues htnt=%u ntht=%u "
         "t_fire=%u to_host_fire=%u from_host_fire=%u "
         "hostport to_host(v=%u r=%u) from_host(v=%u r=%u) "
         "target(in_v=%u out_v=%u)\n",
         context,
         htnt_queue_count,
         ntht_queue_count,
         t_fire,
         to_host_fire,
         from_host_fire,
         hport_to_host_valid,
         hport_to_host_ready,
         hport_from_host_valid,
         hport_from_host_ready,
         target_in_valid,
         target_out_valid);
  printf("SIMPLENIC DEBUG REGS [%s] cycle ready=%u fire_count=%llu "
         "ready_count=%llu not_ready_count=%llu "
         "blocked(no_to_host=%u from_host_channels=%u "
         "to_host_payload_queue=%u reason=0x%02x) debug_cycle=%llu\n",
         context,
         target_cycle_ready,
         static_cast<unsigned long long>(target_cycle_fire_count64),
         static_cast<unsigned long long>(target_cycle_ready_count64),
         static_cast<unsigned long long>(target_cycle_not_ready_count64),
         target_blocked_no_to_host_token,
         target_blocked_from_host_channels,
         target_blocked_to_host_payload_queue,
         current_blocked_reason,
         static_cast<unsigned long long>(debug_cycle));
  printf("SIMPLENIC DEBUG REGS [%s] from_host token_available=%u "
         "payload_token=%u empty_token=%u empty_drop=%u "
         "current_valid=%u pending_valid=%u capture=%u "
         "capture_ready=%u payload_deq=%u empty_drop_fire=%u "
         "capture_count=%llu empty_drop_count=%llu "
         "target_ready=%u to_host_ready_drive=%u\n",
         context,
         from_host_token_available,
         from_host_payload_token_available,
         from_host_empty_token_available,
         from_host_empty_drop,
         from_host_current_payload_valid,
         from_host_pending_payload_valid,
         from_host_payload_capture,
         from_host_payload_capture_ready,
         from_host_payload_deq,
         from_host_empty_drop_fire,
         static_cast<unsigned long long>(from_host_payload_capture_count64),
         static_cast<unsigned long long>(from_host_empty_drop_count64),
         from_host_all_ready,
         to_host_ready_drive);
  printf("SIMPLENIC DEBUG REGS [%s] to_host payload_valid=%u empty_valid=%u "
         "empty_suppressed=%u payload_blocked=%u payload_fire_count=%llu "
         "empty_fire_count=%llu pcie_in_fire=%u pcie_out_fire=%u "
         "adapter pcie_in(v=%u r=%u) pcie_out(v=%u r=%u out_q=%u)\n",
         context,
         to_host_payload_valid,
         to_host_empty_valid,
         to_host_empty_suppressed,
         to_host_payload_blocked,
         static_cast<unsigned long long>(to_host_payload_fire_count64),
         static_cast<unsigned long long>(to_host_empty_fire_count64),
         pcie_in_fire_count,
         pcie_out_fire_count,
         bigtoken_pcie_in_valid,
         bigtoken_pcie_in_ready,
         nicbig_pcie_out_valid,
         nicbig_pcie_out_ready,
         nicbig_output_queue_count);
  printf("SIMPLENIC DEBUG REGS [%s] raw_queue "
         "htnt(enq_v=%u enq_r=%u deq_v=%u deq_r=%u) "
         "ntht(enq_v=%u enq_r=%u deq_v=%u deq_r=%u)\n",
         context,
         htnt_queue_enq_valid,
         htnt_queue_enq_ready,
         htnt_queue_deq_valid,
         htnt_queue_deq_ready,
         ntht_queue_enq_valid,
         ntht_queue_enq_ready,
         ntht_queue_deq_valid,
         ntht_queue_deq_ready);
  fflush(stdout);
#if 0
  auto read_u64 = [this](uint64_t lo_addr, uint64_t hi_addr) -> uint64_t {
    const uint64_t lo = read(lo_addr);
    const uint64_t hi = read(hi_addr);
    return (hi << 32) | lo;
  };

  const auto done = read(mmio_addrs.done);
  const auto minobs_build_marker = read(mmio_addrs.minobs_build_marker);
  const auto htnt_queue_count = read(mmio_addrs.htnt_queue_count);
  const auto ntht_queue_count = read(mmio_addrs.ntht_queue_count);
  const auto t_fire = read(mmio_addrs.t_fire);
  const auto to_host_fire = read(mmio_addrs.to_host_fire);
  const auto from_host_fire = read(mmio_addrs.from_host_fire);
  const auto t_fire_blocked_to_host_valid =
      read(mmio_addrs.t_fire_blocked_to_host_valid);
  const auto t_fire_blocked_from_host_ready =
      read(mmio_addrs.t_fire_blocked_from_host_ready);
  const auto t_fire_blocked_ntht_enq_ready =
      read(mmio_addrs.t_fire_blocked_ntht_enq_ready);
  const auto t_fire_blocked_htnt_deq_valid =
      read(mmio_addrs.t_fire_blocked_htnt_deq_valid);
  const auto hport_to_host_valid = read(mmio_addrs.hport_to_host_valid);
  const auto hport_to_host_ready = read(mmio_addrs.hport_to_host_ready);
  const auto hport_from_host_valid = read(mmio_addrs.hport_from_host_valid);
  const auto hport_from_host_ready = read(mmio_addrs.hport_from_host_ready);
  const auto target_in_valid = read(mmio_addrs.target_in_valid);
  const auto target_out_valid = read(mmio_addrs.target_out_valid);
  const auto chan_to_host_valid_mask =
      read(mmio_addrs.chan_to_host_valid_mask);
  const auto chan_to_host_ready_mask =
      read(mmio_addrs.chan_to_host_ready_mask);
  const auto chan_to_host_fire_mask =
      read(mmio_addrs.chan_to_host_fire_mask);
  const auto chan_to_host_blocked_mask =
      read(mmio_addrs.chan_to_host_blocked_mask);
  const auto chan_from_host_valid_mask =
      read(mmio_addrs.chan_from_host_valid_mask);
  const auto chan_from_host_ready_mask =
      read(mmio_addrs.chan_from_host_ready_mask);
  const auto chan_from_host_fire_mask =
      read(mmio_addrs.chan_from_host_fire_mask);
  const auto chan_from_host_blocked_mask =
      read(mmio_addrs.chan_from_host_blocked_mask);
  const auto chan_from_host_would_valid_mask =
      read(mmio_addrs.chan_from_host_would_valid_mask);
  const auto chan_from_host_would_blocked_mask =
      read(mmio_addrs.chan_from_host_would_blocked_mask);
  const auto from_host_leaf_valid_mask =
      read(mmio_addrs.from_host_leaf_valid_mask);
  const auto from_host_leaf_ready_mask =
      read(mmio_addrs.from_host_leaf_ready_mask);
  const auto from_host_leaf_fire_mask =
      read(mmio_addrs.from_host_leaf_fire_mask);
  const auto from_host_leaf_blocked_mask =
      read(mmio_addrs.from_host_leaf_blocked_mask);
  const auto bigtoken_htnt_valid = read(mmio_addrs.bigtoken_htnt_valid);
  const auto bigtoken_htnt_ready = read(mmio_addrs.bigtoken_htnt_ready);
  const auto nicbig_ntht_valid = read(mmio_addrs.nicbig_ntht_valid);
  const auto nicbig_ntht_ready = read(mmio_addrs.nicbig_ntht_ready);
  const auto bigtoken_pcie_in_valid = read(mmio_addrs.bigtoken_pcie_in_valid);
  const auto bigtoken_pcie_in_ready = read(mmio_addrs.bigtoken_pcie_in_ready);
  const auto nicbig_pcie_out_valid = read(mmio_addrs.nicbig_pcie_out_valid);
  const auto nicbig_pcie_out_ready = read(mmio_addrs.nicbig_pcie_out_ready);
  const auto bigtoken_loop_iter = read(mmio_addrs.bigtoken_loop_iter);
  const auto bigtoken_latched_valid =
      read(mmio_addrs.bigtoken_latched_valid);
  const auto bigtoken_accept_count =
      read(mmio_addrs.bigtoken_accept_count);
  const auto bigtoken_emit_count =
      read(mmio_addrs.bigtoken_emit_count);
  const auto bigtoken_payload_emit_count =
      read(mmio_addrs.bigtoken_payload_emit_count);
  const auto bigtoken_pcie_valid_no_ready_count =
      read(mmio_addrs.bigtoken_pcie_valid_no_ready_count);
  const auto bigtoken_pcie_ready_no_valid_count =
      read(mmio_addrs.bigtoken_pcie_ready_no_valid_count);
  const auto bigtoken_htnt_valid_no_ready_count =
      read(mmio_addrs.bigtoken_htnt_valid_no_ready_count);
  const auto bigtoken_htnt_ready_no_valid_count =
      read(mmio_addrs.bigtoken_htnt_ready_no_valid_count);
  const auto bigtoken_current_slot_meta =
      read(mmio_addrs.bigtoken_current_slot_meta);
  const auto bigtoken_current_slot_data =
      read_u64(mmio_addrs.bigtoken_current_slot_data_0,
               mmio_addrs.bigtoken_current_slot_data_1);
  const auto bigtoken_latched_valid_mask =
      read(mmio_addrs.bigtoken_latched_valid_mask);
  const auto bigtoken_latched_ready_mask =
      read(mmio_addrs.bigtoken_latched_ready_mask);
  const auto bigtoken_latched_last_mask =
      read(mmio_addrs.bigtoken_latched_last_mask);
  const auto bigtoken_last_emit_slot_mask =
      read(mmio_addrs.bigtoken_last_emit_slot_mask);
  const auto bigtoken_ever_emit_slot_mask =
      read(mmio_addrs.bigtoken_ever_emit_slot_mask);
  const auto bigtoken_ever_payload_emit_slot_mask =
      read(mmio_addrs.bigtoken_ever_payload_emit_slot_mask);
  const auto bigtoken_last_emit_data =
      read_u64(mmio_addrs.bigtoken_last_emit_data_0,
               mmio_addrs.bigtoken_last_emit_data_1);
  const auto bigtoken_last_emit_meta =
      read(mmio_addrs.bigtoken_last_emit_meta);
  const auto nicbig_special_counter = read(mmio_addrs.nicbig_special_counter);
  const auto nicbig_full_flush_count =
      read(mmio_addrs.nicbig_full_flush_count);
  const auto nicbig_partial_flush_count =
      read(mmio_addrs.nicbig_partial_flush_count);
  const auto nicbig_last_flush_slot =
      read(mmio_addrs.nicbig_last_flush_slot);
  const auto nicbig_pcie_out_enq_count =
      read(mmio_addrs.nicbig_pcie_out_enq_count);
  const auto nicbig_output_queue_count =
      read(mmio_addrs.nicbig_output_queue_count);
  const auto nicbig_ntht_fire_count =
      read(mmio_addrs.nicbig_ntht_fire_count);
  const auto nicbig_ntht_valid_no_ready_count =
      read(mmio_addrs.nicbig_ntht_valid_no_ready_count);
  const auto nicbig_ntht_ready_no_valid_count =
      read(mmio_addrs.nicbig_ntht_ready_no_valid_count);
  const auto nicbig_pcie_out_valid_no_ready_count =
      read(mmio_addrs.nicbig_pcie_out_valid_no_ready_count);
  const auto nicbig_pcie_out_ready_no_valid_count =
      read(mmio_addrs.nicbig_pcie_out_ready_no_valid_count);
  const auto nicbig_pcie_out_enq_valid_no_ready_count =
      read(mmio_addrs.nicbig_pcie_out_enq_valid_no_ready_count);
  const auto nicbig_flush_candidate =
      read(mmio_addrs.nicbig_flush_candidate);
  const auto nicbig_flush_blocked =
      read(mmio_addrs.nicbig_flush_blocked);
  const auto nicbig_last_ntht_data =
      read_u64(mmio_addrs.nicbig_last_ntht_data_0,
               mmio_addrs.nicbig_last_ntht_data_1);
  const auto nicbig_last_ntht_meta =
      read(mmio_addrs.nicbig_last_ntht_meta);
  const auto t_fire_count = read(mmio_addrs.t_fire_count);
  const auto to_host_fire_count = read(mmio_addrs.to_host_fire_count);
  const auto from_host_fire_count = read(mmio_addrs.from_host_fire_count);
  const auto htnt_queue_enq_fire_count =
      read(mmio_addrs.htnt_queue_enq_fire_count);
  const auto htnt_queue_deq_fire_count =
      read(mmio_addrs.htnt_queue_deq_fire_count);
  const auto ntht_queue_enq_fire_count =
      read(mmio_addrs.ntht_queue_enq_fire_count);
  const auto ntht_queue_deq_fire_count =
      read(mmio_addrs.ntht_queue_deq_fire_count);
  const auto pcie_in_fire_count = read(mmio_addrs.pcie_in_fire_count);
  const auto pcie_out_fire_count = read(mmio_addrs.pcie_out_fire_count);
  const auto current_blocked_reason =
      read(mmio_addrs.current_blocked_reason);
  const auto first_blocked_valid = read(mmio_addrs.first_blocked_valid);
  const auto first_blocked_reason = read(mmio_addrs.first_blocked_reason);
  const auto first_blocked_queue_snapshot =
      read(mmio_addrs.first_blocked_queue_snapshot);
  const auto first_blocked_adapter_snapshot =
      read(mmio_addrs.first_blocked_adapter_snapshot);
  const auto last_blocked_reason = read(mmio_addrs.last_blocked_reason);
  const auto debug_cycle =
      read_u64(mmio_addrs.debug_cycle_0, mmio_addrs.debug_cycle_1);
  const auto last_t_fire_cycle = read_u64(mmio_addrs.last_t_fire_cycle_0,
                                          mmio_addrs.last_t_fire_cycle_1);
  const auto last_to_host_fire_cycle =
      read_u64(mmio_addrs.last_to_host_fire_cycle_0,
               mmio_addrs.last_to_host_fire_cycle_1);
  const auto last_from_host_fire_cycle =
      read_u64(mmio_addrs.last_from_host_fire_cycle_0,
               mmio_addrs.last_from_host_fire_cycle_1);
  const auto last_pcie_in_fire_cycle =
      read_u64(mmio_addrs.last_pcie_in_fire_cycle_0,
               mmio_addrs.last_pcie_in_fire_cycle_1);
  const auto last_pcie_out_fire_cycle =
      read_u64(mmio_addrs.last_pcie_out_fire_cycle_0,
               mmio_addrs.last_pcie_out_fire_cycle_1);
  const auto first_blocked_cycle =
      read_u64(mmio_addrs.first_blocked_cycle_0,
               mmio_addrs.first_blocked_cycle_1);
  const auto last_blocked_cycle =
      read_u64(mmio_addrs.last_blocked_cycle_0,
               mmio_addrs.last_blocked_cycle_1);
  const auto local_queues_ready = read(mmio_addrs.local_queues_ready);
  const auto target_cycle_ready = read(mmio_addrs.target_cycle_ready);
  const auto to_host_ready_drive = read(mmio_addrs.to_host_ready_drive);
  const auto from_host_valid_drive = read(mmio_addrs.from_host_valid_drive);
  const auto from_host_all_ready = read(mmio_addrs.from_host_all_ready);
  const auto from_host_group_fire = read(mmio_addrs.from_host_group_fire);
  const auto from_host_group_blocked =
      read(mmio_addrs.from_host_group_blocked);
  const auto from_host_token_available =
      read(mmio_addrs.from_host_token_available);
  const auto from_host_payload_token_available =
      read(mmio_addrs.from_host_payload_token_available);
  const auto from_host_empty_token_available =
      read(mmio_addrs.from_host_empty_token_available);
  const auto from_host_empty_drop = read(mmio_addrs.from_host_empty_drop);
  const auto to_host_payload_valid = read(mmio_addrs.to_host_payload_valid);
  const auto to_host_empty_valid = read(mmio_addrs.to_host_empty_valid);
  const auto to_host_empty_bypass = read(mmio_addrs.to_host_empty_bypass);
  const auto to_host_empty_enqueue = read(mmio_addrs.to_host_empty_enqueue);
  const auto to_host_payload_blocked =
      read(mmio_addrs.to_host_payload_blocked);
  const auto from_host_current_payload_valid =
      read(mmio_addrs.from_host_current_payload_valid);
  const auto from_host_pending_payload_valid =
      read(mmio_addrs.from_host_pending_payload_valid);
  const auto from_host_payload_capture =
      read(mmio_addrs.from_host_payload_capture);
  const auto from_host_payload_capture_ready =
      read(mmio_addrs.from_host_payload_capture_ready);
  const auto from_host_payload_deq =
      read(mmio_addrs.from_host_payload_deq);
  const auto from_host_empty_drop_fire =
      read(mmio_addrs.from_host_empty_drop_fire);
  const auto from_host_current_payload_data =
      read_u64(mmio_addrs.from_host_current_payload_data_lo,
               mmio_addrs.from_host_current_payload_data_hi);
  const auto from_host_current_payload_keep =
      read(mmio_addrs.from_host_current_payload_keep);
  const auto from_host_current_payload_last =
      read(mmio_addrs.from_host_current_payload_last);
  const auto from_host_pending_payload_data =
      read_u64(mmio_addrs.from_host_pending_payload_data_lo,
               mmio_addrs.from_host_pending_payload_data_hi);
  const auto from_host_pending_payload_keep =
      read(mmio_addrs.from_host_pending_payload_keep);
  const auto from_host_pending_payload_last =
      read(mmio_addrs.from_host_pending_payload_last);
  const auto from_host_payload_capture_count64 =
      read_u64(mmio_addrs.from_host_payload_capture_count64_0,
               mmio_addrs.from_host_payload_capture_count64_1);
  const auto last_from_host_payload_capture_cycle =
      read_u64(mmio_addrs.last_from_host_payload_capture_cycle_0,
               mmio_addrs.last_from_host_payload_capture_cycle_1);
  const auto last_from_host_payload_capture_data =
      read_u64(mmio_addrs.last_from_host_payload_capture_data_0,
               mmio_addrs.last_from_host_payload_capture_data_1);
  const auto last_from_host_payload_capture_meta =
      read(mmio_addrs.last_from_host_payload_capture_meta);
  const auto htnt_queue_enq_ready = read(mmio_addrs.htnt_queue_enq_ready);
  const auto htnt_queue_enq_valid = read(mmio_addrs.htnt_queue_enq_valid);
  const auto htnt_queue_deq_valid = read(mmio_addrs.htnt_queue_deq_valid);
  const auto htnt_queue_deq_ready = read(mmio_addrs.htnt_queue_deq_ready);
  const auto ntht_queue_enq_ready = read(mmio_addrs.ntht_queue_enq_ready);
  const auto ntht_queue_enq_valid = read(mmio_addrs.ntht_queue_enq_valid);
  const auto ntht_queue_deq_valid = read(mmio_addrs.ntht_queue_deq_valid);
  const auto ntht_queue_deq_ready = read(mmio_addrs.ntht_queue_deq_ready);
  const auto target_in_bits_data_lo =
      read(mmio_addrs.target_in_bits_data_lo);
  const auto target_in_bits_data_hi =
      read(mmio_addrs.target_in_bits_data_hi);
  const auto target_in_bits_keep = read(mmio_addrs.target_in_bits_keep);
  const auto target_in_bits_last = read(mmio_addrs.target_in_bits_last);
  const auto target_out_bits_data_lo =
      read(mmio_addrs.target_out_bits_data_lo);
  const auto target_out_bits_data_hi =
      read(mmio_addrs.target_out_bits_data_hi);
  const auto target_out_bits_keep = read(mmio_addrs.target_out_bits_keep);
  const auto target_out_bits_last = read(mmio_addrs.target_out_bits_last);
  const auto htnt_deq_data_lo = read(mmio_addrs.htnt_deq_data_lo);
  const auto htnt_deq_data_hi = read(mmio_addrs.htnt_deq_data_hi);
  const auto htnt_deq_keep = read(mmio_addrs.htnt_deq_keep);
  const auto htnt_deq_last = read(mmio_addrs.htnt_deq_last);
  const auto htnt_deq_data_in_valid =
      read(mmio_addrs.htnt_deq_data_in_valid);
  const auto htnt_deq_data_out_ready =
      read(mmio_addrs.htnt_deq_data_out_ready);
  const auto ntht_enq_data_lo = read(mmio_addrs.ntht_enq_data_lo);
  const auto ntht_enq_data_hi = read(mmio_addrs.ntht_enq_data_hi);
  const auto ntht_enq_keep = read(mmio_addrs.ntht_enq_keep);
  const auto ntht_enq_last = read(mmio_addrs.ntht_enq_last);
  const auto ntht_enq_data_out_valid =
      read(mmio_addrs.ntht_enq_data_out_valid);
  const auto ntht_enq_data_in_ready =
      read(mmio_addrs.ntht_enq_data_in_ready);
  const auto target_cycle_fire_count64 =
      read_u64(mmio_addrs.target_cycle_fire_count64_0,
               mmio_addrs.target_cycle_fire_count64_1);
  const auto target_blocked_no_to_host_token_count64 =
      read_u64(mmio_addrs.target_blocked_no_to_host_token_count64_0,
               mmio_addrs.target_blocked_no_to_host_token_count64_1);
  const auto target_blocked_from_host_channels_count64 =
      read_u64(mmio_addrs.target_blocked_from_host_channels_count64_0,
               mmio_addrs.target_blocked_from_host_channels_count64_1);
  const auto target_blocked_to_host_payload_queue_count64 =
      read_u64(mmio_addrs.target_blocked_to_host_payload_queue_count64_0,
               mmio_addrs.target_blocked_to_host_payload_queue_count64_1);
  const auto target_cycle_ready_count64 =
      read_u64(mmio_addrs.target_cycle_ready_count64_0,
               mmio_addrs.target_cycle_ready_count64_1);
  const auto target_cycle_not_ready_count64 =
      read_u64(mmio_addrs.target_cycle_not_ready_count64_0,
               mmio_addrs.target_cycle_not_ready_count64_1);
  const auto first_target_cycle_ready_valid =
      read(mmio_addrs.first_target_cycle_ready_valid);
  const auto first_target_cycle_ready_cycle =
      read_u64(mmio_addrs.first_target_cycle_ready_cycle_0,
               mmio_addrs.first_target_cycle_ready_cycle_1);
  const auto first_target_cycle_fire_valid =
      read(mmio_addrs.first_target_cycle_fire_valid);
  const auto first_target_cycle_fire_cycle =
      read_u64(mmio_addrs.first_target_cycle_fire_cycle_0,
               mmio_addrs.first_target_cycle_fire_cycle_1);
  const auto target_cycle_ready_streak_count64 =
      read_u64(mmio_addrs.target_cycle_ready_streak_count64_0,
               mmio_addrs.target_cycle_ready_streak_count64_1);
  const auto target_cycle_ready_max_streak_count64 =
      read_u64(mmio_addrs.target_cycle_ready_max_streak_count64_0,
               mmio_addrs.target_cycle_ready_max_streak_count64_1);
  const auto target_cycle_not_ready_streak_count64 =
      read_u64(mmio_addrs.target_cycle_not_ready_streak_count64_0,
               mmio_addrs.target_cycle_not_ready_streak_count64_1);
  const auto target_cycle_not_ready_max_streak_count64 =
      read_u64(mmio_addrs.target_cycle_not_ready_max_streak_count64_0,
               mmio_addrs.target_cycle_not_ready_max_streak_count64_1);
  const auto to_host_ready_drive_count64 =
      read_u64(mmio_addrs.to_host_ready_drive_count64_0,
               mmio_addrs.to_host_ready_drive_count64_1);
  const auto from_host_ready_count64 =
      read_u64(mmio_addrs.from_host_ready_count64_0,
               mmio_addrs.from_host_ready_count64_1);
  const auto target_blocked_no_to_host_token =
      read(mmio_addrs.target_blocked_no_to_host_token);
  const auto target_blocked_no_from_host_token_count64 =
      read_u64(mmio_addrs.target_blocked_no_from_host_token_count64_0,
               mmio_addrs.target_blocked_no_from_host_token_count64_1);
  const auto target_blocked_from_host_channels =
      read(mmio_addrs.target_blocked_from_host_channels);
  const auto target_blocked_to_host_payload_queue =
      read(mmio_addrs.target_blocked_to_host_payload_queue);
  const auto target_blocked_no_from_host_token =
      read(mmio_addrs.target_blocked_no_from_host_token);
  const auto to_host_empty_suppressed =
      read(mmio_addrs.to_host_empty_suppressed);
  const auto to_host_payload_fire_count64 =
      read_u64(mmio_addrs.to_host_payload_fire_count64_0,
               mmio_addrs.to_host_payload_fire_count64_1);
  const auto to_host_empty_fire_count64 =
      read_u64(mmio_addrs.to_host_empty_fire_count64_0,
               mmio_addrs.to_host_empty_fire_count64_1);
  const auto to_host_empty_enqueue_count64 =
      read_u64(mmio_addrs.to_host_empty_enqueue_count64_0,
               mmio_addrs.to_host_empty_enqueue_count64_1);
  const auto to_host_empty_suppressed_count64 =
      read_u64(mmio_addrs.to_host_empty_suppressed_count64_0,
               mmio_addrs.to_host_empty_suppressed_count64_1);
  const auto from_host_empty_drop_count64 =
      read_u64(mmio_addrs.from_host_empty_drop_count64_0,
               mmio_addrs.from_host_empty_drop_count64_1);
  const auto pcie_in_backpressure_count64 =
      read_u64(mmio_addrs.pcie_in_backpressure_count64_0,
               mmio_addrs.pcie_in_backpressure_count64_1);
  const auto pcie_out_backpressure_count64 =
      read_u64(mmio_addrs.pcie_out_backpressure_count64_0,
               mmio_addrs.pcie_out_backpressure_count64_1);
  const auto to_host_missing_valid_count64 =
      read_u64(mmio_addrs.to_host_missing_valid_count64_0,
               mmio_addrs.to_host_missing_valid_count64_1);
  const auto from_host_group_blocked_count64 =
      read_u64(mmio_addrs.from_host_group_blocked_count64_0,
               mmio_addrs.from_host_group_blocked_count64_1);
  const auto from_host_would_blocked_count64 =
      read_u64(mmio_addrs.from_host_would_blocked_count64_0,
               mmio_addrs.from_host_would_blocked_count64_1);
  const auto from_host_leaf_blocked_count64 =
      read_u64(mmio_addrs.from_host_leaf_blocked_count64_0,
               mmio_addrs.from_host_leaf_blocked_count64_1);
  const auto nic_in_blocked_count64 =
      read_u64(mmio_addrs.nic_in_blocked_count64_0,
               mmio_addrs.nic_in_blocked_count64_1);
  const auto macaddr_blocked_count64 =
      read_u64(mmio_addrs.macaddr_blocked_count64_0,
               mmio_addrs.macaddr_blocked_count64_1);
  const auto rlimit_blocked_count64 =
      read_u64(mmio_addrs.rlimit_blocked_count64_0,
               mmio_addrs.rlimit_blocked_count64_1);
  const auto pauser_blocked_count64 =
      read_u64(mmio_addrs.pauser_blocked_count64_0,
               mmio_addrs.pauser_blocked_count64_1);
  const auto nic_in_fire_count64 =
      read_u64(mmio_addrs.nic_in_fire_count64_0,
               mmio_addrs.nic_in_fire_count64_1);
  const auto macaddr_fire_count64 =
      read_u64(mmio_addrs.macaddr_fire_count64_0,
               mmio_addrs.macaddr_fire_count64_1);
  const auto rlimit_fire_count64 =
      read_u64(mmio_addrs.rlimit_fire_count64_0,
               mmio_addrs.rlimit_fire_count64_1);
  const auto pauser_fire_count64 =
      read_u64(mmio_addrs.pauser_fire_count64_0,
               mmio_addrs.pauser_fire_count64_1);
  const auto first_progress_valid =
      read(mmio_addrs.first_progress_valid);
  const auto first_progress_event_mask =
      read(mmio_addrs.first_progress_event_mask);
  const auto first_progress_cycle =
      read_u64(mmio_addrs.first_progress_cycle_0,
               mmio_addrs.first_progress_cycle_1);
  const auto first_progress_queue_snapshot =
      read(mmio_addrs.first_progress_queue_snapshot);
  const auto first_progress_adapter_snapshot =
      read(mmio_addrs.first_progress_adapter_snapshot);
  const auto progress_seen_mask = read(mmio_addrs.progress_seen_mask);
  const auto last_progress_event_mask =
      read(mmio_addrs.last_progress_event_mask);
  const auto last_progress_cycle =
      read_u64(mmio_addrs.last_progress_cycle_0,
               mmio_addrs.last_progress_cycle_1);
  const auto last_progress_queue_snapshot =
      read(mmio_addrs.last_progress_queue_snapshot);
  const auto last_progress_adapter_snapshot =
      read(mmio_addrs.last_progress_adapter_snapshot);
  const auto blocked_streak_count64 =
      read_u64(mmio_addrs.blocked_streak_count64_0,
               mmio_addrs.blocked_streak_count64_1);
  const auto max_blocked_streak_count64 =
      read_u64(mmio_addrs.max_blocked_streak_count64_0,
               mmio_addrs.max_blocked_streak_count64_1);
  const auto from_host_leaf_blocked_streak_count64 =
      read_u64(mmio_addrs.from_host_leaf_blocked_streak_count64_0,
               mmio_addrs.from_host_leaf_blocked_streak_count64_1);
  const auto from_host_leaf_blocked_max_streak_count64 =
      read_u64(mmio_addrs.from_host_leaf_blocked_max_streak_count64_0,
               mmio_addrs.from_host_leaf_blocked_max_streak_count64_1);
  const auto to_host_payload_blocked_streak_count64 =
      read_u64(mmio_addrs.to_host_payload_blocked_streak_count64_0,
               mmio_addrs.to_host_payload_blocked_streak_count64_1);
  const auto to_host_payload_blocked_max_streak_count64 =
      read_u64(mmio_addrs.to_host_payload_blocked_max_streak_count64_0,
               mmio_addrs.to_host_payload_blocked_max_streak_count64_1);
  const auto to_host_ever_valid_mask =
      read(mmio_addrs.to_host_ever_valid_mask);
  const auto to_host_ever_ready_mask =
      read(mmio_addrs.to_host_ever_ready_mask);
  const auto to_host_ever_fire_mask = read(mmio_addrs.to_host_ever_fire_mask);
  const auto to_host_ever_blocked_mask =
      read(mmio_addrs.to_host_ever_blocked_mask);
  const auto from_host_ever_valid_mask =
      read(mmio_addrs.from_host_ever_valid_mask);
  const auto from_host_ever_ready_mask =
      read(mmio_addrs.from_host_ever_ready_mask);
  const auto from_host_ever_fire_mask =
      read(mmio_addrs.from_host_ever_fire_mask);
  const auto from_host_ever_blocked_mask =
      read(mmio_addrs.from_host_ever_blocked_mask);
  const auto last_blocked_to_host_valid_mask =
      read(mmio_addrs.last_blocked_to_host_valid_mask);
  const auto last_blocked_to_host_ready_mask =
      read(mmio_addrs.last_blocked_to_host_ready_mask);
  const auto last_blocked_to_host_fire_mask =
      read(mmio_addrs.last_blocked_to_host_fire_mask);
  const auto last_blocked_to_host_blocked_mask =
      read(mmio_addrs.last_blocked_to_host_blocked_mask);
  const auto last_blocked_from_host_valid_mask =
      read(mmio_addrs.last_blocked_from_host_valid_mask);
  const auto last_blocked_from_host_ready_mask =
      read(mmio_addrs.last_blocked_from_host_ready_mask);
  const auto last_blocked_from_host_fire_mask =
      read(mmio_addrs.last_blocked_from_host_fire_mask);
  const auto last_blocked_from_host_blocked_mask =
      read(mmio_addrs.last_blocked_from_host_blocked_mask);
  const auto last_blocked_queue_snapshot =
      read(mmio_addrs.last_blocked_queue_snapshot);
  const auto last_blocked_adapter_snapshot =
      read(mmio_addrs.last_blocked_adapter_snapshot);
  const auto first_from_host_blocked_valid =
      read(mmio_addrs.first_from_host_blocked_valid);
  const auto first_from_host_blocked_cycle =
      read_u64(mmio_addrs.first_from_host_blocked_cycle_0,
               mmio_addrs.first_from_host_blocked_cycle_1);
  const auto first_from_host_blocked_ready_mask =
      read(mmio_addrs.first_from_host_blocked_ready_mask);
  const auto first_from_host_blocked_valid_mask =
      read(mmio_addrs.first_from_host_blocked_valid_mask);
  const auto last_from_host_blocked_cycle =
      read_u64(mmio_addrs.last_from_host_blocked_cycle_0,
               mmio_addrs.last_from_host_blocked_cycle_1);
  const auto last_from_host_blocked_ready_mask =
      read(mmio_addrs.last_from_host_blocked_ready_mask);
  const auto last_from_host_blocked_valid_mask =
      read(mmio_addrs.last_from_host_blocked_valid_mask);
  const auto first_to_host_blocked_valid =
      read(mmio_addrs.first_to_host_blocked_valid);
  const auto first_to_host_blocked_cycle =
      read_u64(mmio_addrs.first_to_host_blocked_cycle_0,
               mmio_addrs.first_to_host_blocked_cycle_1);
  const auto first_to_host_blocked_ready_mask =
      read(mmio_addrs.first_to_host_blocked_ready_mask);
  const auto first_to_host_blocked_valid_mask =
      read(mmio_addrs.first_to_host_blocked_valid_mask);
  const auto last_to_host_blocked_cycle =
      read_u64(mmio_addrs.last_to_host_blocked_cycle_0,
               mmio_addrs.last_to_host_blocked_cycle_1);
  const auto last_to_host_blocked_ready_mask =
      read(mmio_addrs.last_to_host_blocked_ready_mask);
  const auto last_to_host_blocked_valid_mask =
      read(mmio_addrs.last_to_host_blocked_valid_mask);
  const auto first_from_host_leaf_blocked_valid =
      read(mmio_addrs.first_from_host_leaf_blocked_valid);
  const auto first_from_host_leaf_blocked_cycle =
      read_u64(mmio_addrs.first_from_host_leaf_blocked_cycle_0,
               mmio_addrs.first_from_host_leaf_blocked_cycle_1);
  const auto first_from_host_leaf_blocked_mask =
      read(mmio_addrs.first_from_host_leaf_blocked_mask);
  const auto last_from_host_leaf_blocked_cycle =
      read_u64(mmio_addrs.last_from_host_leaf_blocked_cycle_0,
               mmio_addrs.last_from_host_leaf_blocked_cycle_1);
  const auto last_from_host_leaf_blocked_mask =
      read(mmio_addrs.last_from_host_leaf_blocked_mask);
  const auto htnt_payload_enq_count64 =
      read_u64(mmio_addrs.htnt_payload_enq_count64_0,
               mmio_addrs.htnt_payload_enq_count64_1);
  const auto htnt_empty_enq_count64 =
      read_u64(mmio_addrs.htnt_empty_enq_count64_0,
               mmio_addrs.htnt_empty_enq_count64_1);
  const auto target_in_payload_fire_count64 =
      read_u64(mmio_addrs.target_in_payload_fire_count64_0,
               mmio_addrs.target_in_payload_fire_count64_1);
  const auto target_in_empty_fire_count64 =
      read_u64(mmio_addrs.target_in_empty_fire_count64_0,
               mmio_addrs.target_in_empty_fire_count64_1);
  const auto ntht_payload_enq_count64 =
      read_u64(mmio_addrs.ntht_payload_enq_count64_0,
               mmio_addrs.ntht_payload_enq_count64_1);
  const auto ntht_payload_deq_count64 =
      read_u64(mmio_addrs.ntht_payload_deq_count64_0,
               mmio_addrs.ntht_payload_deq_count64_1);
  const auto last_htnt_enq_cycle =
      read_u64(mmio_addrs.last_htnt_enq_cycle_0,
               mmio_addrs.last_htnt_enq_cycle_1);
  const auto last_htnt_enq_data =
      read_u64(mmio_addrs.last_htnt_enq_data_0,
               mmio_addrs.last_htnt_enq_data_1);
  const auto last_htnt_enq_meta = read(mmio_addrs.last_htnt_enq_meta);
  const auto last_htnt_deq_cycle =
      read_u64(mmio_addrs.last_htnt_deq_cycle_0,
               mmio_addrs.last_htnt_deq_cycle_1);
  const auto last_htnt_deq_data =
      read_u64(mmio_addrs.last_htnt_deq_data_0,
               mmio_addrs.last_htnt_deq_data_1);
  const auto last_htnt_deq_meta = read(mmio_addrs.last_htnt_deq_meta);
  const auto last_target_in_fire_cycle =
      read_u64(mmio_addrs.last_target_in_fire_cycle_0,
               mmio_addrs.last_target_in_fire_cycle_1);
  const auto last_target_in_fire_data =
      read_u64(mmio_addrs.last_target_in_fire_data_0,
               mmio_addrs.last_target_in_fire_data_1);
  const auto last_target_in_fire_meta =
      read(mmio_addrs.last_target_in_fire_meta);
  const auto last_target_out_fire_cycle =
      read_u64(mmio_addrs.last_target_out_fire_cycle_0,
               mmio_addrs.last_target_out_fire_cycle_1);
  const auto last_target_out_fire_data =
      read_u64(mmio_addrs.last_target_out_fire_data_0,
               mmio_addrs.last_target_out_fire_data_1);
  const auto last_target_out_fire_meta =
      read(mmio_addrs.last_target_out_fire_meta);
  const auto last_ntht_enq_cycle =
      read_u64(mmio_addrs.last_ntht_enq_cycle_0,
               mmio_addrs.last_ntht_enq_cycle_1);
  const auto last_ntht_enq_data =
      read_u64(mmio_addrs.last_ntht_enq_data_0,
               mmio_addrs.last_ntht_enq_data_1);
  const auto last_ntht_enq_meta = read(mmio_addrs.last_ntht_enq_meta);
  const auto last_ntht_deq_cycle =
      read_u64(mmio_addrs.last_ntht_deq_cycle_0,
               mmio_addrs.last_ntht_deq_cycle_1);
  const auto last_ntht_deq_data =
      read_u64(mmio_addrs.last_ntht_deq_data_0,
               mmio_addrs.last_ntht_deq_data_1);
  const auto last_ntht_deq_meta = read(mmio_addrs.last_ntht_deq_meta);
  const auto first_htnt_payload_enq_cycle =
      read_u64(mmio_addrs.first_htnt_payload_enq_cycle_0,
               mmio_addrs.first_htnt_payload_enq_cycle_1);
  const auto first_target_in_payload_fire_cycle =
      read_u64(mmio_addrs.first_target_in_payload_fire_cycle_0,
               mmio_addrs.first_target_in_payload_fire_cycle_1);
  const auto first_target_out_payload_fire_cycle =
      read_u64(mmio_addrs.first_target_out_payload_fire_cycle_0,
               mmio_addrs.first_target_out_payload_fire_cycle_1);
  const auto first_ntht_payload_enq_cycle =
      read_u64(mmio_addrs.first_ntht_payload_enq_cycle_0,
               mmio_addrs.first_ntht_payload_enq_cycle_1);
  const auto first_pcie_out_fire_cycle =
      read_u64(mmio_addrs.first_pcie_out_fire_cycle_0,
               mmio_addrs.first_pcie_out_fire_cycle_1);
  const auto write_poll = static_cast<unsigned>(
      static_cast<uint8_t>(pcis_write_bufs[currentround][BUFBYTES]));
  const auto read_poll = static_cast<unsigned>(
      static_cast<uint8_t>(pcis_read_bufs[currentround][BUFBYTES]));

  printf("SIMPLENIC DEBUG [%s] requested_bytes=%u actual_bytes=%u currentround=%d "
         "linklatency=%d stream_to_cpu_idx=%d stream_from_cpu_idx=%d "
         "bufbytes=%d loopback=%d done=%u minobs_marker=0x%08x "
         "write_poll=%u read_poll=%u "
         "successful_rounds=%llu round_debug_limit=%u relaxed_required_bytes=%d "
         "round_pending=%d pending_token_bytes=%u pending_push_offset_bytes=%u "
         "pending_peer_snapshot=%d pending_peer_valid_flits=%u "
         "peer_wait_ticks=%llu push_blocked_ticks=%llu pull_empty_ticks=%llu\n",
         context,
         requested_bytes,
         actual_bytes,
         currentround,
         LINKLATENCY,
         stream_to_cpu_idx,
         stream_from_cpu_idx,
         BUFBYTES,
         loopback ? 1 : 0,
         done,
         minobs_build_marker,
         write_poll,
         read_poll,
         static_cast<unsigned long long>(successful_rounds),
         round_debug_limit,
         relaxed_required_bytes ? 1 : 0,
         round_pending ? 1 : 0,
         pending_token_bytes,
         pending_push_offset_bytes,
         pending_peer_snapshot_valid ? 1 : 0,
         pending_peer_valid_flits,
         static_cast<unsigned long long>(peer_wait_ticks),
         static_cast<unsigned long long>(push_blocked_ticks),
         static_cast<unsigned long long>(pull_empty_ticks));
  printf("SIMPLENIC DEBUG [%s] round_counters pulled_rounds_started=%llu "
         "empty_rounds_started=%llu empty_rounds_skipped_switch=%llu "
         "peer_input_rounds=%llu peer_empty_rounds=%llu "
         "pending_round_from_empty_pull=%d "
         "last_peer_valid_flits=%u last_peer_input_round=%llu\n",
         context,
         static_cast<unsigned long long>(pulled_rounds_started),
         static_cast<unsigned long long>(empty_rounds_started),
         static_cast<unsigned long long>(empty_rounds_skipped_switch),
         static_cast<unsigned long long>(peer_input_rounds),
         static_cast<unsigned long long>(peer_empty_rounds),
         pending_round_from_empty_pull ? 1 : 0,
         last_peer_valid_flits,
         static_cast<unsigned long long>(last_peer_input_round));
  printf("SIMPLENIC DEBUG REGS [%s] queues htnt=%u ntht=%u t_fire=%u "
         "to_host_fire=%u from_host_fire=%u "
         "t_fire_blocked={to_host_valid:%u from_host_ready:%u ntht_enq_ready:%u htnt_deq_valid:%u}\n",
         context,
         htnt_queue_count,
         ntht_queue_count,
         t_fire,
         to_host_fire,
         from_host_fire,
         t_fire_blocked_to_host_valid,
         t_fire_blocked_from_host_ready,
         t_fire_blocked_ntht_enq_ready,
         t_fire_blocked_htnt_deq_valid);
  printf("SIMPLENIC DEBUG REGS [%s] hostport to_host(v=%u r=%u) "
         "from_host(v=%u r=%u) target(in_v=%u out_v=%u)\n",
         context,
         hport_to_host_valid,
         hport_to_host_ready,
         hport_from_host_valid,
         hport_from_host_ready,
         target_in_valid,
         target_out_valid);
  printf("SIMPLENIC DEBUG REGS [%s] chan_masks to_host valid=0x%x "
         "ready=0x%x fire=0x%x blocked=0x%x\n",
         context,
         chan_to_host_valid_mask,
         chan_to_host_ready_mask,
         chan_to_host_fire_mask,
         chan_to_host_blocked_mask);
  printf("SIMPLENIC DEBUG REGS [%s] chan_masks from_host valid=0x%x "
         "ready=0x%x fire=0x%x blocked=0x%x\n",
         context,
         chan_from_host_valid_mask,
         chan_from_host_ready_mask,
         chan_from_host_fire_mask,
         chan_from_host_blocked_mask);
  printf("SIMPLENIC DEBUG REGS [%s] from_host_extra would_valid=0x%x "
         "would_blocked=0x%x leaf(valid=0x%x ready=0x%x fire=0x%x "
         "blocked=0x%x) leaf_order={pauser,rlimit,macaddr,nicIn}\n",
         context,
         chan_from_host_would_valid_mask,
         chan_from_host_would_blocked_mask,
         from_host_leaf_valid_mask,
         from_host_leaf_ready_mask,
         from_host_leaf_fire_mask,
         from_host_leaf_blocked_mask);
  printf("SIMPLENIC DEBUG REGS [%s] adapters htnt(v=%u r=%u) ntht(v=%u r=%u) "
         "pcie_in(v=%u r=%u) pcie_out(v=%u r=%u) loop_iter=%u "
         "special_counter=%u full_flush=%u partial_flush=%u "
         "last_flush_slot=%u ntht_fire=%u pcie_out_enq=%u output_q_count=%u "
         "flush_candidate=%u flush_blocked=%u\n",
         context,
         bigtoken_htnt_valid,
         bigtoken_htnt_ready,
         nicbig_ntht_valid,
         nicbig_ntht_ready,
         bigtoken_pcie_in_valid,
         bigtoken_pcie_in_ready,
         nicbig_pcie_out_valid,
         nicbig_pcie_out_ready,
         bigtoken_loop_iter,
         nicbig_special_counter,
         nicbig_full_flush_count,
         nicbig_partial_flush_count,
         nicbig_last_flush_slot,
         nicbig_ntht_fire_count,
         nicbig_pcie_out_enq_count,
         nicbig_output_queue_count,
         nicbig_flush_candidate,
         nicbig_flush_blocked);
  printf("SIMPLENIC DEBUG REGS [%s] bigtoken_latch valid=%u accept=%u emit=%u "
         "payload_emit=%u pcie_valid_no_ready=%u pcie_ready_no_valid=%u "
         "htnt_valid_no_ready=%u htnt_ready_no_valid=%u "
         "latched_masks(valid=0x%x ready=0x%x last=0x%x) "
         "emit_masks(last=0x%x ever=0x%x ever_payload=0x%x) "
         "slot=%u slot_meta(last=%u valid=%u ready=%u htnt_v=%u htnt_r=%u "
         "pcie_v=%u pcie_r=%u latched=%u) current_data=0x%016llx "
         "last_emit(data=0x%016llx meta=0x%08x)\n",
         context,
         bigtoken_latched_valid,
         bigtoken_accept_count,
         bigtoken_emit_count,
         bigtoken_payload_emit_count,
         bigtoken_pcie_valid_no_ready_count,
         bigtoken_pcie_ready_no_valid_count,
         bigtoken_htnt_valid_no_ready_count,
         bigtoken_htnt_ready_no_valid_count,
         bigtoken_latched_valid_mask,
         bigtoken_latched_ready_mask,
         bigtoken_latched_last_mask,
         bigtoken_last_emit_slot_mask,
         bigtoken_ever_emit_slot_mask,
         bigtoken_ever_payload_emit_slot_mask,
         bigtoken_current_slot_meta & 0x1fu,
         (bigtoken_current_slot_meta >> 8) & 1u,
         (bigtoken_current_slot_meta >> 9) & 1u,
         (bigtoken_current_slot_meta >> 10) & 1u,
         (bigtoken_current_slot_meta >> 11) & 1u,
         (bigtoken_current_slot_meta >> 12) & 1u,
         (bigtoken_current_slot_meta >> 13) & 1u,
         (bigtoken_current_slot_meta >> 14) & 1u,
         (bigtoken_current_slot_meta >> 15) & 1u,
         static_cast<unsigned long long>(bigtoken_current_slot_data),
         static_cast<unsigned long long>(bigtoken_last_emit_data),
         bigtoken_last_emit_meta);
  printf("SIMPLENIC DEBUG REGS [%s] nicbig_handshake "
         "ntht_valid_no_ready=%u ntht_ready_no_valid=%u "
         "pcie_out_valid_no_ready=%u pcie_out_ready_no_valid=%u "
         "pcie_out_enq_valid_no_ready=%u flush_candidate=%u flush_blocked=%u "
         "last_ntht(data=0x%016llx meta=0x%08x keep=0x%02x last=%u "
         "out_valid=%u in_ready=%u)\n",
         context,
         nicbig_ntht_valid_no_ready_count,
         nicbig_ntht_ready_no_valid_count,
         nicbig_pcie_out_valid_no_ready_count,
         nicbig_pcie_out_ready_no_valid_count,
         nicbig_pcie_out_enq_valid_no_ready_count,
         nicbig_flush_candidate,
         nicbig_flush_blocked,
         static_cast<unsigned long long>(nicbig_last_ntht_data),
         nicbig_last_ntht_meta,
         nicbig_last_ntht_meta & 0xffu,
         (nicbig_last_ntht_meta >> 8) & 1u,
         (nicbig_last_ntht_meta >> 9) & 1u,
         (nicbig_last_ntht_meta >> 10) & 1u);
  printf("SIMPLENIC DEBUG REGS [%s] counts t_fire=%u to_host_fire=%u "
         "from_host_fire=%u htnt_enq=%u htnt_deq=%u "
         "ntht_enq=%u ntht_deq=%u pcie_in=%u pcie_out=%u\n",
         context,
         t_fire_count,
         to_host_fire_count,
         from_host_fire_count,
         htnt_queue_enq_fire_count,
         htnt_queue_deq_fire_count,
         ntht_queue_enq_fire_count,
         ntht_queue_deq_fire_count,
         pcie_in_fire_count,
         pcie_out_fire_count);
  printf("SIMPLENIC DEBUG REGS [%s] target_cycle_counts fire=%llu "
         "ready=%llu not_ready=%llu to_host_ready_drive=%llu "
         "from_host_ready=%llu "
         "blocked_no_to_host=%llu blocked_no_from_host=%llu "
         "blocked_from_host_channels=%llu blocked_to_host_payload_queue=%llu "
         "live(no_to_host=%u no_from_host=%u from_host_channels=%u "
         "to_host_payload_queue=%u) first_ready(valid=%u cycle=%llu) "
         "first_fire(valid=%u cycle=%llu) ready_streak=%llu "
         "ready_max=%llu not_ready_streak=%llu not_ready_max=%llu\n",
         context,
         static_cast<unsigned long long>(target_cycle_fire_count64),
         static_cast<unsigned long long>(target_cycle_ready_count64),
         static_cast<unsigned long long>(target_cycle_not_ready_count64),
         static_cast<unsigned long long>(to_host_ready_drive_count64),
         static_cast<unsigned long long>(from_host_ready_count64),
         static_cast<unsigned long long>(target_blocked_no_to_host_token_count64),
         static_cast<unsigned long long>(target_blocked_no_from_host_token_count64),
         static_cast<unsigned long long>(target_blocked_from_host_channels_count64),
         static_cast<unsigned long long>(target_blocked_to_host_payload_queue_count64),
         target_blocked_no_to_host_token,
         target_blocked_no_from_host_token,
         target_blocked_from_host_channels,
         target_blocked_to_host_payload_queue,
         first_target_cycle_ready_valid,
         static_cast<unsigned long long>(first_target_cycle_ready_cycle),
         first_target_cycle_fire_valid,
         static_cast<unsigned long long>(first_target_cycle_fire_cycle),
         static_cast<unsigned long long>(target_cycle_ready_streak_count64),
         static_cast<unsigned long long>(target_cycle_ready_max_streak_count64),
         static_cast<unsigned long long>(target_cycle_not_ready_streak_count64),
         static_cast<unsigned long long>(target_cycle_not_ready_max_streak_count64));
  printf("SIMPLENIC DEBUG REGS [%s] io_counts to_payload_fire=%llu "
         "to_empty_fire=%llu to_empty_enqueue=%llu "
         "to_empty_suppressed=%llu from_empty_drop=%llu "
         "pcie_backpressure(in=%llu out=%llu) to_missing_valid=%llu "
         "from_group_blocked=%llu from_would_blocked=%llu "
         "from_leaf_blocked=%llu leaf_blocked(nic=%llu mac=%llu "
         "rlimit=%llu pauser=%llu) leaf_fire(nic=%llu mac=%llu "
         "rlimit=%llu pauser=%llu)\n",
         context,
         static_cast<unsigned long long>(to_host_payload_fire_count64),
         static_cast<unsigned long long>(to_host_empty_fire_count64),
         static_cast<unsigned long long>(to_host_empty_enqueue_count64),
         static_cast<unsigned long long>(to_host_empty_suppressed_count64),
         static_cast<unsigned long long>(from_host_empty_drop_count64),
         static_cast<unsigned long long>(pcie_in_backpressure_count64),
         static_cast<unsigned long long>(pcie_out_backpressure_count64),
         static_cast<unsigned long long>(to_host_missing_valid_count64),
         static_cast<unsigned long long>(from_host_group_blocked_count64),
         static_cast<unsigned long long>(from_host_would_blocked_count64),
         static_cast<unsigned long long>(from_host_leaf_blocked_count64),
         static_cast<unsigned long long>(nic_in_blocked_count64),
         static_cast<unsigned long long>(macaddr_blocked_count64),
         static_cast<unsigned long long>(rlimit_blocked_count64),
         static_cast<unsigned long long>(pauser_blocked_count64),
         static_cast<unsigned long long>(nic_in_fire_count64),
         static_cast<unsigned long long>(macaddr_fire_count64),
         static_cast<unsigned long long>(rlimit_fire_count64),
         static_cast<unsigned long long>(pauser_fire_count64));
  printf("SIMPLENIC DEBUG REGS [%s] cycles debug=%llu last_t_fire=%llu "
         "last_to_host_fire=%llu last_from_host_fire=%llu "
         "last_pcie_in_fire=%llu last_pcie_out_fire=%llu "
         "first_blocked_valid=%u first_blocked_cycle=%llu "
         "last_blocked_cycle=%llu\n",
         context,
         static_cast<unsigned long long>(debug_cycle),
         static_cast<unsigned long long>(last_t_fire_cycle),
         static_cast<unsigned long long>(last_to_host_fire_cycle),
         static_cast<unsigned long long>(last_from_host_fire_cycle),
         static_cast<unsigned long long>(last_pcie_in_fire_cycle),
         static_cast<unsigned long long>(last_pcie_out_fire_cycle),
         first_blocked_valid,
         static_cast<unsigned long long>(first_blocked_cycle),
         static_cast<unsigned long long>(last_blocked_cycle));
  print_blocked_reason(context, "current_blocked_reason", current_blocked_reason);
  print_blocked_reason(context, "first_blocked_reason", first_blocked_reason);
  print_blocked_reason(context, "last_blocked_reason", last_blocked_reason);
  printf("SIMPLENIC DEBUG REGS [%s] drives local_queues_ready=%u "
         "target_cycle_ready=%u to_host_ready_drive=%u from_host_valid_drive=%u "
         "from_host_all_ready=%u from_host_group_fire=%u "
         "from_host_group_blocked=%u from_host_token_available=%u "
         "from_payload=%u from_empty=%u from_empty_drop=%u "
         "to_payload=%u to_empty=%u to_empty_bypass=%u to_empty_enqueue=%u "
         "to_empty_suppressed=%u to_payload_blocked=%u "
         "queue_raw htnt(enq_v=%u enq_r=%u deq_v=%u deq_r=%u) "
         "ntht(enq_v=%u enq_r=%u deq_v=%u deq_r=%u)\n",
         context,
         local_queues_ready,
         target_cycle_ready,
         to_host_ready_drive,
         from_host_valid_drive,
         from_host_all_ready,
         from_host_group_fire,
         from_host_group_blocked,
         from_host_token_available,
         from_host_payload_token_available,
         from_host_empty_token_available,
         from_host_empty_drop,
         to_host_payload_valid,
         to_host_empty_valid,
         to_host_empty_bypass,
         to_host_empty_enqueue,
         to_host_empty_suppressed,
         to_host_payload_blocked,
         htnt_queue_enq_valid,
         htnt_queue_enq_ready,
         htnt_queue_deq_valid,
         htnt_queue_deq_ready,
         ntht_queue_enq_valid,
         ntht_queue_enq_ready,
         ntht_queue_deq_valid,
         ntht_queue_deq_ready);
  printf("SIMPLENIC DEBUG REGS [%s] from_host_stage "
         "current(valid=%u data=0x%016llx keep=0x%02x last=%u) "
         "pending(valid=%u data=0x%016llx keep=0x%02x last=%u) "
         "capture=%u capture_ready=%u payload_deq=%u empty_drop_fire=%u "
         "capture_count=%llu last_capture(cycle=%llu data=0x%016llx "
         "keep=0x%02x last=%u in_valid=%u out_ready=%u)\n",
         context,
         from_host_current_payload_valid,
         static_cast<unsigned long long>(from_host_current_payload_data),
         from_host_current_payload_keep,
         from_host_current_payload_last,
         from_host_pending_payload_valid,
         static_cast<unsigned long long>(from_host_pending_payload_data),
         from_host_pending_payload_keep,
         from_host_pending_payload_last,
         from_host_payload_capture,
         from_host_payload_capture_ready,
         from_host_payload_deq,
         from_host_empty_drop_fire,
         static_cast<unsigned long long>(from_host_payload_capture_count64),
         static_cast<unsigned long long>(last_from_host_payload_capture_cycle),
         static_cast<unsigned long long>(last_from_host_payload_capture_data),
         last_from_host_payload_capture_meta & 0xffu,
         (last_from_host_payload_capture_meta >> 8) & 1u,
         (last_from_host_payload_capture_meta >> 9) & 1u,
         (last_from_host_payload_capture_meta >> 10) & 1u);
  printf("SIMPLENIC DEBUG REGS [%s] target_bits in(data=%08x%08x keep=%u last=%u) "
         "out(data=%08x%08x keep=%u last=%u)\n",
         context,
         target_in_bits_data_hi,
         target_in_bits_data_lo,
         target_in_bits_keep,
         target_in_bits_last,
         target_out_bits_data_hi,
         target_out_bits_data_lo,
         target_out_bits_keep,
         target_out_bits_last);
  printf("SIMPLENIC DEBUG REGS [%s] queue_tokens htnt_deq(data=%08x%08x keep=%u "
         "last=%u in_valid=%u out_ready=%u) ntht_enq(data=%08x%08x keep=%u "
         "last=%u out_valid=%u in_ready=%u)\n",
         context,
         htnt_deq_data_hi,
         htnt_deq_data_lo,
         htnt_deq_keep,
         htnt_deq_last,
         htnt_deq_data_in_valid,
         htnt_deq_data_out_ready,
         ntht_enq_data_hi,
         ntht_enq_data_lo,
         ntht_enq_keep,
         ntht_enq_last,
         ntht_enq_data_out_valid,
         ntht_enq_data_in_ready);
  printf("SIMPLENIC DEBUG REGS [%s] progress first_valid=%u "
         "first_event=0x%08x first_cycle=%llu seen=0x%08x "
         "last_event=0x%08x last_cycle=%llu blocked_streak=%llu "
         "max_blocked_streak=%llu from_leaf_blocked_streak=%llu "
         "from_leaf_blocked_max=%llu to_payload_blocked_streak=%llu "
         "to_payload_blocked_max=%llu\n",
         context,
         first_progress_valid,
         first_progress_event_mask,
         static_cast<unsigned long long>(first_progress_cycle),
         progress_seen_mask,
         last_progress_event_mask,
         static_cast<unsigned long long>(last_progress_cycle),
         static_cast<unsigned long long>(blocked_streak_count64),
         static_cast<unsigned long long>(max_blocked_streak_count64),
         static_cast<unsigned long long>(from_host_leaf_blocked_streak_count64),
         static_cast<unsigned long long>(from_host_leaf_blocked_max_streak_count64),
         static_cast<unsigned long long>(to_host_payload_blocked_streak_count64),
         static_cast<unsigned long long>(to_host_payload_blocked_max_streak_count64));
  print_queue_snapshot(context, "first_progress_queue_snapshot",
                       first_progress_queue_snapshot);
  print_adapter_snapshot(context, "first_progress_adapter_snapshot",
                         first_progress_adapter_snapshot);
  print_queue_snapshot(context, "last_progress_queue_snapshot",
                       last_progress_queue_snapshot);
  print_adapter_snapshot(context, "last_progress_adapter_snapshot",
                         last_progress_adapter_snapshot);
  printf("SIMPLENIC DEBUG REGS [%s] ever_masks to_host(v=0x%x r=0x%x "
         "f=0x%x b=0x%x) from_host(v=0x%x r=0x%x f=0x%x b=0x%x)\n",
         context,
         to_host_ever_valid_mask,
         to_host_ever_ready_mask,
         to_host_ever_fire_mask,
         to_host_ever_blocked_mask,
         from_host_ever_valid_mask,
         from_host_ever_ready_mask,
         from_host_ever_fire_mask,
         from_host_ever_blocked_mask);
  printf("SIMPLENIC DEBUG REGS [%s] last_blocked_masks to_host(v=0x%x "
         "r=0x%x f=0x%x b=0x%x) from_host(v=0x%x r=0x%x f=0x%x b=0x%x) "
         "queue_snapshot=0x%08x adapter_snapshot=0x%08x\n",
         context,
         last_blocked_to_host_valid_mask,
         last_blocked_to_host_ready_mask,
         last_blocked_to_host_fire_mask,
         last_blocked_to_host_blocked_mask,
         last_blocked_from_host_valid_mask,
         last_blocked_from_host_ready_mask,
         last_blocked_from_host_fire_mask,
         last_blocked_from_host_blocked_mask,
         last_blocked_queue_snapshot,
         last_blocked_adapter_snapshot);
  print_queue_snapshot(context, "first_blocked_queue_snapshot",
                       first_blocked_queue_snapshot);
  print_adapter_snapshot(context, "first_blocked_adapter_snapshot",
                         first_blocked_adapter_snapshot);
  print_queue_snapshot(context, "last_blocked_queue_snapshot",
                       last_blocked_queue_snapshot);
  print_adapter_snapshot(context, "last_blocked_adapter_snapshot",
                         last_blocked_adapter_snapshot);
  printf("SIMPLENIC DEBUG REGS [%s] blocked_edges from_host first(valid=%u "
         "cycle=%llu ready=0x%x valid_mask=0x%x) last(cycle=%llu ready=0x%x "
         "valid_mask=0x%x) to_host first(valid=%u cycle=%llu ready=0x%x "
         "valid_mask=0x%x) last(cycle=%llu ready=0x%x valid_mask=0x%x) "
         "from_leaf first(valid=%u cycle=%llu blocked=0x%x) "
         "last(cycle=%llu blocked=0x%x)\n",
         context,
         first_from_host_blocked_valid,
         static_cast<unsigned long long>(first_from_host_blocked_cycle),
         first_from_host_blocked_ready_mask,
         first_from_host_blocked_valid_mask,
         static_cast<unsigned long long>(last_from_host_blocked_cycle),
         last_from_host_blocked_ready_mask,
         last_from_host_blocked_valid_mask,
         first_to_host_blocked_valid,
         static_cast<unsigned long long>(first_to_host_blocked_cycle),
         first_to_host_blocked_ready_mask,
         first_to_host_blocked_valid_mask,
         static_cast<unsigned long long>(last_to_host_blocked_cycle),
         last_to_host_blocked_ready_mask,
         last_to_host_blocked_valid_mask,
         first_from_host_leaf_blocked_valid,
         static_cast<unsigned long long>(first_from_host_leaf_blocked_cycle),
         first_from_host_leaf_blocked_mask,
         static_cast<unsigned long long>(last_from_host_leaf_blocked_cycle),
         last_from_host_leaf_blocked_mask);
  printf("SIMPLENIC DEBUG REGS [%s] chain_counts htnt(payload_enq=%llu "
         "empty_enq=%llu) target_in(payload_fire=%llu empty_fire=%llu) "
         "ntht(payload_enq=%llu payload_deq=%llu)\n",
         context,
         static_cast<unsigned long long>(htnt_payload_enq_count64),
         static_cast<unsigned long long>(htnt_empty_enq_count64),
         static_cast<unsigned long long>(target_in_payload_fire_count64),
         static_cast<unsigned long long>(target_in_empty_fire_count64),
         static_cast<unsigned long long>(ntht_payload_enq_count64),
         static_cast<unsigned long long>(ntht_payload_deq_count64));
  printf("SIMPLENIC DEBUG REGS [%s] chain_first_cycles htnt_payload_enq=%llu "
         "target_in_payload_fire=%llu target_out_payload_fire=%llu "
         "ntht_payload_enq=%llu pcie_out_fire=%llu\n",
         context,
         static_cast<unsigned long long>(first_htnt_payload_enq_cycle),
         static_cast<unsigned long long>(first_target_in_payload_fire_cycle),
         static_cast<unsigned long long>(first_target_out_payload_fire_cycle),
         static_cast<unsigned long long>(first_ntht_payload_enq_cycle),
         static_cast<unsigned long long>(first_pcie_out_fire_cycle));
  printf("SIMPLENIC DEBUG REGS [%s] last_htnt enq(cycle=%llu "
         "data=0x%016llx meta=0x%08x keep=0x%02x last=%u in_valid=%u "
         "out_ready=%u) deq(cycle=%llu data=0x%016llx meta=0x%08x "
         "keep=0x%02x last=%u in_valid=%u out_ready=%u)\n",
         context,
         static_cast<unsigned long long>(last_htnt_enq_cycle),
         static_cast<unsigned long long>(last_htnt_enq_data),
         last_htnt_enq_meta,
         last_htnt_enq_meta & 0xffu,
         (last_htnt_enq_meta >> 8) & 1u,
         (last_htnt_enq_meta >> 9) & 1u,
         (last_htnt_enq_meta >> 10) & 1u,
         static_cast<unsigned long long>(last_htnt_deq_cycle),
         static_cast<unsigned long long>(last_htnt_deq_data),
         last_htnt_deq_meta,
         last_htnt_deq_meta & 0xffu,
         (last_htnt_deq_meta >> 8) & 1u,
         (last_htnt_deq_meta >> 9) & 1u,
         (last_htnt_deq_meta >> 10) & 1u);
  printf("SIMPLENIC DEBUG REGS [%s] last_target in(cycle=%llu "
         "data=0x%016llx meta=0x%08x keep=0x%02x last=%u payload_valid=%u "
         "hvalid=%u ready=%u fire=%u) out(cycle=%llu data=0x%016llx "
         "meta=0x%08x keep=0x%02x last=%u payload_valid=%u hvalid=%u "
         "ready=%u fire=%u)\n",
         context,
         static_cast<unsigned long long>(last_target_in_fire_cycle),
         static_cast<unsigned long long>(last_target_in_fire_data),
         last_target_in_fire_meta,
         last_target_in_fire_meta & 0xffu,
         (last_target_in_fire_meta >> 8) & 1u,
         (last_target_in_fire_meta >> 9) & 1u,
         (last_target_in_fire_meta >> 10) & 1u,
         (last_target_in_fire_meta >> 11) & 1u,
         (last_target_in_fire_meta >> 12) & 1u,
         static_cast<unsigned long long>(last_target_out_fire_cycle),
         static_cast<unsigned long long>(last_target_out_fire_data),
         last_target_out_fire_meta,
         last_target_out_fire_meta & 0xffu,
         (last_target_out_fire_meta >> 8) & 1u,
         (last_target_out_fire_meta >> 9) & 1u,
         (last_target_out_fire_meta >> 10) & 1u,
         (last_target_out_fire_meta >> 11) & 1u,
         (last_target_out_fire_meta >> 12) & 1u);
  printf("SIMPLENIC DEBUG REGS [%s] last_ntht enq(cycle=%llu "
         "data=0x%016llx meta=0x%08x keep=0x%02x last=%u out_valid=%u "
         "in_ready=%u) deq(cycle=%llu data=0x%016llx meta=0x%08x "
         "keep=0x%02x last=%u out_valid=%u in_ready=%u)\n",
         context,
         static_cast<unsigned long long>(last_ntht_enq_cycle),
         static_cast<unsigned long long>(last_ntht_enq_data),
         last_ntht_enq_meta,
         last_ntht_enq_meta & 0xffu,
         (last_ntht_enq_meta >> 8) & 1u,
         (last_ntht_enq_meta >> 9) & 1u,
         (last_ntht_enq_meta >> 10) & 1u,
         static_cast<unsigned long long>(last_ntht_deq_cycle),
         static_cast<unsigned long long>(last_ntht_deq_data),
         last_ntht_deq_meta,
         last_ntht_deq_meta & 0xffu,
         (last_ntht_deq_meta >> 8) & 1u,
         (last_ntht_deq_meta >> 9) & 1u,
         (last_ntht_deq_meta >> 10) & 1u);
  fflush(stdout);
#endif
}

void simplenic_t::dump_tx_path_snapshot(const char *context) {
  auto read_u64 = [this](uint64_t lo_addr, uint64_t hi_addr) -> uint64_t {
    const uint64_t lo = read(lo_addr);
    const uint64_t hi = read(hi_addr);
    return (hi << 32) | lo;
  };

  const auto to_host_payload_fire_count64 =
      read_u64(mmio_addrs.to_host_payload_fire_count64_0,
               mmio_addrs.to_host_payload_fire_count64_1);
  const auto to_host_empty_fire_count64 =
      read_u64(mmio_addrs.to_host_empty_fire_count64_0,
               mmio_addrs.to_host_empty_fire_count64_1);
  const auto pcie_out_fire_count = read(mmio_addrs.pcie_out_fire_count);
  const auto nicbig_output_queue_count =
      read(mmio_addrs.nicbig_output_queue_count);
  const auto ntht_queue_enq_valid = read(mmio_addrs.ntht_queue_enq_valid);
  const auto ntht_queue_enq_ready = read(mmio_addrs.ntht_queue_enq_ready);
  const auto ntht_queue_deq_valid = read(mmio_addrs.ntht_queue_deq_valid);
  const auto ntht_queue_deq_ready = read(mmio_addrs.ntht_queue_deq_ready);
  const auto nicbig_pcie_out_valid = read(mmio_addrs.nicbig_pcie_out_valid);
  const auto nicbig_pcie_out_ready = read(mmio_addrs.nicbig_pcie_out_ready);

  printf("SIMPLENIC TX SNAPSHOT [%s] compact pcie_out_fire=%u "
         "output_q_count=%u to_payload_fire=%llu to_empty_fire=%llu "
         "ntht(enq_v=%u enq_r=%u deq_v=%u deq_r=%u) "
         "pcie_out(v=%u r=%u)\n",
         context,
         pcie_out_fire_count,
         nicbig_output_queue_count,
         static_cast<unsigned long long>(to_host_payload_fire_count64),
         static_cast<unsigned long long>(to_host_empty_fire_count64),
         ntht_queue_enq_valid,
         ntht_queue_enq_ready,
         ntht_queue_deq_valid,
         ntht_queue_deq_ready,
         nicbig_pcie_out_valid,
         nicbig_pcie_out_ready);
  fflush(stdout);
#if 0
  auto read_u64 = [this](uint64_t lo_addr, uint64_t hi_addr) -> uint64_t {
    const uint64_t lo = read(lo_addr);
    const uint64_t hi = read(hi_addr);
    return (hi << 32) | lo;
  };

  const auto nicbig_pcie_out_fire_count =
      read(mmio_addrs.nicbig_pcie_out_fire_count);
  const auto nicbig_pcie_out_enq_count =
      read(mmio_addrs.nicbig_pcie_out_enq_count);
  const auto nicbig_output_queue_count =
      read(mmio_addrs.nicbig_output_queue_count);
  const auto ntht_queue_enq_fire_count =
      read(mmio_addrs.ntht_queue_enq_fire_count);
  const auto ntht_queue_deq_fire_count =
      read(mmio_addrs.ntht_queue_deq_fire_count);
  const auto to_host_payload_fire_count64 =
      read_u64(mmio_addrs.to_host_payload_fire_count64_0,
               mmio_addrs.to_host_payload_fire_count64_1);
  const auto to_host_empty_fire_count64 =
      read_u64(mmio_addrs.to_host_empty_fire_count64_0,
               mmio_addrs.to_host_empty_fire_count64_1);
  const uint64_t enq_words[8] = {
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word0_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word0_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word1_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word1_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word2_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word2_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word3_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word3_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word4_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word4_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word5_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word5_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word6_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word6_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_enq_word7_0,
               mmio_addrs.nicbig_last_pcie_out_enq_word7_1),
  };
  const uint64_t deq_words[8] = {
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word0_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word0_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word1_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word1_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word2_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word2_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word3_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word3_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word4_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word4_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word5_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word5_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word6_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word6_1),
      read_u64(mmio_addrs.nicbig_last_pcie_out_deq_word7_0,
               mmio_addrs.nicbig_last_pcie_out_deq_word7_1),
  };

  printf("SIMPLENIC TX SNAPSHOT [%s] compact "
         "nicbig_pcie_out_fire=%u nicbig_pcie_out_enq=%u "
         "output_q_count=%u ntht_enq=%u ntht_deq=%u "
         "to_payload_fire=%llu to_empty_fire=%llu\n",
         context,
         nicbig_pcie_out_fire_count,
         nicbig_pcie_out_enq_count,
         nicbig_output_queue_count,
         ntht_queue_enq_fire_count,
         ntht_queue_deq_fire_count,
         static_cast<unsigned long long>(to_host_payload_fire_count64),
         static_cast<unsigned long long>(to_host_empty_fire_count64));
  fflush(stdout);

  const auto to_host_packet_seq =
      read_u64(mmio_addrs.to_host_packet_seq_0,
               mmio_addrs.to_host_packet_seq_1);
  const auto to_host_packet_flit_idx =
      read(mmio_addrs.to_host_packet_flit_idx);
  printf("SIMPLENIC TX SNAPSHOT [%s] to_host_packet_seq=%llu "
         "live_flit_idx=%u\n",
         context,
         static_cast<unsigned long long>(to_host_packet_seq),
         to_host_packet_flit_idx);
  printf("SIMPLENIC TX SNAPSHOT [%s] pcie_out_enq "
         "w0=0x%016llx w1=0x%016llx w2=0x%016llx w3=0x%016llx "
         "w4=0x%016llx w5=0x%016llx w6=0x%016llx w7=0x%016llx\n",
         context,
         static_cast<unsigned long long>(enq_words[0]),
         static_cast<unsigned long long>(enq_words[1]),
         static_cast<unsigned long long>(enq_words[2]),
         static_cast<unsigned long long>(enq_words[3]),
         static_cast<unsigned long long>(enq_words[4]),
         static_cast<unsigned long long>(enq_words[5]),
         static_cast<unsigned long long>(enq_words[6]),
         static_cast<unsigned long long>(enq_words[7]));
  printf("SIMPLENIC TX SNAPSHOT [%s] pcie_out_deq "
         "w0=0x%016llx w1=0x%016llx w2=0x%016llx w3=0x%016llx "
         "w4=0x%016llx w5=0x%016llx w6=0x%016llx w7=0x%016llx\n",
         context,
         static_cast<unsigned long long>(deq_words[0]),
         static_cast<unsigned long long>(deq_words[1]),
         static_cast<unsigned long long>(deq_words[2]),
         static_cast<unsigned long long>(deq_words[3]),
         static_cast<unsigned long long>(deq_words[4]),
         static_cast<unsigned long long>(deq_words[5]),
         static_cast<unsigned long long>(deq_words[6]),
         static_cast<unsigned long long>(deq_words[7]));
  fflush(stdout);
#endif
}

void simplenic_t::dump_token_buffer(const char *context,
                                    const char *buf,
                                    uint32_t bigtokens,
                                    uint32_t actual_bytes,
                                    uint64_t *event_counter) {
  if (token_debug_limit == 0) {
    return;
  }

  (*event_counter)++;
  const uint64_t event = *event_counter;
  const auto *words = reinterpret_cast<const uint64_t *>(buf);
  uint32_t nonzero_lrv = 0;
  uint32_t valid_flits = 0;
  uint32_t last_flits = 0;
  uint32_t short_packets = 0;
  uint32_t zero_first_packets = 0;
  uint32_t zero_single_flit_packets = 0;
  bool in_packet = false;
  uint32_t packet_flits = 0;
  uint64_t packet_first_data = 0;

  for (uint32_t bigtoken = 0; bigtoken < bigtokens; bigtoken++) {
    const uint64_t lrv = words[bigtoken * 8];
    if (lrv != 0) {
      nonzero_lrv++;
    }
    for (int token = 0; token < TOKENS_PER_BIGTOKEN; token++) {
      const bool valid = (lrv >> (43 + token * 3)) & 1L;
      if (!valid) {
        continue;
      }
      const bool last = (lrv >> (45 + token * 3)) & 1L;
      const uint64_t data = words[bigtoken * 8 + 1 + token];
      if (!in_packet) {
        in_packet = true;
        packet_flits = 0;
        packet_first_data = data;
      }
      packet_flits++;
      valid_flits++;
      if (last) {
        last_flits++;
        if (packet_flits < 3) {
          short_packets++;
        }
        if (packet_first_data == 0) {
          zero_first_packets++;
        }
        if (packet_flits == 1 && data == 0) {
          zero_single_flit_packets++;
        }
        in_packet = false;
        packet_flits = 0;
      }
    }
  }

  const bool suspicious =
      (actual_bytes > 0 && valid_flits == 0) || short_packets != 0 ||
      zero_first_packets != 0 || zero_single_flit_packets != 0;
  if (event > token_debug_limit &&
      !(suspicious && should_log_coop_wait(event))) {
    return;
  }

  printf("SIMPLENIC TOKEN DEBUG [%s] event=%llu actual_bytes=%u "
         "bigtokens=%u nonzero_lrv=%u valid_flits=%u last_flits=%u "
         "open_packet_flits=%u short_packets=%u zero_first_packets=%u "
         "zero_single_flit_packets=%u suspicious=%u\n",
         context,
         static_cast<unsigned long long>(event),
         actual_bytes,
         bigtokens,
         nonzero_lrv,
         valid_flits,
         last_flits,
         in_packet ? packet_flits : 0,
         short_packets,
         zero_first_packets,
         zero_single_flit_packets,
         suspicious ? 1u : 0u);

  const uint32_t raw_headers = bigtokens < 4 ? bigtokens : 4;
  for (uint32_t bigtoken = 0; bigtoken < raw_headers; bigtoken++) {
    printf("SIMPLENIC TOKEN DEBUG [%s] raw_header bigtoken=%u "
           "lrv=0x%016llx data0=0x%016llx data1=0x%016llx\n",
           context,
           bigtoken,
           static_cast<unsigned long long>(words[bigtoken * 8]),
           static_cast<unsigned long long>(words[bigtoken * 8 + 1]),
           static_cast<unsigned long long>(words[bigtoken * 8 + 2]));
  }
  print_valid_flit_sample(context, buf, bigtokens, 12);
  if (strcmp(context, "target_to_switch_pull") == 0) {
    dump_tx_path_snapshot(context);
  }
  fflush(stdout);
}

#define ceil_div(n, d) (((n)-1) / (d) + 1)

void simplenic_t::init() {
  write(mmio_addrs.macaddr_upper, (mac_lendian >> 32) & 0xFFFF);
  write(mmio_addrs.macaddr_lower, mac_lendian & 0xFFFFFFFF);
  write(mmio_addrs.rlimit_settings,
        (rlimit_inc << 16) | ((rlimit_period - 1) << 8) | rlimit_size);
  write(mmio_addrs.pause_threshold, pause_threshold);
  write(mmio_addrs.pause_times,
        (pause_refresh << 16) | (pause_quanta & 0xffff));

  // In lieu of reading "count", check that the stream is empty by doing a pull.
  // To make this work under alveo we'd almost definitely need to call flush
  // first.
  auto bytes_received = this->pull(
      stream_to_cpu_idx, pcis_read_bufs[0], SIMLATENCY_BT * BUFWIDTH, 0);
  if ((bytes_received != 0)) {
    dump_host_debug("init_pull_expected_empty", SIMLATENCY_BT * BUFWIDTH, bytes_received);
    printf("FAIL. Exactly 1 tokens should be present in the cpu-bound stream "
           "on init");
    exit(1);
  }

  // Enqueue SIMLATENCY_BT beats into the from-cpu stream. This permits the
  // FPGA-hosted part of the simulator to execute SIMLATENCY cycles in the
  // NIC-local clock domain before requiring additional interaction from the
  // driver.
  auto token_bytes_to_send = SIMLATENCY_BT * BUFWIDTH;
  // Set the threshold here to 0 as a proxy for checking the stream capacity.
  // If we cannot enqueue the full payload, the stream is likely undersized
  // for our desired latency or the FPGA has not been properly reset /
  // reprogrammed.
  auto token_bytes_produced = this->push(
      stream_from_cpu_idx, pcis_write_bufs[1], token_bytes_to_send, 0);

  if (token_bytes_produced != token_bytes_to_send) {
    dump_host_debug("init_push_capacity", token_bytes_to_send, token_bytes_produced);
    printf("FAIL. Could not enqueue big tokens to support the desired sim "
           "latency on init. Required %d, enqueued %lu\n",
           SIMLATENCY_BT,
           token_bytes_produced / BUFWIDTH);
    exit(1);
  }
}

// #define TOKENVERIFY

void simplenic_t::tick() {
  /* #define DEBUG_NIC_PRINT */

  const uint32_t tokens_this_round = SIMLATENCY_BT;
  const uint32_t requested_token_bytes = BUFWIDTH * tokens_this_round;

  if (!round_pending) {
    uint32_t token_bytes_obtained_from_fpga = 0;
    const auto required_pull_bytes =
        relaxed_required_bytes ? 0 : requested_token_bytes;
    if (relaxed_required_bytes && pcis_read_buf_dirty[currentround]) {
      clear_bigtoken_headers(pcis_read_bufs[currentround], tokens_this_round);
      pcis_read_buf_dirty[currentround] = false;
    }
    token_bytes_obtained_from_fpga =
        pull(stream_to_cpu_idx,
             pcis_read_bufs[currentround],
             requested_token_bytes,
             required_pull_bytes // In normal mode, copy only if the stream can
                                 // provide exactly as many bytes as we want.
        );

    if (token_bytes_obtained_from_fpga == 0) {
      // The target can be silent while the host-side switch has packets
      // waiting on tap0. Still publish an empty target->host round so the
      // switch can leave ShmemPort::recv(), ingest tap traffic, and return a
      // host->target round for us to push into the FPGA. This is throttled by
      // empty_switch_poll_interval because the FPGA-side bridge now
      // self-generates no-packet host input tokens; polling the switch on every
      // empty NIC cycle burns host CPU and can make Linux boot look stalled.
      pull_empty_ticks++;
      empty_rounds_started++;
      const bool poll_empty_switch =
          empty_switch_poll_interval != 0 &&
          (empty_switch_poll_interval == 1 ||
           (empty_rounds_started % empty_switch_poll_interval) == 0);
      if (!poll_empty_switch) {
        empty_rounds_skipped_switch++;
        successful_rounds++;
        round_pending = false;
        pending_peer_snapshot_valid = false;
        pending_token_bytes = 0;
        pending_push_offset_bytes = 0;
        pending_peer_valid_flits = 0;
        pending_round_from_empty_pull = false;
        if (round_debug_limit > 0 &&
            should_log_coop_wait(empty_rounds_skipped_switch)) {
          dump_host_debug("tick_empty_skip_switch", requested_token_bytes, 0);
        }
        return;
      }
      if (pcis_read_buf_dirty[currentround]) {
        clear_bigtoken_headers(pcis_read_bufs[currentround],
                               tokens_this_round);
        pcis_read_buf_dirty[currentround] = false;
      }
      pending_round_from_empty_pull = true;
      if (round_debug_limit > 0 && should_log_coop_wait(empty_rounds_started)) {
        dump_host_debug("tick_start_empty_round", requested_token_bytes, 0);
      }
    } else if (token_bytes_obtained_from_fpga != requested_token_bytes) {
      if (!relaxed_required_bytes) {
        dump_host_debug("tick_pull_mismatch",
                        BUFWIDTH * tokens_this_round,
                        token_bytes_obtained_from_fpga);
        printf("ERR MISMATCH! on reading tokens out. actually read %d bytes, "
               "wanted %d bytes.\n",
               token_bytes_obtained_from_fpga,
               BUFWIDTH * tokens_this_round);
        printf("errno: %s\n", strerror(errno));
        exit(1);
      }
      pulled_rounds_started++;
      partial_pull_rounds_started++;
      pending_round_from_empty_pull = false;
      pcis_read_buf_dirty[currentround] = true;
      if (round_debug_limit > 0 &&
          should_log_coop_wait(partial_pull_rounds_started)) {
        dump_host_debug("tick_partial_pull",
                        requested_token_bytes,
                        token_bytes_obtained_from_fpga);
      }
    } else {
      pulled_rounds_started++;
      pending_round_from_empty_pull = false;
      pcis_read_buf_dirty[currentround] = true;
    }
    if (token_bytes_obtained_from_fpga > 0) {
      dump_token_buffer("target_to_switch_pull",
                        pcis_read_bufs[currentround],
                        tokens_this_round,
                        token_bytes_obtained_from_fpga,
                        &to_host_token_debug_events);
    }
    // read into read_buffer
    pcis_read_bufs[currentround][BUFBYTES] = 1;
    round_pending = true;
    pending_token_bytes = requested_token_bytes;
    pending_push_offset_bytes = 0;

#ifdef DEBUG_NIC_PRINT
    niclog_printf("send pcis_read_bufs[%d][%d]: %d\n",
                  currentround,
                  BUFBYTES,
                  pcis_read_bufs[currentround][BUFBYTES]);
#endif


#ifdef TOKENVERIFY
    // the widget is designed to tag tokens with a 43 bit number,
    // incrementing for each sent token. verify that we are not losing
    // tokens over PCIS
    for (int i = 0; i < tokens_this_round; i++) {
      uint64_t TOKENLRV_AND_COUNT =
          *(((uint64_t *)pcis_read_bufs[currentround]) + i * 8);
      uint8_t LAST;
      for (int token_in_bigtoken = 0; token_in_bigtoken < 7;
           token_in_bigtoken++) {
        if (TOKENLRV_AND_COUNT & (1L << (43 + token_in_bigtoken * 3))) {
          LAST = (TOKENLRV_AND_COUNT >> (45 + token_in_bigtoken * 3)) & 0x1;
          niclog_printf("sending to other node, valid data chunk: "
                        "%016lx, last %x, sendcycle: %016ld\n",
                        *((((uint64_t *)pcis_read_bufs[currentround]) + i * 8) +
                          1 + token_in_bigtoken),
                        LAST,
                        timeelapsed_cycles + i * 7 + token_in_bigtoken);
        }
      }

      //            *((uint64_t*)(pcis_read_buf + i*64)) |= 0x4924900000000000;
      uint32_t thistoken =
          *((uint32_t *)(pcis_read_bufs[currentround] + i * 64));
      if (thistoken != next_token_from_fpga) {
        niclog_printf("FAIL! Token lost on FPGA interface.\n");
        exit(1);
      }
      next_token_from_fpga++;
    }
#endif
    if (!relaxed_required_bytes &&
        token_bytes_obtained_from_fpga != 0 &&
        token_bytes_obtained_from_fpga != tokens_this_round * BUFWIDTH) {
      dump_host_debug(
          "tick_pull_mismatch",
          BUFWIDTH * tokens_this_round,
          token_bytes_obtained_from_fpga);
      printf("ERR MISMATCH! on reading tokens out. actually read %d bytes, "
             "wanted %d bytes.\n",
             token_bytes_obtained_from_fpga,
             BUFWIDTH * tokens_this_round);
      printf("errno: %s\n", strerror(errno));
      exit(1);
    }

#ifdef TOKENVERIFY
    timeelapsed_cycles += LINKLATENCY;
#endif
  }

#ifdef DEBUG_NIC_PRINT
    niclog_printf("receiving ... round %d\n", currentround);
#endif

    if (!pending_peer_snapshot_valid && !loopback) {
      volatile uint8_t *polladdr =
          (uint8_t *)(pcis_write_bufs[currentround] + BUFBYTES);
      if (*polladdr == 0) {
        peer_wait_ticks++;
        if (round_debug_limit > 0 && should_log_coop_wait(peer_wait_ticks)) {
          dump_host_debug("tick_wait_peer", pending_token_bytes, 0);
        }
        return;
      }
    }

#ifdef DEBUG_NIC_PRINT
    niclog_printf("done recv round %d\n", currentround);
#endif

#ifdef TOKENVERIFY
    // this does not do tokenverify - it's just printing tokens
    // there should not be tokenverify on this interface
    for (int i = 0; i < tokens_this_round; i++) {
      uint64_t TOKENLRV_AND_COUNT =
          *(((uint64_t *)pcis_write_bufs[currentround]) + i * 8);
      uint8_t LAST;
      for (int token_in_bigtoken = 0; token_in_bigtoken < 7;
           token_in_bigtoken++) {
        if (TOKENLRV_AND_COUNT & (1L << (43 + token_in_bigtoken * 3))) {
          LAST = (TOKENLRV_AND_COUNT >> (45 + token_in_bigtoken * 3)) & 0x1;
          niclog_printf(
              "from other node, valid data chunk: %016lx, "
              "last %x, recvcycle: %016ld\n",
              *((((uint64_t *)pcis_write_bufs[currentround]) + i * 8) + 1 +
                token_in_bigtoken),
              LAST,
              timeelapsed_cycles + i * 7 + token_in_bigtoken);
        }
      }
    }
#endif
    uint32_t token_bytes_sent_to_fpga = 0;
    if (!pending_peer_snapshot_valid) {
      const auto *peer_words =
          reinterpret_cast<const uint64_t *>(pcis_write_bufs[currentround]);
      const bool peer_empty_marker =
          peer_words[0] == EMPTY_SHMEM_ROUND_MARKER;
      const uint32_t peer_valid_flits =
          peer_empty_marker
              ? 0
              : count_valid_flits(pcis_write_bufs[currentround],
                                  tokens_this_round);
      if (peer_valid_flits > 0 ||
          (!peer_empty_marker && peer_words[0] != 0)) {
        dump_token_buffer("switch_to_target_peer",
                          pcis_write_bufs[currentround],
                          tokens_this_round,
                          pending_token_bytes,
                          &peer_token_debug_events);
      }
      pending_peer_snapshot_valid = true;
      pending_peer_valid_flits = peer_valid_flits;
      last_peer_valid_flits = pending_peer_valid_flits;
      if (pending_peer_valid_flits > 0) {
        memcpy(pending_peer_buf,
               pcis_write_bufs[currentround],
               BUFBYTES + EXTRABYTES);
      }
      if (!loopback) {
        pcis_write_bufs[currentround][BUFBYTES] = 0;
        if (peer_empty_marker) {
          reinterpret_cast<uint64_t *>(pcis_write_bufs[currentround])[0] = 0;
        }
      }

      if (pending_peer_valid_flits > 0) {
        peer_input_rounds++;
        last_peer_input_round = successful_rounds;
        if (round_debug_limit > 0 && should_log_coop_wait(peer_input_rounds)) {
          dump_host_debug("tick_peer_input",
                          pending_token_bytes,
                          pending_peer_valid_flits);
          print_valid_flit_sample("tick_peer_input",
                                  pending_peer_buf,
                                  tokens_this_round,
                                  8);
        }
      } else {
        peer_empty_rounds++;
        if (pending_round_from_empty_pull &&
            round_debug_limit > 0 && should_log_coop_wait(peer_empty_rounds)) {
          dump_host_debug("tick_peer_empty_after_empty_pull",
                          pending_token_bytes,
                          0);
        }
      }
    } else {
      last_peer_valid_flits = pending_peer_valid_flits;
    }

    // The FPGA-side bridge now self-generates stable no-packet host input
    // tokens. Empty switch rounds therefore do not need to be replayed through
    // the CPU-managed from-host stream; doing so costs one BAR4 write per
    // 64-bit word and dominates boot time on F2 after the stream width fix.
    if (pending_peer_valid_flits == 0) {
      if (successful_rounds < round_debug_limit) {
        dump_host_debug("tick_peer_empty_skip_push", pending_token_bytes, 0);
      }
      successful_rounds++;
      round_pending = false;
      pending_peer_snapshot_valid = false;
      pending_token_bytes = 0;
      pending_push_offset_bytes = 0;
      pending_peer_valid_flits = 0;
      pending_round_from_empty_pull = false;
      currentround = (currentround + 1) % 2;
      return;
    }

    const auto remaining_push_bytes =
        pending_token_bytes - pending_push_offset_bytes;
    if (pending_push_offset_bytes == 0) {
      dump_token_buffer("host_to_target_push",
                        pending_peer_buf,
                        tokens_this_round,
                        pending_token_bytes,
                        &push_token_debug_events);
    }
    token_bytes_sent_to_fpga = push(stream_from_cpu_idx,
                                    pending_peer_buf +
                                        pending_push_offset_bytes,
                                    remaining_push_bytes,
                                    0);
    if (token_bytes_sent_to_fpga == 0) {
      push_blocked_ticks++;
      if (round_debug_limit > 0 && should_log_coop_wait(push_blocked_ticks)) {
        dump_host_debug(
            "tick_push_blocked_retry", remaining_push_bytes, 0);
      }
      return;
    }
    if (token_bytes_sent_to_fpga > remaining_push_bytes ||
        (token_bytes_sent_to_fpga % BUFWIDTH) != 0) {
      dump_host_debug(
          "tick_push_mismatch",
          remaining_push_bytes,
          token_bytes_sent_to_fpga);
      printf("ERR MISMATCH! on writing tokens in. actually wrote in %d bytes, "
             "wanted %d bytes.\n",
             token_bytes_sent_to_fpga,
             remaining_push_bytes);
      printf("errno: %s\n", strerror(errno));
      exit(1);
    }
    pending_push_offset_bytes += token_bytes_sent_to_fpga;
    if (pending_push_offset_bytes < pending_token_bytes) {
      return;
    }

    if (successful_rounds < round_debug_limit) {
      dump_host_debug(
          "tick_round_success", pending_token_bytes, token_bytes_sent_to_fpga);
    }
    successful_rounds++;
    round_pending = false;
    pending_peer_snapshot_valid = false;
    pending_token_bytes = 0;
    pending_push_offset_bytes = 0;
    pending_peer_valid_flits = 0;
    pending_round_from_empty_pull = false;
    currentround = (currentround + 1) % 2;
}
