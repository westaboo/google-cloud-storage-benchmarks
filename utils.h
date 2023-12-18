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

#ifndef GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_UTILS_H_
#define GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_UTILS_H_

#include <chrono>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "google/cloud/options.h"
#include "google/cloud/status_or.h"

namespace google {
namespace cloud {
namespace storage {
namespace load_generator {

std::int64_t constexpr kKiB = 1024;
std::int64_t constexpr kMiB = 1024 * kKiB;
std::int64_t constexpr kGiB = 1024 * kMiB;
std::int64_t constexpr kTiB = 1024 * kGiB;

std::int64_t constexpr kKB = 1000;
std::int64_t constexpr kMB = 1000 * kKB;
std::int64_t constexpr kGB = 1000 * kMB;
std::int64_t constexpr kTB = 1000 * kGB;

// Make a random object name.
std::string MakeRandomObjectName(std::mt19937_64& gen);
std::string MakeRandomFileName(std::mt19937_64& gen);

// Make random data.
std::string MakeRandomData(std::mt19937_64& gen, std::size_t desired_size);

/// Parse a string as a byte size, with support for unit suffixes.
std::int64_t ParseSize(std::string const& val);

/**
 * Parse a string as a byte size, with support for unit suffixes.
 *
 * The size must be small enough for an in-memory buffer.
 */
std::size_t ParseBufferSize(std::string const& val);

/// Parse a string as a duration with support for hours (h), minutes (m), or
/// second (s) suffixes.
std::chrono::seconds ParseDuration(std::string const& val);

/// Parse a string as a boolean, returning a not-present value if the string is
/// empty.
std::optional<bool> ParseBoolean(std::string const& val);

/// Defines a command-line option.
struct OptionDescriptor {
  using OptionParser = std::function<void(std::string const&)>;

  std::string option;
  std::string help;
  OptionParser parser;
};

/// Build the `Usage` string from a list of command-line option descriptions.
std::string BuildUsage(std::vector<OptionDescriptor> const& desc,
                       std::string const& command_path);

/**
 * Parse @p argv using the descriptions in @p desc, return unparsed arguments.
 */
std::vector<std::string> OptionsParse(std::vector<OptionDescriptor> const& desc,
                                      std::vector<std::string> argv);

/// Format a buffer size in human readable form.
std::string FormatSize(std::uintmax_t size);

// Technically gRPC is not a different API, just the JSON API over a different
// protocol, but it is easier to represent it as such in the benchmark.
enum class ApiName {
  kApiJson,
  kApiGrpc,
};
char const* ToString(ApiName api);

StatusOr<ApiName> ParseApiName(std::string const& val);

template <typename Rep, typename Period>
std::string FormatBandwidthGbPerSecond(
    std::uintmax_t bytes, std::chrono::duration<Rep, Period> elapsed) {
  using ns = ::std::chrono::nanoseconds;
  auto const elapsed_ns = std::chrono::duration_cast<ns>(elapsed);
  if (elapsed_ns == ns(0)) return "NaN";

  auto const bandwidth =
      8 * static_cast<double>(bytes) / static_cast<double>(elapsed_ns.count());
  std::ostringstream os;
  os << std::fixed << std::setprecision(2) << bandwidth;
  return std::move(os).str();
}

template <typename Rep, typename Period>
std::string FormatQueriesPerSecond(size_t queries,
                                   std::chrono::duration<Rep, Period> elapsed) {
  using ns = ::std::chrono::nanoseconds;
  auto const elapsed_ns = std::chrono::duration_cast<ns>(elapsed);
  if (elapsed_ns == ns(0)) return "NaN";

  auto const qps = static_cast<double>(queries) /
                   static_cast<double>(elapsed_ns.count()) * 1000000000;
  std::ostringstream os;
  os << std::fixed << std::setprecision(2) << qps;
  return std::move(os).str();
}

// Print any well-known options.
void PrintOptions(std::ostream& os, std::string const& prefix,
                  Options const& options);

// Format a timestamp
std::string FormatTimestamp(std::chrono::system_clock::time_point tp);

// The current time, formatted
inline std::string CurrentTime() {
  return FormatTimestamp(std::chrono::system_clock::now());
}

}  // namespace load_generator
}  // namespace storage
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_STORAGE_LOAD_GENERATOR_UTILS_H_

