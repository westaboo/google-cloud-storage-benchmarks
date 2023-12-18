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

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "google/cloud/common_options.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/internal/throw_delegate.h"
#include "google/cloud/options.h"
#include "google/cloud/status.h"
#include "google/cloud/status_or.h"
#include "google/cloud/storage/options.h"

namespace google {
namespace cloud {
namespace storage {
namespace load_generator {

namespace {

std::string Sample(std::mt19937_64& gen, size_t n,
                   std::string const& population) {
  std::uniform_int_distribution<std::size_t> rd(0, population.size() - 1);

  std::string result(n, '0');
  std::generate(result.begin(), result.end(),
                [&rd, &gen, &population]() { return population[rd(gen)]; });
  return result;
}

std::string Basename(std::string const& path) {
  // With C++17 we would use `std::filesystem::path`, until then use
  // `find_last_of()`
#if _WIN32
  return path.substr(path.find_last_of("\\/") + 1);
#else
  return path.substr(path.find_last_of('/') + 1);
#endif  // _WIN32
}

}  // namespace

namespace gcs = ::google::cloud::storage;

std::string MakeRandomObjectName(std::mt19937_64& gen) {
  // GCS accepts object name up to 1024 characters, but 128 seems long enough to
  // avoid collisions.
  auto constexpr kObjectNameLength = 128UL;
  return Sample(gen, kObjectNameLength,
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789");
}

std::string MakeRandomFileName(std::mt19937_64& gen) {
  // All the operating systems we support handle filenames with 28 characters,
  // they may support much longer names in fact, but 28 is good enough for our
  // purposes.
  auto constexpr kFilenameLength = 28UL;
  return Sample(gen, kFilenameLength,
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789") +
         ".txt";
}

std::string MakeRandomData(std::mt19937_64& gen, std::size_t desired_size) {
  std::string result;
  result.reserve(desired_size);

  // Create lines of 128 characters to start with, we can fill the remaining
  // characters at the end.
  constexpr int kLineSize = 128UL;
  auto gen_random_line = [&gen](std::size_t count) -> std::string {
    if (count == 0) {
      return "";
    }
    return Sample(gen, count - 1,
                  "abcdefghijklmnopqrstuvwxyz"
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                  "0123456789"
                  " - _ : /") +
           "\n";
  };
  while (result.size() + kLineSize < desired_size) {
    result += gen_random_line(kLineSize);
  }
  if (result.size() < desired_size) {
    result += gen_random_line(desired_size - result.size());
  }

  return result;
}

// This parser does not validate the input fully, but it is good enough for our
// purposes.
std::int64_t ParseSize(std::string const& val) {
  struct Match {
    char const* suffix;
    std::int64_t multiplier;
  } matches[] = {
      {"TiB", kTiB}, {"GiB", kGiB}, {"MiB", kMiB}, {"KiB", kKiB},
      {"TB", kTB},   {"GB", kGB},   {"MB", kMB},   {"KB", kKB},
  };
  auto const s = std::stol(val);
  for (auto const& m : matches) {
    if (absl::EndsWith(val, m.suffix)) return s * m.multiplier;
  }
  return s;
}

std::size_t ParseBufferSize(std::string const& val) {
  auto const s = ParseSize(val);
  if (s < 0 || static_cast<std::uint64_t>(s) >
                   (std::numeric_limits<std::size_t>::max)()) {
    google::cloud::internal::ThrowRangeError(
        "invalid range in ParseBufferSize");
  }
  return static_cast<std::size_t>(s);
}

std::chrono::seconds ParseDuration(std::string const& val) {
  absl::Duration d;
  if (!absl::ParseDuration(val, &d)) {
    google::cloud::internal::ThrowInvalidArgument("invalid duration: " + val);
  }
  return absl::ToChronoSeconds(d);
}

std::optional<bool> ParseBoolean(std::string const& val) {
  auto lower = val;
  std::transform(
      lower.begin(), lower.end(), lower.begin(),
      [](unsigned char x) { return static_cast<char>(std::tolower(x)); });
  if (lower == "true") return true;
  if (lower == "false") return false;
  return {};
}

std::string BuildUsage(std::vector<OptionDescriptor> const& desc,
                       std::string const& command_path) {
  std::ostringstream os;
  os << "Usage: " << Basename(command_path) << " [options]\n";
  for (auto const& d : desc) {
    os << "    " << d.option << ": " << d.help << "\n";
  }
  return std::move(os).str();
}

std::vector<std::string> OptionsParse(std::vector<OptionDescriptor> const& desc,
                                      std::vector<std::string> argv) {
  if (argv.empty()) {
    return argv;
  }

  auto next_arg = argv.begin() + 1;
  while (next_arg != argv.end()) {
    std::string const& argument = *next_arg;

    // Try to match `argument` against the options in `desc`
    bool matched = false;
    for (auto const& d : desc) {
      if (!absl::StartsWith(argument, d.option)) {
        // Not a match, keep searching
        continue;
      }
      std::string val = argument.substr(d.option.size());
      if (!val.empty() && val[0] != '=') {
        // Matched a prefix of an option, keep searching.
        continue;
      }
      if (!val.empty()) {
        // The first character must be '=', remove it too.
        val.erase(val.begin());
      }
      d.parser(val);
      // This is a match, consume the argument and stop the search.
      matched = true;
      break;
    }
    // If next_arg is matched against any option erase it, otherwise skip it.
    next_arg = matched ? argv.erase(next_arg) : next_arg + 1;
  }
  return argv;
}

std::string FormatSize(std::uintmax_t size) {
  struct {
    std::uintmax_t limit;
    std::uintmax_t resolution;
    char const* name;
  } ranges[] = {
      {kKiB, 1, "B"},
      {kMiB, kKiB, "KiB"},
      {kGiB, kMiB, "MiB"},
      {kTiB, kGiB, "GiB"},
  };
  std::uintmax_t resolution = kTiB;
  char const* name = "TiB";
  for (auto const& r : ranges) {
    if (size < r.limit) {
      resolution = r.resolution;
      name = r.name;
      break;
    }
  }
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(1);
  os << (static_cast<double>(size) / static_cast<double>(resolution)) << name;
  return os.str();
}

char const* ToString(ApiName api) {
  switch (api) {
    case ApiName::kApiJson:
      return "JSON";
    case ApiName::kApiGrpc:
      return "GRPC";
  }
  return "";
}

StatusOr<ApiName> ParseApiName(std::string const& val) {
  for (auto a : {ApiName::kApiJson, ApiName::kApiGrpc}) {
    if (val == ToString(a)) return a;
  }
  return Status{StatusCode::kInvalidArgument, "unknown ApiName " + val};
}

void PrintOptions(std::ostream& os, std::string const& prefix,
                  Options const& options) {
  if (options.has<GrpcBackgroundThreadPoolSizeOption>()) {
    os << "\n# " << prefix << " Grpc Background Threads: "
       << options.get<GrpcBackgroundThreadPoolSizeOption>();
  }
  if (options.has<GrpcNumChannelsOption>()) {
    os << "\n# " << prefix
       << " gRPC Channel Count: " << options.get<GrpcNumChannelsOption>();
  }
  if (options.has<EndpointOption>()) {
    os << "\n# " << prefix
       << " Grpc Endpoint: " << options.get<EndpointOption>();
  }
  if (options.has<AuthorityOption>()) {
    os << "\n# " << prefix << " Authority: " << options.get<AuthorityOption>();
  }
  if (options.has<gcs::ConnectionPoolSizeOption>()) {
    os << "\n# " << prefix << " REST Connection Pool Size: "
       << options.get<gcs::ConnectionPoolSizeOption>();
  }
  if (options.has<gcs::RestEndpointOption>()) {
    os << "\n# " << prefix
       << " REST Endpoint: " << options.get<gcs::RestEndpointOption>();
  }
  if (options.has<gcs::TransferStallTimeoutOption>()) {
    os << "\n# " << prefix << " Transfer Stall Timeout: "
       << absl::FormatDuration(
              absl::FromChrono(options.get<gcs::TransferStallTimeoutOption>()));
  }
  if (options.has<gcs::TransferStallMinimumRateOption>()) {
    os << "\n# " << prefix << " Transfer Stall Minimum Rate: "
       << FormatSize(static_cast<std::uint64_t>(
              options.get<gcs::TransferStallMinimumRateOption>()));
  }
  if (options.has<gcs::DownloadStallTimeoutOption>()) {
    os << "\n# " << prefix << " Download Stall Timeout: "
       << absl::FormatDuration(
              absl::FromChrono(options.get<gcs::DownloadStallTimeoutOption>()));
  }
  if (options.has<gcs::DownloadStallMinimumRateOption>()) {
    os << "\n# " << prefix << " Download Stall Minimum Rate: "
       << FormatSize(static_cast<std::uint64_t>(
              options.get<gcs::DownloadStallMinimumRateOption>()));
  }

  if (options.has<google::cloud::storage::internal::TargetApiVersionOption>()) {
    os << "\n# " << prefix << " Api Version Path: "
       << options
              .has<google::cloud::storage::internal::TargetApiVersionOption>();
  }
}

// Format a timestamp
std::string FormatTimestamp(std::chrono::system_clock::time_point tp) {
  auto constexpr kFormat = "%E4Y-%m-%dT%H:%M:%E*SZ";
  auto const t = absl::FromChrono(tp);
  return absl::FormatTime(kFormat, t, absl::UTCTimeZone());
}

}  // namespace load_generator
}  // namespace storage
}  // namespace cloud
}  // namespace google

