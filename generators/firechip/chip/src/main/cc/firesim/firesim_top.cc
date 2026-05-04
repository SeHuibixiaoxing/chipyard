// See LICENSE for license details

#include "bridges/clock.h"
#include "bridges/blockdev.h"
#include "bridges/fased_memory_timing_model.h"
#include "bridges/heartbeat.h"
#include "bridges/peek_poke.h"
#include "bridges/reset_pulse.h"
#include "bridges/simplenic.h"
#include "bridges/tsibridge.h"
#include "bridges/uart.h"
#include "core/bridge_driver.h"
#include "core/simif.h"
#include "core/simulation.h"
#include "core/systematic_scheduler.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>

namespace {
bool has_arg(const std::vector<std::string> &args, const char *name) {
  for (const auto &arg : args) {
    if (arg == name)
      return true;
  }
  return false;
}

uint64_t get_uint64_arg(const std::vector<std::string> &args,
                        const char *prefix,
                        uint64_t default_value) {
  const size_t prefix_len = std::strlen(prefix);
  for (const auto &arg : args) {
    if (arg.find(prefix) == 0)
      return std::strtoull(arg.c_str() + prefix_len, nullptr, 0);
  }
  return default_value;
}

const char *bridge_name(bridge_driver_t *bridge) {
  if (dynamic_cast<heartbeat_t *>(bridge))
    return "heartbeat";
  if (dynamic_cast<peek_poke_t *>(bridge))
    return "peek_poke";
  if (dynamic_cast<reset_pulse_t *>(bridge))
    return "reset_pulse";
  if (dynamic_cast<uart_t *>(bridge))
    return "uart";
  if (dynamic_cast<blockdev_t *>(bridge))
    return "blockdev";
  if (dynamic_cast<FASEDMemoryTimingModel *>(bridge))
    return "fased_memory";
  if (dynamic_cast<simplenic_t *>(bridge))
    return "simplenic";
  if (dynamic_cast<tsibridge_t *>(bridge))
    return "tsibridge";
  return "unknown";
}
} // namespace

class firesim_top_t : public systematic_scheduler_t, public simulation_t {
public:
  firesim_top_t(simif_t &simif,
                widget_registry_t &registry,
                const std::vector<std::string> &args);

  int simulation_run() override;

  bool simulation_timed_out() override { return !terminated; }

private:
  simif_t &simif;
  /// Reference to the peek-poke bridge.
  peek_poke_t &peek_poke;
  /// Flag to indicate that the simulation was terminated.
  bool terminated = false;
  /// Optional, plusarg-gated driver progress logging for FPGA hangs.
  bool driver_debug = false;
  uint64_t driver_debug_interval = 1000000;
};

firesim_top_t::firesim_top_t(simif_t &simif,
                             widget_registry_t &registry,
                             const std::vector<std::string> &args)
    : systematic_scheduler_t(args), simulation_t(registry, args), simif(simif),
      peek_poke(registry.get_widget<peek_poke_t>()) {

  // Cycles to advance before profiling instrumentation registers in models.
  std::optional<uint64_t> profile_interval;
  for (auto &arg : args) {
    if (arg.find("+profile-interval=") == 0) {
      profile_interval = atoi(arg.c_str() + 18);

      if (*profile_interval == 0) {
        fprintf(stderr, "Must provide a profile interval > 0\n");
        exit(1);
      }
    }
  }

  driver_debug = has_arg(args, "+firesim-driver-debug");
  driver_debug_interval =
      get_uint64_arg(args, "+firesim-driver-debug-interval=", 1000000);
  if (driver_debug_interval == 0) {
    driver_debug_interval = 1;
  }
  if (driver_debug) {
    fprintf(stderr,
            "FIRESIM DRIVER DEBUG enabled interval=%" PRIu64 "\n",
            driver_debug_interval);
  }

  registry.add_widget(
      new heartbeat_t(simif, registry.get_widget<clockmodule_t>(), args));

  // Add functions you'd like to periodically invoke on a paused simulator here.
  if (profile_interval) {
    register_task(
        0, [&, profile_interval] { // capture profile_interval by value,
                                   //  since its lifetime is bound to
                                   //  firesim_top_t's constructor
          for (auto &mod : registry.get_bridges<FASEDMemoryTimingModel>()) {
            mod->profile();
          }
          return *profile_interval;
        });
  }
}

int firesim_top_t::simulation_run() {
  int exit_code = 0;
  uint64_t step_count = 0;
  uint64_t total_poll_count = 0;
  auto &clock = registry.get_widget<clockmodule_t>();
  while (!terminated && !finished_scheduled_tasks()) {
    run_scheduled_tasks();
    const uint32_t step_size = get_largest_stepsize();
    if (driver_debug) {
      const uint64_t before_tcycle = clock.tcycle();
      const uint64_t before_hcycle = clock.hcycle();
      const bool before_done = peek_poke.is_done();
      fprintf(stderr,
              "FIRESIM DRIVER DEBUG [before_step] step=%" PRIu64
              " step_size=%" PRIu32 " done=%u tcycle=%" PRIu64
              " hcycle=%" PRIu64 "\n",
              step_count,
              step_size,
              before_done ? 1 : 0,
              before_tcycle,
              before_hcycle);
    }
    peek_poke.step(step_size, false);
    uint64_t poll_count = 0;
    while (!peek_poke.is_done() && !terminated) {
      poll_count++;
      total_poll_count++;
      if (driver_debug && (poll_count == 1 ||
                           (poll_count % driver_debug_interval) == 0)) {
        fprintf(stderr,
                "FIRESIM DRIVER DEBUG [poll] step=%" PRIu64
                " poll=%" PRIu64 " total_poll=%" PRIu64
                " done=0 tcycle=%" PRIu64 " hcycle=%" PRIu64 "\n",
                step_count,
                poll_count,
                total_poll_count,
                clock.tcycle(),
                clock.hcycle());
      }
      for (auto *bridge : registry.get_all_bridges()) {
        bridge->tick();
        if (bridge->terminate()) {
          exit_code = bridge->exit_code();
          terminated = true;
          if (driver_debug) {
            fprintf(stderr,
                    "FIRESIM DRIVER DEBUG [terminate] step=%" PRIu64
                    " poll=%" PRIu64 " total_poll=%" PRIu64
                    " bridge=%s exit_code=%d tcycle=%" PRIu64
                    " hcycle=%" PRIu64 "\n",
                    step_count,
                    poll_count,
                    total_poll_count,
                    bridge_name(bridge),
                    exit_code,
                    clock.tcycle(),
                    clock.hcycle());
          }
          break;
        }
      }
    }
    if (driver_debug) {
      const bool after_done = peek_poke.is_done();
      fprintf(stderr,
              "FIRESIM DRIVER DEBUG [after_step] step=%" PRIu64
              " polls=%" PRIu64 " total_poll=%" PRIu64
              " done=%u terminated=%u tcycle=%" PRIu64
              " hcycle=%" PRIu64 "\n",
              step_count,
              poll_count,
              total_poll_count,
              after_done ? 1 : 0,
              terminated ? 1 : 0,
              clock.tcycle(),
              clock.hcycle());
    }
    step_count++;
  }
  return exit_code;
}

std::unique_ptr<simulation_t>
create_simulation(simif_t &simif,
                  widget_registry_t &registry,
                  const std::vector<std::string> &args) {
  return std::make_unique<firesim_top_t>(simif, registry, args);
}
