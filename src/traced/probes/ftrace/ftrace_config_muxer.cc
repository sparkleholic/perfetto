/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/traced/probes/ftrace/ftrace_config_muxer.h"

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "src/traced/probes/ftrace/atrace_wrapper.h"
#include "src/traced/probes/ftrace/compact_sched.h"

namespace perfetto {
namespace {

constexpr int kDefaultPerCpuBufferSizeKb = 2 * 1024;  // 2mb
constexpr int kMaxPerCpuBufferSizeKb = 64 * 1024;     // 64mb

// trace_clocks in preference order.
// If this list is changed, the FtraceClocks enum in ftrace_event_bundle.proto
// and FtraceConfigMuxer::SetupClock() should be also changed accordingly.
constexpr const char* kClocks[] = {"boot", "global", "local"};

void AddEventGroup(const ProtoTranslationTable* table,
                   const std::string& group,
                   std::set<GroupAndName>* to) {
  const std::vector<const Event*>* events = table->GetEventsByGroup(group);
  if (!events)
    return;
  for (const Event* event : *events)
    to->insert(GroupAndName(group, event->name));
}

std::set<GroupAndName> ReadEventsInGroupFromFs(
    const FtraceProcfs& ftrace_procfs,
    const std::string& group) {
  std::set<std::string> names =
      ftrace_procfs.GetEventNamesForGroup("events/" + group);
  std::set<GroupAndName> events;
  for (const auto& name : names)
    events.insert(GroupAndName(group, name));
  return events;
}

std::pair<std::string, std::string> EventToStringGroupAndName(
    const std::string& event) {
  auto slash_pos = event.find('/');
  if (slash_pos == std::string::npos)
    return std::make_pair("", event);
  return std::make_pair(event.substr(0, slash_pos),
                        event.substr(slash_pos + 1));
}

void UnionInPlace(const std::vector<std::string>& unsorted_a,
                  std::vector<std::string>* out) {
  std::vector<std::string> a = unsorted_a;
  std::sort(a.begin(), a.end());
  std::sort(out->begin(), out->end());
  std::vector<std::string> v;
  std::set_union(a.begin(), a.end(), out->begin(), out->end(),
                 std::back_inserter(v));
  *out = std::move(v);
}

void IntersectInPlace(const std::vector<std::string>& unsorted_a,
                      std::vector<std::string>* out) {
  std::vector<std::string> a = unsorted_a;
  std::sort(a.begin(), a.end());
  std::sort(out->begin(), out->end());
  std::vector<std::string> v;
  std::set_intersection(a.begin(), a.end(), out->begin(), out->end(),
                        std::back_inserter(v));
  *out = std::move(v);
}

// This is just to reduce binary size and stack frame size of the insertions.
// It effectively undoes STL's set::insert inlining.
void PERFETTO_NO_INLINE InsertEvent(const char* group,
                                    const char* name,
                                    std::set<GroupAndName>* dst) {
  dst->insert(GroupAndName(group, name));
}

}  // namespace

std::set<GroupAndName> FtraceConfigMuxer::GetFtraceEvents(
    const FtraceConfig& request,
    const ProtoTranslationTable* table) {
  std::set<GroupAndName> events;
  for (const auto& config_value : request.ftrace_events()) {
    std::string group;
    std::string name;
    std::tie(group, name) = EventToStringGroupAndName(config_value);
    if (name == "*") {
      for (const auto& event : ReadEventsInGroupFromFs(*ftrace_, group))
        events.insert(event);
    } else if (group.empty()) {
      // If there is no group specified, find an event with that name and
      // use it's group.
      const Event* e = table->GetEventByName(name);
      if (!e) {
        PERFETTO_DLOG(
            "Event doesn't exist: %s. Include the group in the config to allow "
            "the event to be output as a generic event.",
            name.c_str());
        continue;
      }
      events.insert(GroupAndName(e->group, e->name));
    } else {
      events.insert(GroupAndName(group, name));
    }
  }
  if (RequiresAtrace(request)) {
    InsertEvent("ftrace", "print", &events);

    // Ideally we should keep this code in sync with:
    // platform/frameworks/native/cmds/atrace/atrace.cpp
    // It's not a disaster if they go out of sync, we can always add the ftrace
    // categories manually server side but this is user friendly and reduces the
    // size of the configs.
    for (const std::string& category : request.atrace_categories()) {
      if (category == "gfx") {
        AddEventGroup(table, "mdss", &events);
        InsertEvent("mdss", "rotator_bw_ao_as_context", &events);
        InsertEvent("mdss", "mdp_trace_counter", &events);
        InsertEvent("mdss", "tracing_mark_write", &events);
        InsertEvent("mdss", "mdp_cmd_wait_pingpong", &events);
        InsertEvent("mdss", "mdp_cmd_kickoff", &events);
        InsertEvent("mdss", "mdp_cmd_release_bw", &events);
        InsertEvent("mdss", "mdp_cmd_readptr_done", &events);
        InsertEvent("mdss", "mdp_cmd_pingpong_done", &events);
        InsertEvent("mdss", "mdp_misr_crc", &events);
        InsertEvent("mdss", "mdp_compare_bw", &events);
        InsertEvent("mdss", "mdp_perf_update_bus", &events);
        InsertEvent("mdss", "mdp_video_underrun_done", &events);
        InsertEvent("mdss", "mdp_commit", &events);
        InsertEvent("mdss", "mdp_mixer_update", &events);
        InsertEvent("mdss", "mdp_perf_prefill_calc", &events);
        InsertEvent("mdss", "mdp_perf_set_ot", &events);
        InsertEvent("mdss", "mdp_perf_set_wm_levels", &events);
        InsertEvent("mdss", "mdp_perf_set_panic_luts", &events);
        InsertEvent("mdss", "mdp_perf_set_qos_luts", &events);
        InsertEvent("mdss", "mdp_sspp_change", &events);
        InsertEvent("mdss", "mdp_sspp_set", &events);
        AddEventGroup(table, "mali", &events);
        InsertEvent("mali", "tracing_mark_write", &events);

        AddEventGroup(table, "sde", &events);
        InsertEvent("sde", "tracing_mark_write", &events);
        InsertEvent("sde", "sde_perf_update_bus", &events);
        InsertEvent("sde", "sde_perf_set_qos_luts", &events);
        InsertEvent("sde", "sde_perf_set_ot", &events);
        InsertEvent("sde", "sde_perf_set_danger_luts", &events);
        InsertEvent("sde", "sde_perf_crtc_update", &events);
        InsertEvent("sde", "sde_perf_calc_crtc", &events);
        InsertEvent("sde", "sde_evtlog", &events);
        InsertEvent("sde", "sde_encoder_underrun", &events);
        InsertEvent("sde", "sde_cmd_release_bw", &events);

        AddEventGroup(table, "dpu", &events);
        InsertEvent("dpu", "tracing_mark_write", &events);

        AddEventGroup(table, "g2d", &events);
        InsertEvent("g2d", "tracing_mark_write", &events);
        InsertEvent("g2d", "g2d_perf_update_qos", &events);
        continue;
      }

      if (category == "ion") {
        InsertEvent("kmem", "ion_alloc_buffer_start", &events);
        continue;
      }

      // Note: sched_wakeup intentionally removed (diverging from atrace), as it
      // is high-volume, but mostly redundant when sched_waking is also enabled.
      // The event can still be enabled explicitly when necessary.
      if (category == "sched") {
        InsertEvent("sched", "sched_switch", &events);
        InsertEvent("sched", "sched_waking", &events);
        InsertEvent("sched", "sched_blocked_reason", &events);
        InsertEvent("sched", "sched_cpu_hotplug", &events);
        InsertEvent("sched", "sched_pi_setprio", &events);
        InsertEvent("sched", "sched_process_exit", &events);
        AddEventGroup(table, "cgroup", &events);
        InsertEvent("cgroup", "cgroup_transfer_tasks", &events);
        InsertEvent("cgroup", "cgroup_setup_root", &events);
        InsertEvent("cgroup", "cgroup_rmdir", &events);
        InsertEvent("cgroup", "cgroup_rename", &events);
        InsertEvent("cgroup", "cgroup_remount", &events);
        InsertEvent("cgroup", "cgroup_release", &events);
        InsertEvent("cgroup", "cgroup_mkdir", &events);
        InsertEvent("cgroup", "cgroup_destroy_root", &events);
        InsertEvent("cgroup", "cgroup_attach_task", &events);
        InsertEvent("oom", "oom_score_adj_update", &events);
        InsertEvent("task", "task_rename", &events);
        InsertEvent("task", "task_newtask", &events);

        AddEventGroup(table, "systrace", &events);
        InsertEvent("systrace", "0", &events);

        AddEventGroup(table, "scm", &events);
        InsertEvent("scm", "scm_call_start", &events);
        InsertEvent("scm", "scm_call_end", &events);
        continue;
      }

      if (category == "irq") {
        AddEventGroup(table, "irq", &events);
        InsertEvent("irq", "tasklet_hi_exit", &events);
        InsertEvent("irq", "tasklet_hi_entry", &events);
        InsertEvent("irq", "tasklet_exit", &events);
        InsertEvent("irq", "tasklet_entry", &events);
        InsertEvent("irq", "softirq_raise", &events);
        InsertEvent("irq", "softirq_exit", &events);
        InsertEvent("irq", "softirq_entry", &events);
        InsertEvent("irq", "irq_handler_exit", &events);
        InsertEvent("irq", "irq_handler_entry", &events);
        AddEventGroup(table, "ipi", &events);
        InsertEvent("ipi", "ipi_raise", &events);
        InsertEvent("ipi", "ipi_exit", &events);
        InsertEvent("ipi", "ipi_entry", &events);
        continue;
      }

      if (category == "irqoff") {
        InsertEvent("preemptirq", "irq_enable", &events);
        InsertEvent("preemptirq", "irq_disable", &events);
        continue;
      }

      if (category == "preemptoff") {
        InsertEvent("preemptirq", "preempt_enable", &events);
        InsertEvent("preemptirq", "preempt_disable", &events);
        continue;
      }

      if (category == "i2c") {
        AddEventGroup(table, "i2c", &events);
        InsertEvent("i2c", "i2c_read", &events);
        InsertEvent("i2c", "i2c_write", &events);
        InsertEvent("i2c", "i2c_result", &events);
        InsertEvent("i2c", "i2c_reply", &events);
        InsertEvent("i2c", "smbus_read", &events);
        InsertEvent("i2c", "smbus_write", &events);
        InsertEvent("i2c", "smbus_result", &events);
        InsertEvent("i2c", "smbus_reply", &events);
        continue;
      }

      if (category == "freq") {
        InsertEvent("power", "cpu_frequency", &events);
        InsertEvent("power", "gpu_frequency", &events);
        InsertEvent("power", "clock_set_rate", &events);
        InsertEvent("power", "clock_disable", &events);
        InsertEvent("power", "clock_enable", &events);
        InsertEvent("clk", "clk_set_rate", &events);
        InsertEvent("clk", "clk_disable", &events);
        InsertEvent("clk", "clk_enable", &events);
        InsertEvent("power", "cpu_frequency_limits", &events);
        InsertEvent("power", "suspend_resume", &events);
        InsertEvent("cpuhp", "cpuhp_enter", &events);
        InsertEvent("cpuhp", "cpuhp_exit", &events);
        InsertEvent("cpuhp", "cpuhp_pause", &events);
        AddEventGroup(table, "msm_bus", &events);
        InsertEvent("msm_bus", "bus_update_request_end", &events);
        InsertEvent("msm_bus", "bus_update_request", &events);
        InsertEvent("msm_bus", "bus_rules_matches", &events);
        InsertEvent("msm_bus", "bus_max_votes", &events);
        InsertEvent("msm_bus", "bus_client_status", &events);
        InsertEvent("msm_bus", "bus_bke_params", &events);
        InsertEvent("msm_bus", "bus_bimc_config_limiter", &events);
        InsertEvent("msm_bus", "bus_avail_bw", &events);
        InsertEvent("msm_bus", "bus_agg_bw", &events);
        continue;
      }

      if (category == "membus") {
        AddEventGroup(table, "memory_bus", &events);
        continue;
      }

      if (category == "idle") {
        InsertEvent("power", "cpu_idle", &events);
        continue;
      }

      if (category == "disk") {
        InsertEvent("f2fs", "f2fs_sync_file_enter", &events);
        InsertEvent("f2fs", "f2fs_sync_file_exit", &events);
        InsertEvent("f2fs", "f2fs_write_begin", &events);
        InsertEvent("f2fs", "f2fs_write_end", &events);
        InsertEvent("ext4", "ext4_da_write_begin", &events);
        InsertEvent("ext4", "ext4_da_write_end", &events);
        InsertEvent("ext4", "ext4_sync_file_enter", &events);
        InsertEvent("ext4", "ext4_sync_file_exit", &events);
        InsertEvent("block", "block_rq_issue", &events);
        InsertEvent("block", "block_rq_complete", &events);
        continue;
      }

      if (category == "mmc") {
        AddEventGroup(table, "mmc", &events);
        continue;
      }

      if (category == "load") {
        AddEventGroup(table, "cpufreq_interactive", &events);
        continue;
      }

      if (category == "sync") {
        // linux kernel < 4.9
        AddEventGroup(table, "sync", &events);
        InsertEvent("sync", "sync_pt", &events);
        InsertEvent("sync", "sync_timeline", &events);
        InsertEvent("sync", "sync_wait", &events);
        // linux kernel == 4.9.x
        AddEventGroup(table, "fence", &events);
        InsertEvent("fence", "fence_annotate_wait_on", &events);
        InsertEvent("fence", "fence_destroy", &events);
        InsertEvent("fence", "fence_emit", &events);
        InsertEvent("fence", "fence_enable_signal", &events);
        InsertEvent("fence", "fence_init", &events);
        InsertEvent("fence", "fence_signaled", &events);
        InsertEvent("fence", "fence_wait_end", &events);
        InsertEvent("fence", "fence_wait_start", &events);
        // linux kernel > 4.9
        AddEventGroup(table, "dma_fence", &events);
        continue;
      }

      if (category == "workq") {
        AddEventGroup(table, "workqueue", &events);
        InsertEvent("workqueue", "workqueue_queue_work", &events);
        InsertEvent("workqueue", "workqueue_execute_start", &events);
        InsertEvent("workqueue", "workqueue_execute_end", &events);
        InsertEvent("workqueue", "workqueue_activate_work", &events);
        continue;
      }

      if (category == "memreclaim") {
        InsertEvent("vmscan", "mm_vmscan_direct_reclaim_begin", &events);
        InsertEvent("vmscan", "mm_vmscan_direct_reclaim_end", &events);
        InsertEvent("vmscan", "mm_vmscan_kswapd_wake", &events);
        InsertEvent("vmscan", "mm_vmscan_kswapd_sleep", &events);
        AddEventGroup(table, "lowmemorykiller", &events);
        InsertEvent("lowmemorykiller", "lowmemory_kill", &events);
        continue;
      }

      if (category == "regulators") {
        AddEventGroup(table, "regulator", &events);
        events.insert(
            GroupAndName("regulator", "regulator_set_voltage_complete"));
        InsertEvent("regulator", "regulator_set_voltage", &events);
        InsertEvent("regulator", "regulator_enable_delay", &events);
        InsertEvent("regulator", "regulator_enable_complete", &events);
        InsertEvent("regulator", "regulator_enable", &events);
        InsertEvent("regulator", "regulator_disable_complete", &events);
        InsertEvent("regulator", "regulator_disable", &events);
        continue;
      }

      if (category == "binder_driver") {
        InsertEvent("binder", "binder_transaction", &events);
        InsertEvent("binder", "binder_transaction_received", &events);
        InsertEvent("binder", "binder_transaction_alloc_buf", &events);
        InsertEvent("binder", "binder_set_priority", &events);
        continue;
      }

      if (category == "binder_lock") {
        InsertEvent("binder", "binder_lock", &events);
        InsertEvent("binder", "binder_locked", &events);
        InsertEvent("binder", "binder_unlock", &events);
        continue;
      }

      if (category == "pagecache") {
        AddEventGroup(table, "filemap", &events);
        events.insert(
            GroupAndName("filemap", "mm_filemap_delete_from_page_cache"));
        InsertEvent("filemap", "mm_filemap_add_to_page_cache", &events);
        InsertEvent("filemap", "filemap_set_wb_err", &events);
        InsertEvent("filemap", "file_check_and_advance_wb_err", &events);
        continue;
      }

      if (category == "memory") {
        InsertEvent("kmem", "rss_stat", &events);
        InsertEvent("kmem", "ion_heap_grow", &events);
        InsertEvent("kmem", "ion_heap_shrink", &events);
        // ion_stat supersedes ion_heap_grow / shrink for kernel 4.19+
        InsertEvent("ion", "ion_stat", &events);
        InsertEvent("mm_event", "mm_event_record", &events);
        InsertEvent("dmabuf_heap", "dma_heap_stat", &events);
        continue;
      }

      if (category == "thermal") {
        InsertEvent("thermal", "thermal_temperature", &events);
        InsertEvent("thermal", "cdev_update", &events);
        continue;
      }
    }
  }
  return events;
}

// Post-conditions:
// 1. result >= 1 (should have at least one page per CPU)
// 2. result * 4 < kMaxTotalBufferSizeKb
// 3. If input is 0 output is a good default number.
size_t ComputeCpuBufferSizeInPages(size_t requested_buffer_size_kb) {
  if (requested_buffer_size_kb == 0)
    requested_buffer_size_kb = kDefaultPerCpuBufferSizeKb;
  if (requested_buffer_size_kb > kMaxPerCpuBufferSizeKb) {
    PERFETTO_ELOG(
        "The requested ftrace buf size (%zu KB) is too big, capping to %d KB",
        requested_buffer_size_kb, kMaxPerCpuBufferSizeKb);
    requested_buffer_size_kb = kMaxPerCpuBufferSizeKb;
  }

  size_t pages = requested_buffer_size_kb / (base::kPageSize / 1024);
  if (pages == 0)
    return 1;

  return pages;
}

FtraceConfigMuxer::FtraceConfigMuxer(
    FtraceProcfs* ftrace,
    ProtoTranslationTable* table,
    std::map<std::string, std::vector<GroupAndName>> vendor_events)
    : ftrace_(ftrace),
      table_(table),
      current_state_(),
      ds_configs_(),
      vendor_events_(vendor_events) {}
FtraceConfigMuxer::~FtraceConfigMuxer() = default;

FtraceConfigId FtraceConfigMuxer::SetupConfig(const FtraceConfig& request) {
  EventFilter filter;
  bool is_ftrace_enabled = ftrace_->IsTracingEnabled();
  if (ds_configs_.empty()) {
    PERFETTO_DCHECK(active_configs_.empty());

    // If someone outside of perfetto is using ftrace give up now.
    if (is_ftrace_enabled && !IsOldAtrace()) {
      PERFETTO_ELOG("ftrace in use by non-Perfetto.");
      return 0;
    }

    // Setup ftrace, without starting it. Setting buffers can be quite slow
    // (up to hundreds of ms).
    SetupClock(request);
    SetupBufferSize(request);
  } else {
    // Did someone turn ftrace off behind our back? If so give up.
    if (!active_configs_.empty() && !is_ftrace_enabled && !IsOldAtrace()) {
      PERFETTO_ELOG("ftrace disabled by non-Perfetto.");
      return 0;
    }
  }

  std::set<GroupAndName> events = GetFtraceEvents(request, table_);

  // Vendors can provide a set of extra ftrace categories to be enabled when a
  // specific atrace category is used (e.g. "gfx" -> ["my_hw/my_custom_event",
  // "my_hw/my_special_gpu"]). Merge them with the hard coded events for each
  // categories.
  for (const std::string& category : request.atrace_categories()) {
    if (vendor_events_.count(category)) {
      for (const GroupAndName& event : vendor_events_[category]) {
        events.insert(event);
      }
    }
  }

  if (RequiresAtrace(request)) {
    if (IsOldAtrace() && !ds_configs_.empty()) {
      PERFETTO_ELOG(
          "Concurrent atrace sessions are not supported before Android P, "
          "bailing out.");
      return 0;
    }
    UpdateAtrace(request);
  }

  for (const auto& group_and_name : events) {
    const Event* event = table_->GetOrCreateEvent(group_and_name);
    if (!event) {
      PERFETTO_DLOG("Can't enable %s, event not known",
                    group_and_name.ToString().c_str());
      continue;
    }
    // Note: ftrace events are always implicitly enabled (and don't have an
    // "enable" file). So they aren't tracked by the central event filter (but
    // still need to be added to the per data source event filter to retain the
    // events during parsing).
    if (current_state_.ftrace_events.IsEventEnabled(event->ftrace_event_id) ||
        std::string("ftrace") == event->group) {
      filter.AddEnabledEvent(event->ftrace_event_id);
      continue;
    }
    if (ftrace_->EnableEvent(event->group, event->name)) {
      current_state_.ftrace_events.AddEnabledEvent(event->ftrace_event_id);
      filter.AddEnabledEvent(event->ftrace_event_id);
    } else {
      PERFETTO_DPLOG("Failed to enable %s.", group_and_name.ToString().c_str());
    }
  }

  auto compact_sched =
      CreateCompactSchedConfig(request, table_->compact_sched_format());

  std::vector<std::string> apps(request.atrace_apps());
  std::vector<std::string> categories(request.atrace_categories());
  FtraceConfigId id = ++last_id_;
  ds_configs_.emplace(
      std::piecewise_construct, std::forward_as_tuple(id),
      std::forward_as_tuple(std::move(filter), compact_sched, std::move(apps),
                            std::move(categories), request.symbolize_ksyms()));
  return id;
}

bool FtraceConfigMuxer::ActivateConfig(FtraceConfigId id) {
  if (!id || ds_configs_.count(id) == 0) {
    PERFETTO_DFATAL("Config not found");
    return false;
  }

  if (active_configs_.empty()) {
    if (ftrace_->IsTracingEnabled() && !IsOldAtrace()) {
      // If someone outside of perfetto is using ftrace give up now.
      PERFETTO_ELOG("ftrace in use by non-Perfetto.");
      return false;
    }
    if (!ftrace_->EnableTracing()) {
      PERFETTO_ELOG("Failed to enable ftrace.");
      return false;
    }
  }

  active_configs_.insert(id);
  return true;
}

bool FtraceConfigMuxer::RemoveConfig(FtraceConfigId config_id) {
  if (!config_id || !ds_configs_.erase(config_id))
    return false;
  EventFilter expected_ftrace_events;
  std::vector<std::string> expected_apps;
  std::vector<std::string> expected_categories;
  for (const auto& id_config : ds_configs_) {
    const perfetto::FtraceDataSourceConfig& config = id_config.second;
    expected_ftrace_events.EnableEventsFrom(config.event_filter);
    UnionInPlace(config.atrace_apps, &expected_apps);
    UnionInPlace(config.atrace_categories, &expected_categories);
  }
  // At this point expected_{apps,categories} contains the union of the
  // leftover configs (if any) that should be still on. However we did not
  // necessarily succeed in turning on atrace for each of those configs
  // previously so we now intersect the {apps,categories} that we *did* manage
  // to turn on with those we want on to determine the new state we should aim
  // for:
  IntersectInPlace(current_state_.atrace_apps, &expected_apps);
  IntersectInPlace(current_state_.atrace_categories, &expected_categories);
  // Work out if there is any difference between the current state and the
  // desired state: It's sufficient to compare sizes here (since we know from
  // above that expected_{apps,categories} is now a subset of
  // atrace_{apps,categories}:
  bool atrace_changed =
      (current_state_.atrace_apps.size() != expected_apps.size()) ||
      (current_state_.atrace_categories.size() != expected_categories.size());

  // Disable any events that are currently enabled, but are not in any configs
  // anymore.
  std::set<size_t> event_ids = current_state_.ftrace_events.GetEnabledEvents();
  for (size_t id : event_ids) {
    if (expected_ftrace_events.IsEventEnabled(id))
      continue;
    const Event* event = table_->GetEventById(id);
    // Any event that was enabled must exist.
    PERFETTO_DCHECK(event);
    if (ftrace_->DisableEvent(event->group, event->name))
      current_state_.ftrace_events.DisableEvent(event->ftrace_event_id);
  }

  // If there aren't any more active configs, disable ftrace.
  auto active_it = active_configs_.find(config_id);
  if (active_it != active_configs_.end()) {
    active_configs_.erase(active_it);
    if (active_configs_.empty()) {
      // This was the last active config, disable ftrace.
      if (!ftrace_->DisableTracing())
        PERFETTO_ELOG("Failed to disable ftrace.");
    }
  }

  // Even if we don't have any other active configs, we might still have idle
  // configs around. Tear down the rest of the ftrace config only if all
  // configs are removed.
  if (ds_configs_.empty()) {
    if (ftrace_->SetCpuBufferSizeInPages(1))
      current_state_.cpu_buffer_size_pages = 1;
    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
  }

  if (current_state_.atrace_on) {
    if (expected_apps.empty() && expected_categories.empty()) {
      DisableAtrace();
    } else if (atrace_changed) {
      // Update atrace to remove the no longer wanted categories/apps. For
      // some categories this won't disable them (e.g. categories that just
      // enable ftrace events) for those there is nothing we can do till the
      // last ftrace config is removed.
      if (StartAtrace(expected_apps, expected_categories)) {
        // Update current_state_ to reflect this change.
        current_state_.atrace_apps = expected_apps;
        current_state_.atrace_categories = expected_categories;
      }
    }
  }

  return true;
}

const FtraceDataSourceConfig* FtraceConfigMuxer::GetDataSourceConfig(
    FtraceConfigId id) {
  if (!ds_configs_.count(id))
    return nullptr;
  return &ds_configs_.at(id);
}

void FtraceConfigMuxer::SetupClock(const FtraceConfig&) {
  std::string current_clock = ftrace_->GetClock();
  std::set<std::string> clocks = ftrace_->AvailableClocks();

  for (size_t i = 0; i < base::ArraySize(kClocks); i++) {
    std::string clock = std::string(kClocks[i]);
    if (!clocks.count(clock))
      continue;
    if (current_clock == clock)
      break;
    ftrace_->SetClock(clock);
    current_clock = clock;
    break;
  }

  namespace pb0 = protos::pbzero;
  if (current_clock == "boot") {
    // "boot" is the default expectation on modern kernels, which is why we
    // don't have an explicit FTRACE_CLOCK_BOOT enum and leave it unset.
    // See comments in ftrace_event_bundle.proto.
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_UNSPECIFIED;
  } else if (current_clock == "global") {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_GLOBAL;
  } else if (current_clock == "local") {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_LOCAL;
  } else {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_UNKNOWN;
  }
}

void FtraceConfigMuxer::SetupBufferSize(const FtraceConfig& request) {
  size_t pages = ComputeCpuBufferSizeInPages(request.buffer_size_kb());
  ftrace_->SetCpuBufferSizeInPages(pages);
  current_state_.cpu_buffer_size_pages = pages;
}

size_t FtraceConfigMuxer::GetPerCpuBufferSizePages() {
  return current_state_.cpu_buffer_size_pages;
}

void FtraceConfigMuxer::UpdateAtrace(const FtraceConfig& request) {
  // We want to avoid poisoning current_state_.atrace_{categories, apps}
  // if for some reason these args make atrace unhappy so we stash the
  // union into temps and only update current_state_ if we successfully
  // run atrace.

  std::vector<std::string> combined_categories = request.atrace_categories();
  UnionInPlace(current_state_.atrace_categories, &combined_categories);

  std::vector<std::string> combined_apps = request.atrace_apps();
  UnionInPlace(current_state_.atrace_apps, &combined_apps);

  if (current_state_.atrace_on &&
      combined_apps.size() == current_state_.atrace_apps.size() &&
      combined_categories.size() == current_state_.atrace_categories.size()) {
    return;
  }

  if (StartAtrace(combined_apps, combined_categories)) {
    current_state_.atrace_categories = combined_categories;
    current_state_.atrace_apps = combined_apps;
    current_state_.atrace_on = true;
  }
}

// static
bool FtraceConfigMuxer::StartAtrace(
    const std::vector<std::string>& apps,
    const std::vector<std::string>& categories) {
  PERFETTO_DLOG("Update atrace config...");

  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--async_start");
  if (!IsOldAtrace())
    args.push_back("--only_userspace");

  for (const auto& category : categories)
    args.push_back(category);

  if (!apps.empty()) {
    args.push_back("-a");
    std::string arg = "";
    for (const auto& app : apps) {
      arg += app;
      arg += ",";
    }
    arg.resize(arg.size() - 1);
    args.push_back(arg);
  }

  bool result = RunAtrace(args);
  PERFETTO_DLOG("...done (%s)", result ? "success" : "fail");
  return result;
}

void FtraceConfigMuxer::DisableAtrace() {
  PERFETTO_DCHECK(current_state_.atrace_on);

  PERFETTO_DLOG("Stop atrace...");

  std::vector<std::string> args{"atrace", "--async_stop"};
  if (!IsOldAtrace())
    args.push_back("--only_userspace");
  if (RunAtrace(args)) {
    current_state_.atrace_categories.clear();
    current_state_.atrace_apps.clear();
    current_state_.atrace_on = false;
  }

  PERFETTO_DLOG("...done");
}

}  // namespace perfetto
