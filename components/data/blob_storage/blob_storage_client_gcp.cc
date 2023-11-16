// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "components/data/blob_storage/blob_storage_client_gcp.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "components/data/blob_storage/blob_storage_client.h"
#include "components/data/blob_storage/seeking_input_streambuf.h"
#include "components/errors/error_util_gcp.h"
#include "glog/logging.h"
#include "google/cloud/storage/client.h"

namespace kv_server {
namespace {

using privacy_sandbox::server_common::MetricsRecorder;

class GcpBlobInputStreamBuf : public SeekingInputStreambuf {
 public:
  GcpBlobInputStreamBuf(google::cloud::storage::Client& client,
                        BlobStorageClient::DataLocation location,
                        MetricsRecorder& metrics_recorder,
                        SeekingInputStreambuf::Options options)
      : SeekingInputStreambuf(metrics_recorder, std::move(options)),
        client_(client),
        location_(std::move(location)) {}

  GcpBlobInputStreamBuf(const GcpBlobInputStreamBuf&) = delete;
  GcpBlobInputStreamBuf& operator=(const GcpBlobInputStreamBuf&) = delete;

 protected:
  absl::StatusOr<int64_t> SizeImpl() override {
    auto object_metadata =
        client_.GetObjectMetadata(location_.bucket, location_.key);
    if (!object_metadata) {
      return GoogleErrorStatusToAbslStatus(object_metadata.status());
    }
    return object_metadata->size();
  }

  absl::StatusOr<int64_t> ReadChunk(int64_t offset, int64_t chunk_size,
                                    char* dest_buffer) override {
    auto stream = client_.ReadObject(
        location_.bucket, location_.key,
        google::cloud::storage::ReadRange(offset, offset + chunk_size));
    if (!stream.status().ok()) {
      return GoogleErrorStatusToAbslStatus(stream.status());
    }
    std::string contents(std::istreambuf_iterator<char>{stream}, {});
    int64_t true_size = contents.size();
    std::copy(contents.begin(), contents.end(), dest_buffer);
    return true_size;
  }

 private:
  google::cloud::storage::Client& client_;
  const BlobStorageClient::DataLocation location_;
};

class GcpBlobReader : public BlobReader {
 public:
  GcpBlobReader(google::cloud::storage::Client& client,
                BlobStorageClient::DataLocation location,
                MetricsRecorder& metrics_recorder)
      : BlobReader(),
        streambuf_(client, location, metrics_recorder,
                   GetOptions([this, location](absl::Status status) {
                     LOG(ERROR) << "Blob " << location.key
                                << " failed stream with: " << status;
                     is_.setstate(std::ios_base::badbit);
                   })),
        is_(&streambuf_) {}

  std::istream& Stream() { return is_; }
  bool CanSeek() const { return true; }

 private:
  static SeekingInputStreambuf::Options GetOptions(
      std::function<void(absl::Status)> error_callback) {
    SeekingInputStreambuf::Options options;
    options.error_callback = std::move(error_callback);
    return options;
  }

  GcpBlobInputStreamBuf streambuf_;
  std::istream is_;
};
}  // namespace

GcpBlobStorageClient::GcpBlobStorageClient(
    MetricsRecorder& metrics_recorder,
    std::unique_ptr<google::cloud::storage::Client> client)
    : metrics_recorder_(metrics_recorder), client_(std::move(client)) {}

std::unique_ptr<BlobReader> GcpBlobStorageClient::GetBlobReader(
    DataLocation location) {
  return std::make_unique<GcpBlobReader>(*client_, std::move(location),
                                         metrics_recorder_);
}

absl::Status GcpBlobStorageClient::PutBlob(BlobReader& blob_reader,
                                           DataLocation location) {
  auto blob_ostream = client_->WriteObject(location.bucket, location.key);
  if (!blob_ostream) {
    return GoogleErrorStatusToAbslStatus(blob_ostream.last_status());
  }
  blob_ostream << blob_reader.Stream().rdbuf();
  blob_ostream.Close();
  return blob_ostream
             ? absl::OkStatus()
             : GoogleErrorStatusToAbslStatus(blob_ostream.last_status());
}

absl::Status GcpBlobStorageClient::DeleteBlob(DataLocation location) {
  google::cloud::Status status =
      client_->DeleteObject(location.bucket, location.key);
  return status.ok() ? absl::OkStatus() : GoogleErrorStatusToAbslStatus(status);
}

absl::StatusOr<std::vector<std::string>> GcpBlobStorageClient::ListBlobs(
    DataLocation location, ListOptions options) {
  auto list_object_reader = client_->ListObjects(
      location.bucket, google::cloud::storage::Prefix(options.prefix),
      google::cloud::storage::StartOffset(options.start_after));
  std::vector<std::string> keys;
  if (list_object_reader.begin() == list_object_reader.end()) {
    return keys;
  }
  for (auto&& object_metadata : list_object_reader) {
    if (!object_metadata) {
      LOG(ERROR) << "Blob error when listing blobs:"
                 << std::move(object_metadata).status().message();
      continue;
    }

    // Manually exclude the starting name as the StartOffset option is
    // inclusive.
    if (object_metadata->name() == options.start_after) {
      continue;
    }
    keys.push_back(object_metadata->name());
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

namespace {
class GcpBlobStorageClientFactory : public BlobStorageClientFactory {
 public:
  ~GcpBlobStorageClientFactory() = default;
  std::unique_ptr<BlobStorageClient> CreateBlobStorageClient(
      MetricsRecorder& metrics_recorder,
      BlobStorageClient::ClientOptions /*client_options*/) override {
    return std::make_unique<GcpBlobStorageClient>(
        metrics_recorder, std::make_unique<google::cloud::storage::Client>());
  }
};
}  // namespace

std::unique_ptr<BlobStorageClientFactory> BlobStorageClientFactory::Create() {
  return std::make_unique<GcpBlobStorageClientFactory>();
}
}  // namespace kv_server
