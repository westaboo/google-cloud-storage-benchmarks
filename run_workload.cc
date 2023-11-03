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
#include "benchmark_utils.h"

#include "google/cloud/storage/client.h"
#include "google/cloud/storage/grpc_plugin.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/internal/build_info.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/random.h"
#include "google/cloud/log.h"
#include "google/cloud/options.h"
#include "google/cloud/testing_util/command_line_parsing.h"
#include "google/cloud/testing_util/timer.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"

namespace {

namespace gcs = ::google::cloud::storage;
namespace gcs_bm = ::google::cloud::storage_benchmarks;
namespace gcs_load = ::google::cloud::storage_load_generation;

using gcs_bm::FormatBandwidthGbPerSecond;
using gcs_bm::FormatQueriesPerSecond;
using gcs_bm::FormatTimestamp;
using ::google::cloud::storage_load_generation::WorkloadOptions;
using ::google::cloud::testing_util::FormatSize;
using ::google::cloud::testing_util::Timer;
using Counters = std::map<std::string, std::int64_t>;

char const kDescription[] =
    R"""(A load generation tool for Google Cloud Storage.

This benchmark repeatedly performs GCS operations, and reports the time taken
for each operation.

The benchmark uses multiple threads to perform operations, expecting higher
throughput as threads are added. The benchmark runs multiple iterations of the
same workload. After each iteration it prints the latency for each request,
with arbitrary annotations describing the library configuration (API, buffer
sizes, the iteration number), as well as arbitrary labels provided by the
application, and the overall results for the iteration ("denormalized" to
simplify any external scripts used in analysis).

The data for each object is pre-generated and used by all threads, and consist
of a repeating block of N lines with random ASCII characters. The size of this
block is configurable in the command-line. We recommend using multiples of
256KiB for this block size.
)""";

struct TaskConfig {
  gcs::Client client;
  std::seed_seq::result_type seed;
};

std::string ExtractPeer(
    const std::multimap<std::string, std::string>& headers) {
  auto p = headers.find(":grpc-context-peer");
  if (p == headers.end()) {
    p = headers.find(":curl-peer");
  }
  return p == headers.end() ? std::string{"[peer-unknown]"} : p->second;
}

std::string ExtractUploadId(std::string v) {
  auto constexpr kRestField = "upload_id=";
  auto const pos = v.find(kRestField);
  if (pos == std::string::npos) return v;
  return v.substr(pos + std::strlen(kRestField));
}

google::cloud::StatusOr<WorkloadOptions> ParseArgs(int argc, char* argv[]) {
  auto workload_options =
      gcs_load::ParseWorkloadOptions({argv, argv + argc}, kDescription);
  if (!workload_options) return workload_options;
  return workload_options;
}

class DownloadHelper {
 public:
  struct DownloadDetail {
    int iteration;
    std::chrono::system_clock::time_point start_time;
    std::string peer;
    std::uint64_t bytes_downloaded;
    std::chrono::microseconds elapsed_time;
    google::cloud::Status status;
  };

  struct DownloadTaskResult {
    std::uint64_t bytes_downloaded = 0;
    std::vector<DownloadDetail> details;
    Counters counters;
  };

  DownloadHelper(const WorkloadOptions& workload_options,
                 std::vector<gcs::ObjectMetadata> objects)
      : workload_options_(workload_options),
        objects_(std::move(objects)),
        remaining_requests_(workload_options.request_count()) {}

  static DownloadDetail DownloadOneObject(
      gcs::Client& client, std::mt19937_64& generator,
      const WorkloadOptions& benchmark_options,
      const gcs::ObjectMetadata& object) {
    using clock = std::chrono::steady_clock;
    using std::chrono::duration_cast;
    using std::chrono::microseconds;

    int64_t read_size = benchmark_options.download_options().read_size();
    size_t read_buffer_size =
        benchmark_options.download_options().read_buffer_size();

    std::vector<char> buffer(read_buffer_size);
    auto const buffer_size = static_cast<std::streamsize>(buffer.size());
    auto const object_start = clock::now();
    auto const start = std::chrono::system_clock::now();
    auto object_bytes = std::uint64_t{0};
    auto const object_size = static_cast<std::int64_t>(object.size());
    auto range = gcs::ReadRange();
    if (read_size != 0 && read_size < object_size) {
      auto read_start = std::uniform_int_distribution<std::int64_t>(
          0, object_size - read_size);
      range = gcs::ReadRange(read_start(generator), read_size);
    }
    auto stream =
        client.ReadObject(object.bucket(), object.name(),
                          gcs::Generation(object.generation()), range);
    while (stream.read(buffer.data(), buffer_size)) {
      object_bytes += stream.gcount();
    }
    stream.Close();
    // Flush the logs, if any.
    if (stream.bad()) google::cloud::LogSink::Instance().Flush();
    auto const object_elapsed =
        duration_cast<microseconds>(clock::now() - object_start);
    auto p = stream.headers().find(":grpc-context-peer");
    if (p == stream.headers().end()) {
      p = stream.headers().find(":curl-peer");
    }
    const auto& peer =
        p == stream.headers().end() ? std::string{"unknown"} : p->second;
    return DownloadDetail{.start_time = start,
                          .peer = peer,
                          .bytes_downloaded = object_bytes,
                          .elapsed_time = object_elapsed,
                          .status = stream.status()};
  }

  DownloadTaskResult DownloadTask(const TaskConfig& config) {
    auto client = config.client;
    auto generator = std::mt19937_64(config.seed);

    DownloadTaskResult result;
    while (true) {
      std::unique_lock<std::mutex> lk(mu_);
      if (remaining_requests_ <= 0) break;
      const gcs::ObjectMetadata& object =
          objects_.at(generator() % objects_.size());
      --remaining_requests_;
      lk.unlock();
      result.details.push_back(
          DownloadOneObject(client, generator, workload_options_, object));
      result.bytes_downloaded += result.details.back().bytes_downloaded;
    }
    return result;
  }

 private:
  std::mutex mu_;
  const WorkloadOptions& workload_options_;
  const std::vector<gcs::ObjectMetadata> objects_;
  std::int32_t remaining_requests_;
};

class UploadHelper {
 public:
  struct UploadItem {
    std::string object_name;
    std::uint64_t object_size;
  };

  struct UploadDetail {
    std::chrono::system_clock::time_point start_time;
    std::string bucket_name;
    std::string object_name;
    std::string upload_id;
    std::string peer;
    std::uint64_t bytes_uploaded;
    std::chrono::microseconds elapsed_time;
    google::cloud::Status status;
    std::optional<gcs::ObjectMetadata> metadata;
  };

  struct UploadTaskResult {
    std::uint64_t bytes_uploaded = 0;
    std::vector<UploadDetail> details;
    Counters counters;
  };

  UploadHelper(const WorkloadOptions& workload_options,
               std::vector<UploadItem> upload_items)
      : workload_options_(workload_options),
        remaining_work_(std::move(upload_items)) {}

  static UploadDetail UploadOneObject(gcs::Client& client,
                                      const WorkloadOptions& workload_options,
                                      const UploadItem& upload,
                                      const std::string& write_block,
                                      bool return_metadata = false) {
    using clock = std::chrono::steady_clock;
    using std::chrono::duration_cast;
    using std::chrono::microseconds;

    auto const buffer_size = static_cast<std::streamsize>(write_block.size());
    auto const object_start = clock::now();
    auto const start = std::chrono::system_clock::now();

    auto stream =
        client.WriteObject(workload_options.bucket_name(), upload.object_name);
    auto object_bytes = std::uint64_t{0};
    while (object_bytes < upload.object_size) {
      auto n = std::min(static_cast<std::uint64_t>(buffer_size),
                        upload.object_size - object_bytes);
      if (!stream.write(write_block.data(), static_cast<std::streamsize>(n))) {
        break;
      }
      object_bytes += n;
    }
    stream.Close();
    // Flush the logs, if any.
    if (!stream.metadata().ok()) google::cloud::LogSink::Instance().Flush();
    auto const object_elapsed =
        duration_cast<microseconds>(clock::now() - object_start);
    auto peer = ExtractPeer(stream.headers());
    auto upload_id = ExtractUploadId(stream.resumable_session_id());
    return UploadDetail{.start_time = start,
                        .bucket_name = workload_options.bucket_name(),
                        .object_name = upload.object_name,
                        .upload_id = std::move(upload_id),
                        .peer = std::move(peer),
                        .bytes_uploaded = object_bytes,
                        .elapsed_time = object_elapsed,
                        .status = stream.metadata().status(),
                        .metadata = return_metadata
                                        ? std::optional<gcs::ObjectMetadata>(
                                              stream.metadata().value())
                                        : std::nullopt};
  }

  UploadTaskResult UploadTask(const TaskConfig& config,
                              const std::string& write_block) {
    auto client = config.client;

    UploadTaskResult result;
    while (true) {
      std::unique_lock<std::mutex> lk(mu_);
      if (remaining_work_.empty()) break;
      auto const upload = std::move(remaining_work_.back());
      remaining_work_.pop_back();
      lk.unlock();
      result.details.push_back(
          UploadOneObject(client, workload_options_, upload, write_block));
      result.bytes_uploaded += result.details.back().bytes_uploaded;
    }
    return result;
  }

 private:
  std::mutex mu_;
  const WorkloadOptions& workload_options_;
  std::vector<UploadItem> remaining_work_;
};

class GetObjectMetadataHelper {
 public:
  struct GetObjectMetadataDetail {
    std::chrono::microseconds elapsed_time;
    google::cloud::Status status;
  };

  struct GetObjectMetadataTaskResult {
    std::vector<GetObjectMetadataDetail> details;
    Counters counters;
  };

  GetObjectMetadataHelper(const WorkloadOptions& workload_options,
                          std::vector<gcs::ObjectMetadata> objects)
      : workload_options_(workload_options),
        objects_(std::move(objects)),
        remaining_requests_(workload_options.request_count()) {}

  static GetObjectMetadataDetail GetObjectMetadata(
      gcs::Client& client, const gcs::ObjectMetadata& object) {
    using clock = std::chrono::steady_clock;
    using std::chrono::duration_cast;
    using std::chrono::microseconds;
    auto const start = clock::now();
    auto result = client.GetObjectMetadata(object.bucket(), object.name());
    auto const object_elapsed =
        duration_cast<microseconds>(clock::now() - start);
    return GetObjectMetadataDetail{.elapsed_time = object_elapsed,
                                   .status = result.status()};
  }

  GetObjectMetadataTaskResult GetObjectMetadataTask(const TaskConfig& config) {
    auto client = config.client;
    auto generator = std::mt19937_64(config.seed);

    GetObjectMetadataTaskResult result;
    while (true) {
      std::unique_lock<std::mutex> lk(mu_);
      if (remaining_requests_ <= 0) break;
      const gcs::ObjectMetadata& object =
          objects_.at(generator() % objects_.size());
      --remaining_requests_;
      lk.unlock();
      result.details.push_back(GetObjectMetadata(client, object));
    }
    return result;
  }

 private:
  std::mutex mu_;
  const WorkloadOptions& workload_options_;
  const std::vector<gcs::ObjectMetadata> objects_;
  std::int32_t remaining_requests_;
};

gcs::Client MakeClient(const WorkloadOptions& workload_options) {
  auto opts = GetStorageClientOptions(workload_options)
                  // Make the upload buffer size small, the library will flush
                  // on almost all `.write()` requests.
                  .set<gcs::UploadBufferSizeOption>(256 * gcs_bm::kKiB);
#if GOOGLE_CLOUD_CPP_STORAGE_HAVE_GRPC
  namespace gcs_ex = ::google::cloud::storage_experimental;
  if (workload_options.api() == "GRPC") {
    return gcs_ex::DefaultGrpcClient(
        std::move(opts).set<gcs_ex::GrpcPluginOption>("metadata"));
  }
#endif  // GOOGLE_CLOUD_CPP_STORAGE_HAVE_GRPC
  return gcs::Client(std::move(opts));
}

}  // namespace

void DownloadMain(gcs::Client& client, const WorkloadOptions& workload_options,
                  std::vector<TaskConfig>& configs, Counters& accumulated) {
  // Generate the dataset.
  constexpr std::int32_t kMaxObjects = 1;
  std::vector<gcs::ObjectMetadata> objects(kMaxObjects);
  std::vector<UploadHelper::UploadItem> upload_items(kMaxObjects);
  std::mt19937_64 generator(std::random_device{}());
  std::generate(upload_items.begin(), upload_items.end(), [&] {
    auto const object_size = std::uniform_int_distribution<std::uint64_t>(
        workload_options.minimum_object_size(),
        workload_options.maximum_object_size())(generator);
    return UploadHelper::UploadItem{workload_options.object_prefix() +
                                        gcs_bm::MakeRandomObjectName(generator),
                                    object_size};
  });
  auto const write_block = [&] {
    std::string block;
    std::int64_t lineno = 0;
    while (block.size() <
           workload_options.upload_options().resumable_upload_chunk_size()) {
      // Create data that consists of equally-sized, numbered lines.
      auto constexpr kLineSize = 128;
      auto header = absl::StrFormat("%09d", lineno++);
      block += header;
      block += gcs_bm::MakeRandomData(generator, kLineSize - header.size());
    }
    return block;
  }();
  for (int i = 0; i < upload_items.size(); ++i) {
    UploadHelper::UploadDetail object_detail = UploadHelper::UploadOneObject(
        client, workload_options, upload_items.at(i), write_block,
        /*return_metadata=*/true);
    objects.push_back(*object_detail.metadata);
  }

  auto timer = Timer::PerProcess();
  DownloadHelper download_helper(workload_options, objects);
  auto task = [&download_helper](const TaskConfig& c) {
    return download_helper.DownloadTask(c);
  };
  std::vector<std::future<DownloadHelper::DownloadTaskResult>> tasks(
      configs.size());
  std::transform(configs.begin(), configs.end(), tasks.begin(),
                 [&task](const TaskConfig& c) {
                   return std::async(std::launch::async, task, std::cref(c));
                 });

  std::vector<DownloadHelper::DownloadTaskResult> iteration_results(
      configs.size());
  std::transform(std::make_move_iterator(tasks.begin()),
                 std::make_move_iterator(tasks.end()),
                 iteration_results.begin(),
                 [](std::future<DownloadHelper::DownloadTaskResult> f) {
                   return f.get();
                 });
  auto const usage = timer.Sample();

  // Update the counters.
  for (const auto& r : iteration_results) {
    for (const auto& kv : r.counters) accumulated[kv.first] += kv.second;
  }

  // After each iteration print a human-readable summary.
  auto accumulate_bytes_downloaded =
      [](const std::vector<DownloadHelper::DownloadTaskResult>& r) {
        return std::accumulate(
            r.begin(), r.end(), std::int64_t{0},
            [](std::int64_t a, const DownloadHelper::DownloadTaskResult& b) {
              return a + b.bytes_downloaded;
            });
      };
  auto const downloaded_bytes = accumulate_bytes_downloaded(iteration_results);
  auto const bandwidth =
      FormatBandwidthGbPerSecond(downloaded_bytes, usage.elapsed_time);
  std::cout << "# " << gcs_bm::CurrentTime()
            << " downloaded=" << downloaded_bytes
            << " cpu_time=" << absl::FromChrono(usage.cpu_time)
            << " elapsed_time=" << absl::FromChrono(usage.elapsed_time)
            << " Gbit/s=" << bandwidth << std::endl;
}

void UploadMain(const WorkloadOptions& workload_options,
                std::vector<TaskConfig>& configs, Counters& accumulated) {
  std::vector<UploadHelper::UploadItem> upload_items(
      workload_options.request_count());
  std::mt19937_64 generator(std::random_device{}());
  std::generate(upload_items.begin(), upload_items.end(), [&] {
    auto const object_size = std::uniform_int_distribution<std::uint64_t>(
        workload_options.minimum_object_size(),
        workload_options.maximum_object_size())(generator);
    return UploadHelper::UploadItem{workload_options.object_prefix() +
                                        gcs_bm::MakeRandomObjectName(generator),
                                    object_size};
  });
  auto const write_block = [&] {
    std::string block;
    std::int64_t lineno = 0;
    while (block.size() <
           workload_options.upload_options().resumable_upload_chunk_size()) {
      // Create data that consists of equally-sized, numbered lines.
      auto constexpr kLineSize = 128;
      auto header = absl::StrFormat("%09d", lineno++);
      block += header;
      block += gcs_bm::MakeRandomData(generator, kLineSize - header.size());
    }
    return block;
  }();

  auto accumulate_bytes_uploaded =
      [](const std::vector<UploadHelper::UploadTaskResult>& r) {
        return std::accumulate(
            r.begin(), r.end(), std::int64_t{0},
            [](std::int64_t a, const UploadHelper::UploadTaskResult& b) {
              return a + b.bytes_uploaded;
            });
      };

  auto timer = Timer::PerProcess();
  UploadHelper upload_helper(workload_options, upload_items);
  auto task = [&upload_helper, &write_block](const TaskConfig& c) {
    return upload_helper.UploadTask(c, write_block);
  };
  std::vector<std::future<UploadHelper::UploadTaskResult>> tasks(
      configs.size());
  std::transform(configs.begin(), configs.end(), tasks.begin(),
                 [&task](const TaskConfig& c) {
                   return std::async(std::launch::async, task, std::cref(c));
                 });

  // After each iteration print a human-readable summary.
  std::vector<UploadHelper::UploadTaskResult> iteration_results(configs.size());
  std::transform(
      std::make_move_iterator(tasks.begin()),
      std::make_move_iterator(tasks.end()), iteration_results.begin(),
      [](std::future<UploadHelper::UploadTaskResult> f) { return f.get(); });
  auto const usage = timer.Sample();
  auto const uploaded_bytes = accumulate_bytes_uploaded(iteration_results);
  for (const auto& r : iteration_results) {
    // Update the counters.
    for (const auto& kv : r.counters) accumulated[kv.first] += kv.second;
  }
  auto const bandwidth =
      FormatBandwidthGbPerSecond(uploaded_bytes, usage.elapsed_time);
  std::cout << "# " << gcs_bm::CurrentTime() << " uploaded=" << uploaded_bytes
            << " cpu_time=" << absl::FromChrono(usage.cpu_time)
            << " elapsed_time=" << absl::FromChrono(usage.elapsed_time)
            << " Gbit/s=" << bandwidth << std::endl;
}

void GetObjectMetadataMain(gcs::Client& client,
                           const WorkloadOptions& workload_options,
                           std::vector<TaskConfig>& configs,
                           Counters& accumulated) {
  // Generate the dataset.
  constexpr std::int32_t kMaxObjects = 1;
  std::vector<gcs::ObjectMetadata> staged_objects(kMaxObjects);
  std::vector<UploadHelper::UploadItem> upload_items(kMaxObjects);
  std::mt19937_64 generator(std::random_device{}());
  std::generate(upload_items.begin(), upload_items.end(), [&] {
    auto const object_size = std::uniform_int_distribution<std::uint64_t>(
        workload_options.minimum_object_size(),
        workload_options.maximum_object_size())(generator);
    return UploadHelper::UploadItem{workload_options.object_prefix() +
                                        gcs_bm::MakeRandomObjectName(generator),
                                    object_size};
  });
  auto const write_block = [&] {
    std::string block;
    std::int64_t lineno = 0;
    while (block.size() <
           workload_options.upload_options().resumable_upload_chunk_size()) {
      // Create data that consists of equally-sized, numbered lines.
      auto constexpr kLineSize = 128;
      auto header = absl::StrFormat("%09d", lineno++);
      block += header;
      block += gcs_bm::MakeRandomData(generator, kLineSize - header.size());
    }
    return block;
  }();
  for (int i = 0; i < upload_items.size(); ++i) {
    UploadHelper::UploadDetail object_detail = UploadHelper::UploadOneObject(
        client, workload_options, upload_items.at(i), write_block,
        /*return_metadata=*/true);
    staged_objects.push_back(*object_detail.metadata);
  }

  auto timer = Timer::PerProcess();
  GetObjectMetadataHelper get_object_metadata_helper(workload_options,
                                                     staged_objects);
  auto task = [&get_object_metadata_helper](const TaskConfig& c) {
    return get_object_metadata_helper.GetObjectMetadataTask(c);
  };
  std::vector<std::future<GetObjectMetadataHelper::GetObjectMetadataTaskResult>>
      tasks(configs.size());
  std::transform(configs.begin(), configs.end(), tasks.begin(),
                 [&task](const TaskConfig& c) {
                   return std::async(std::launch::async, task, std::cref(c));
                 });

  std::vector<GetObjectMetadataHelper::GetObjectMetadataTaskResult>
      iteration_results(configs.size());
  std::transform(
      std::make_move_iterator(tasks.begin()),
      std::make_move_iterator(tasks.end()), iteration_results.begin(),
      [](std::future<GetObjectMetadataHelper::GetObjectMetadataTaskResult> f) {
        return f.get();
      });
  auto const usage = timer.Sample();

  // Update the counters.
  size_t requests = 0;
  for (const auto& r : iteration_results) {
    for (const auto& kv : r.counters) accumulated[kv.first] += kv.second;
    requests += r.details.size();
  }

  auto const qps =
      FormatQueriesPerSecond(requests, usage.elapsed_time);
  std::cout << "# " << gcs_bm::CurrentTime()
            << " requests=" << requests
            << " cpu_time=" << absl::FromChrono(usage.cpu_time)
            << " elapsed_time=" << absl::FromChrono(usage.elapsed_time)
            << " QPS=" << qps << std::endl;
}

// **************************************************************************

int main(int argc, char* argv[]) {
  // Parse command line flags.
  google::cloud::StatusOr<WorkloadOptions> workload_options =
      ParseArgs(argc, argv);
  if (!workload_options) {
    std::cerr << workload_options.status() << "\n";
    return 1;
  }
  if (workload_options->exit_after_parse()) return 0;

  // Create the client and print workload information.
  // TODO(zhanif): Add upload and download information.
  auto client = MakeClient(*workload_options);
  std::cout << "# Start time: " << gcs_bm::CurrentTime()
            << "\n# Labels: " << workload_options->labels()
            << "\n# Bucket Name: " << workload_options->bucket_name()
            << "\n# Object Prefix: " << workload_options->object_prefix()
            << "\n# Request Count: " << workload_options->request_count()
            << "\n# Minimum Object Size: "
            << FormatSize(workload_options->minimum_object_size())
            << "\n# Maximum Object Size: "
            << FormatSize(workload_options->maximum_object_size())
            << "\n# Thread Count: " << workload_options->thread_count()
            << "\n# API: " << workload_options->api()
            << "\n# Client Per Thread: " << std::boolalpha
            << workload_options->client_per_thread();
  gcs_bm::PrintOptions(std::cout, "Client Options",
                       gcs_load::GetStorageClientOptions(*workload_options));
  std::string notes = google::cloud::storage::version_string() + ";" +
                      google::cloud::internal::compiler() + ";" +
                      google::cloud::internal::compiler_flags();
  std::transform(notes.begin(), notes.end(), notes.begin(),
                 [](char c) { return c == '\n' ? ';' : c; });
  std::cout << "\n# Build Info: " << notes << std::endl;

  // Create the configs.
  auto configs = [](const WorkloadOptions& workload_options,
                    const gcs::Client& default_client) {
    std::random_device rd;
    std::vector<std::seed_seq::result_type> seeds(
        workload_options.thread_count());
    std::seed_seq({rd(), rd(), rd()}).generate(seeds.begin(), seeds.end());
    std::vector<TaskConfig> config(workload_options.thread_count(),
                                   TaskConfig{.client = default_client});
    for (std::size_t i = 0; i != config.size(); ++i) {
      if (workload_options.client_per_thread())
        config[i].client = MakeClient(workload_options);
      config[i].seed = seeds[i];
    }
    return config;
  }(*workload_options, client);

  // Run the workload.
  Counters accumulated;
  switch (workload_options->operation()) {
    case WorkloadOptions::DOWNLOAD:
      DownloadMain(client, *workload_options, configs, accumulated);
      break;
    case WorkloadOptions::UPLOAD:
      UploadMain(*workload_options, configs, accumulated);
      break;
    case WorkloadOptions::GET_OBJECT_METADATA:
      GetObjectMetadataMain(client, *workload_options, configs, accumulated);
      break;
    case WorkloadOptions::OPERATION_TYPE_UNSPECIFIED:
      break;
  }

  for (auto& kv : accumulated) {
    std::cout << "# counter " << kv.first << ": " << kv.second << "\n";
  }

  return 0;
}
