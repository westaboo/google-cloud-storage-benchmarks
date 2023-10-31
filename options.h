// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_STORAGE_BENCHMARKS_AGGREGATE_UPLOAD_THROUGHPUT_OPTIONS_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_STORAGE_BENCHMARKS_AGGREGATE_UPLOAD_THROUGHPUT_OPTIONS_H

#include "benchmark_utils.h"
#include "options.pb.h"

#include <cstdint>
#include <string>
#include <vector>

namespace google {
namespace cloud {
namespace storage_load_generation {

google::cloud::StatusOr<WorkloadOptions> ParseWorkloadOptions(
    std::vector<std::string> const& argv, std::string const& description);

google::cloud::Options GetStorageClientOptions(const WorkloadOptions& options);

}  // namespace storage_load_generation
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_STORAGE_BENCHMARKS_AGGREGATE_UPLOAD_THROUGHPUT_OPTIONS_H
