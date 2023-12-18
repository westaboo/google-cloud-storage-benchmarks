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

#include "options.h"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_join.h"
#include "google/cloud/common_options.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/options.h"
#include "google/cloud/status.h"
#include "google/cloud/status_or.h"
#include "google/cloud/storage/options.h"
#include "options.pb.h"
#include "src/google/protobuf/text_format.h"
#include "utils.h"

namespace google {
namespace cloud {
namespace storage {
namespace load_generator {

namespace {

using ::google::cloud::storage::ConnectionPoolSizeOption;
using ::google::cloud::storage::DownloadStallMinimumRateOption;
using ::google::cloud::storage::RestEndpointOption;
using ::google::cloud::storage::TransferStallMinimumRateOption;
using ::google::cloud::storage::TransferStallTimeoutOption;
using ::google::cloud::storage_experimental::HttpVersionOption;
using ::google::protobuf::TextFormat;

}  // namespace

google::cloud::Options GetStorageClientOptions(
    const WorkerOptions& worker_options) {
  Options storage_client_options;
  if (worker_options.storage_client_options().has_rest_http_version()) {
    storage_client_options.set<HttpVersionOption>(
        worker_options.storage_client_options().rest_http_version());
  }
  storage_client_options.set<RestEndpointOption>(
      worker_options.storage_client_options().rest_endpoint());
  storage_client_options.set<EndpointOption>(
      worker_options.storage_client_options().grpc_endpoint());
  if (worker_options.storage_client_options().has_grpc_channel_count()) {
    storage_client_options.set<GrpcNumChannelsOption>(
        worker_options.storage_client_options().grpc_channel_count());
  }
  if (worker_options.storage_client_options().has_grpc_background_threads()) {
    storage_client_options.set<GrpcBackgroundThreadPoolSizeOption>(
        worker_options.storage_client_options().grpc_background_threads());
  }
  if (worker_options.storage_client_options().has_rest_pool_size()) {
    storage_client_options.set<ConnectionPoolSizeOption>(
        worker_options.storage_client_options().rest_pool_size());
  }
  if (worker_options.storage_client_options().has_request_stall_timeout()) {
    storage_client_options.set<TransferStallTimeoutOption>(ParseDuration(
        worker_options.storage_client_options().request_stall_timeout()));
  }
  if (worker_options.storage_client_options().has_request_stall_timeout()) {
    storage_client_options.set<DownloadStallTimeoutOption>(ParseDuration(
        worker_options.storage_client_options().request_stall_timeout()));
  }
  if (worker_options.storage_client_options()
          .has_request_stall_minimum_rate()) {
    storage_client_options.set<TransferStallMinimumRateOption>(
        static_cast<std::int32_t>(worker_options.storage_client_options()
                                      .request_stall_minimum_rate()));
  }
  if (worker_options.storage_client_options()
          .has_request_stall_minimum_rate()) {
    storage_client_options.set<DownloadStallMinimumRateOption>(
        static_cast<std::int32_t>(worker_options.storage_client_options()
                                      .request_stall_minimum_rate()));
  }
  return storage_client_options;
}

Status ValidateOptions(std::string const& usage,
                       const WorkerOptions& worker_options) {
  auto make_status = [](std::ostringstream& os) {
    return Status{StatusCode::kInvalidArgument, std::move(os).str()};
  };

  if (!worker_options.has_operation() ||
      worker_options.operation() == WorkerOptions::OPERATION_TYPE_UNSPECIFIED) {
    std::ostringstream os;
    os << "Missing operation option\n" << usage << "\n";
    return make_status(os);
  }
  if (!worker_options.has_bucket_name()) {
    std::ostringstream os;
    os << "Missing bucket-name option\n" << usage << "\n";
    return make_status(os);
  }
  if (worker_options.thread_count() <= 0) {
    std::ostringstream os;
    os << "Invalid number of threads (" << worker_options.thread_count()
       << "), check your thread_count option\n";
    return make_status(os);
  }
  if (worker_options.storage_client_options().grpc_channel_count() < 0) {
    std::ostringstream os;
    os << "Invalid number of gRPC channels ("
       << worker_options.storage_client_options().grpc_channel_count()
       << "), check your grpc_channel_count option\n";
    return make_status(os);
  }

  return Status{StatusCode::kOk, "OK"};
}

google::cloud::StatusOr<WorkerOptions> ParseWorkerOptions(
    std::vector<std::string> const& argv, std::string const& description) {
  WorkerOptions worker_options;
  bool wants_help = false;
  bool wants_description = false;

  std::vector<OptionDescriptor> desc{
      {"--help", "print usage information",
       [&wants_help](std::string const&) { wants_help = true; }},
      {"--description", "print benchmark description",
       [&wants_description](std::string const&) { wants_description = true; }},
      {"--options_proto",
       "Base64 encoded google.cloud.storage_benchmarks.WorkerOptions for "
       "this load generator. ",
       [&worker_options](std::string const& val) {
         std::string options_textpb;
         if (!absl::Base64Unescape(val, &options_textpb)) {
           std::cerr << "Failed to base64 decode --options_proto: " << val
                     << std::endl;
         }
         if (!TextFormat::ParseFromString(options_textpb, &worker_options)) {
           std::cerr << "Failed to parse --options_proto: " << options_textpb
                     << std::endl;
         }
       }},
  };
  auto usage = BuildUsage(desc, argv[0]);

  auto unparsed = OptionsParse(desc, argv);
  if (wants_help) {
    std::cout << usage << "\n";
    worker_options.set_exit_after_parse(true);
    return worker_options;
  }
  if (wants_description) {
    std::cout << description << "\n";
    worker_options.set_exit_after_parse(true);
    return worker_options;
  }
  if (unparsed.size() != 1) {
    std::ostringstream os;
    os << "Unknown arguments or options: "
       << absl::StrJoin(std::next(unparsed.begin()), unparsed.end(), ", ")
       << "\n"
       << usage << "\n";
    return Status{StatusCode::kInvalidArgument, std::move(os).str()};
  }

  auto status = ValidateOptions(usage, worker_options);
  if (!status.ok()) {
    return status;
  }

  return worker_options;
}

}  // namespace load_generator
}  // namespace storage
}  // namespace cloud
}  // namespace google

