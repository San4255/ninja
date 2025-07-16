// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "build.h"
#include "jobserver.h"
#include "limits.h"
#include "subprocess.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <sys/sysinfo.h>
#include <fstream>

struct RealCommandRunner : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config,
                             Jobserver::Client* jobserver)
      : config_(config), jobserver_(jobserver) {}
  size_t CanRunMore() const override;
  bool StartCommand(Edge* edge) override;
  bool WaitForCommand(Result* result) override;
  std::vector<Edge*> GetActiveEdges() override;
  void Abort() override;

  void ClearJobTokens() {
    if (jobserver_) {
      for (Edge* edge : GetActiveEdges()) {
        jobserver_->Release(std::move(edge->job_slot_));
      }
    }
  }

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  Jobserver::Client* jobserver_ = nullptr;
  std::map<const Subprocess*, Edge*> subproc_to_edge_;
};

std::vector<Edge*> RealCommandRunner::GetActiveEdges() {
  std::vector<Edge*> edges;
  for (std::map<const Subprocess*, Edge*>::iterator e =
           subproc_to_edge_.begin();
       e != subproc_to_edge_.end(); ++e)
    edges.push_back(e->second);
  return edges;
}

void RealCommandRunner::Abort() {
  ClearJobTokens();
  subprocs_.Clear();
}


size_t RealCommandRunner::CanRunMore() const {
  // Count both running and finished to know how many slots are in use
  size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();

  // Start with parallelism limit
  int64_t capacity = config_.parallelism - static_cast<int>(subproc_number);

  // If jobserver tokens are in use, let token acquisition do the throttling
  if (jobserver_) {
    capacity = INT_MAX;
  }

  // Enforce load everage limit
  if (config_.max_load_average > 0.0f) {
    double load = GetLoadAverage();
    int load_capacity =
        static_cast<int>(config_.max_load_average - load);
    if (load_capacity < capacity) {
      capacity = load_capacity;
    }
  }

  // Absolute system-RAM throttle (-m <MB>)
 if (config_.max_memory_bytes > 0) {
  struct sysinfo si;
  sysinfo(&si);
  // how many bytes are already in use
  uint64_t used_bytes   = si.totalram - si.freeram;
  // your threshold in bytes
  uint64_t thresh_bytes = config_.max_memory_bytes;

  // once used RAM ≥ threshold, stop launching new jobs
  if (used_bytes >= thresh_bytes) {
    capacity = 0;
  }
 }

  // Throttle based on cgroup memory usage (-M)
  if (config_.max_cg_mem_usage > 0.0f) {
   double cg_mem = GetCgroupMemoryUsage();

  // DEBUG: show cgroup usage & capacity before throttling
  fprintf(stderr,
          "[ninja] cg=%.2f thresh=%.2f cap_before=%zu\n",
          cg_mem, config_.max_cg_mem_usage, capacity);

  if (cg_mem >= config_.max_cg_mem_usage) {
    capacity = 0;
  } else {
    double headroom =
        (config_.max_cg_mem_usage - cg_mem)
        / config_.max_cg_mem_usage;
    int mem_capacity = static_cast<int>(headroom * capacity);
    if (mem_capacity < capacity)
      capacity = mem_capacity;
  }

  // DEBUG: show capacity after throttling
  fprintf(stderr, "[ninja] cap_after=%zu\n", capacity);
}

  // Never negative
  if (capacity < 0) {
    capacity = 0;
  }

  // If nothing can run but no jobs are active, force at least one to make progress
  if (capacity == 0 && subprocs_.running_.empty()) {
    capacity = 1;
  }

    // Delay starting each job by --delay ms
   if (capacity > 0 && config_.start_delay_ms > 0) {
     // DEBUG: print before sleeping
     fprintf(stderr,
             "[ninja] delaying next job by %u ms (capacity=%zu)\n",
             config_.start_delay_ms,
             capacity);
     usleep(config_.start_delay_ms * 1000);
   }
  return static_cast<size_t>(capacity);
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  std::string command = edge->EvaluateCommand();
  Subprocess* subproc = subprocs_.Add(command, edge->use_console());
  if (!subproc)
    return false;
  subproc_to_edge_.insert(std::make_pair(subproc, edge));

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == NULL) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
  result->output = subproc->GetOutput();

  std::map<const Subprocess*, Edge*>::iterator e =
      subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

CommandRunner* CommandRunner::factory(const BuildConfig& config,
                                      Jobserver::Client* jobserver) {
  return new RealCommandRunner(config, jobserver);
}
