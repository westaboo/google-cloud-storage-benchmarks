// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_TIMER_H_
#define GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_TIMER_H_

#include <chrono>  // NOLINT
#include <string>
#if GOOGLE_CLOUD_CPP_HAVE_GETRUSAGE
#include <sys/resource.h>
#endif  // GOOGLE_CLOUD_CPP_HAVE_GETRUSAGE

namespace google {
namespace cloud {
namespace storage {
namespace load_generator {

class Timer {
 public:
  struct Snapshot {
    std::chrono::microseconds elapsed_time;
    std::chrono::microseconds cpu_time;
  };
  static Timer PerThread() { return Timer(CpuAccounting::kPerThread); }
  static Timer PerProcess() { return Timer(CpuAccounting::kPerProcess); }

  Snapshot Sample() const;
  std::string Annotations() const;

  static bool SupportsPerThreadUsage();

 private:
  enum class CpuAccounting {
    kPerThread,
    kPerProcess,
  };

  explicit Timer(CpuAccounting cpu_accounting);

  int RUsageWho() const;

  CpuAccounting accounting_;
  std::chrono::steady_clock::time_point start_;
#if GOOGLE_CLOUD_CPP_HAVE_GETRUSAGE
  struct rusage start_usage_;
#endif  // GOOGLE_CLOUD_CPP_HAVE_GETRUSAGE
};

}  // namespace load_generator
}  // namespace storage
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_TIMER_H_

