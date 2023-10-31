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

#include "options.h"

#include "absl/strings/escaping.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/internal/absl_str_join_quiet.h"
#include "src/google/protobuf/text_format.h"
#include <iterator>
#include <sstream>

namespace google {
namespace cloud {
namespace storage_load_generation {

namespace {

namespace gcs = ::google::cloud::storage;
namespace gcs_ex = ::google::cloud::storage_experimental;
namespace gcs_bm = ::google::cloud::storage_benchmarks;
using ::google::cloud::testing_util::OptionDescriptor;
using ::google::protobuf::TextFormat;

}  // namespace

google::cloud::Options GetStorageClientOptions(
    const WorkloadOptions& workload_options) {
  Options storage_client_options;
  if (workload_options.storage_client_options().has_rest_http_version()) {
    storage_client_options.set<gcs_ex::HttpVersionOption>(
        workload_options.storage_client_options().rest_http_version());
  }
  if (workload_options.storage_client_options().has_rest_endpoint()) {
    storage_client_options.set<gcs::RestEndpointOption>(
        workload_options.storage_client_options().rest_endpoint());
  }
  if (workload_options.storage_client_options().has_grpc_endpoint()) {
    storage_client_options.set<EndpointOption>(
        workload_options.storage_client_options().grpc_endpoint());
  }
  if (workload_options.storage_client_options().has_grpc_channel_count()) {
    storage_client_options.set<GrpcNumChannelsOption>(
        workload_options.storage_client_options().grpc_channel_count());
  }
  if (workload_options.storage_client_options().has_grpc_background_threads()) {
    storage_client_options.set<GrpcBackgroundThreadPoolSizeOption>(
        workload_options.storage_client_options().grpc_background_threads());
  }
  if (workload_options.storage_client_options().has_rest_pool_size()) {
    storage_client_options.set<gcs::ConnectionPoolSizeOption>(
        workload_options.storage_client_options().rest_pool_size());
  }
  if (workload_options.storage_client_options().has_request_stall_timeout()) {
    storage_client_options.set<gcs::TransferStallTimeoutOption>(
        gcs_bm::ParseDuration(
            workload_options.storage_client_options().request_stall_timeout()));
  }
  if (workload_options.storage_client_options().has_request_stall_timeout()) {
    storage_client_options.set<gcs::DownloadStallTimeoutOption>(
        gcs_bm::ParseDuration(
            workload_options.storage_client_options().request_stall_timeout()));
  }
  if (workload_options.storage_client_options()
          .has_request_stall_minimum_rate()) {
    storage_client_options.set<gcs::TransferStallMinimumRateOption>(
        workload_options.storage_client_options().request_stall_minimum_rate());
  }
  if (workload_options.storage_client_options()
          .has_request_stall_minimum_rate()) {
    storage_client_options.set<gcs::DownloadStallMinimumRateOption>(
        workload_options.storage_client_options().request_stall_minimum_rate());
  }
  return storage_client_options;
}

void ParseOperationSpecificOptions(const WorkloadOptions& options_proto,
                                   Options* out_client_options) {}

Status ValidateOptions(std::string const& usage,
                       const WorkloadOptions& workload_options) {
  auto make_status = [](std::ostringstream& os) {
    return Status{StatusCode::kInvalidArgument, std::move(os).str()};
  };

  if (!workload_options.has_operation() ||
      workload_options.operation() ==
          WorkloadOptions::OPERATION_TYPE_UNSPECIFIED) {
    std::ostringstream os;
    os << "Missing operation option\n" << usage << "\n";
    return make_status(os);
  }
  if (!workload_options.has_bucket_name()) {
    std::ostringstream os;
    os << "Missing bucket-name option\n" << usage << "\n";
    return make_status(os);
  }
  if (workload_options.thread_count() <= 0) {
    std::ostringstream os;
    os << "Invalid number of threads (" << workload_options.thread_count()
       << "), check your thread_count option\n";
    return make_status(os);
  }
  if (workload_options.storage_client_options().grpc_channel_count() < 0) {
    std::ostringstream os;
    os << "Invalid number of gRPC channels ("
       << workload_options.storage_client_options().grpc_channel_count()
       << "), check your grpc_channel_count option\n";
    return make_status(os);
  }

  return Status{StatusCode::kOk, "OK"};
}

google::cloud::StatusOr<WorkloadOptions> ParseWorkloadOptions(
    std::vector<std::string> const& argv, std::string const& description) {
  WorkloadOptions workload_options;
  bool wants_help = false;
  bool wants_description = false;

  std::vector<OptionDescriptor> desc{
      {"--help", "print usage information",
       [&wants_help](std::string const&) { wants_help = true; }},
      {"--description", "print benchmark description",
       [&wants_description](std::string const&) { wants_description = true; }},
      {"--options_proto",
       "Base64 encoded google.cloud.storage_benchmarks.WorkloadOptions for "
       "this "
       "load generator. ",
       [&workload_options](std::string const& val) {
         std::string options_textpb;
         if (!absl::Base64Unescape(val, &options_textpb)) {
           std::cerr << "Failed to base64 decode --options_proto: " << val
                     << std::endl;
         }
         if (!TextFormat::ParseFromString(options_textpb, &workload_options)) {
           std::cerr << "Failed to parse --options_proto: " << options_textpb
                     << std::endl;
         }
       }},
  };
  auto usage = BuildUsage(desc, argv[0]);

  auto unparsed = OptionsParse(desc, argv);
  if (wants_help) {
    std::cout << usage << "\n";
    workload_options.set_exit_after_parse(true);
    return workload_options;
  }
  if (wants_description) {
    std::cout << description << "\n";
    workload_options.set_exit_after_parse(true);
    return workload_options;
  }
  if (unparsed.size() != 1) {
    std::ostringstream os;
    os << "Unknown arguments or options: "
       << absl::StrJoin(std::next(unparsed.begin()), unparsed.end(), ", ")
       << "\n"
       << usage << "\n";
    return Status{StatusCode::kInvalidArgument, std::move(os).str()};
  }

  auto status = ValidateOptions(usage, workload_options);
  if (!status.ok()) {
    return status;
  }

  return workload_options;
}

}  // namespace storage_load_generation
}  // namespace cloud
}  // namespace google

