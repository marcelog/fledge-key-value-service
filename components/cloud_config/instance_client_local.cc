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
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "components/cloud_config/instance_client.h"
#include "glog/logging.h"

ABSL_FLAG(std::string, environment, "local", "Environment name.");
ABSL_FLAG(std::string, shard_num, "0", "Shard number.");

namespace kv_server {
namespace {

using privacy_sandbox::server_common::MetricsRecorder;

class LocalInstanceClient : public InstanceClient {
 public:
  absl::StatusOr<std::string> GetEnvironmentTag() override {
    return absl::GetFlag(FLAGS_environment);
  }

  absl::StatusOr<std::string> GetShardNumTag() override {
    return absl::GetFlag(FLAGS_shard_num);
  }

  absl::Status RecordLifecycleHeartbeat(
      std::string_view lifecycle_hook_name) override {
    LOG(INFO) << "Record lifecycle heartbeat.";
    return absl::OkStatus();
  }

  absl::Status CompleteLifecycle(
      std::string_view lifecycle_hook_name) override {
    LOG(INFO) << "Complete lifecycle.";
    return absl::OkStatus();
  }

  absl::StatusOr<std::string> GetInstanceId() override {
    std::string hostname;
    hostname.resize(HOST_NAME_MAX);
    const int result = gethostname(hostname.data(), HOST_NAME_MAX);
    if (result != 0) {
      return absl::ErrnoToStatus(errno, strerror(errno));
    }
    hostname.resize(strlen(hostname.c_str()));
    return hostname;
  }

  absl::StatusOr<std::vector<InstanceInfo>> DescribeInstanceGroupInstances(
      DescribeInstanceGroupInput& describe_instance_group_input) override {
    auto id = GetInstanceId();
    return DescribeInstances({});
  }

  absl::StatusOr<std::vector<InstanceInfo>> DescribeInstances(
      const absl::flat_hash_set<std::string>& instance_ids) {
    auto id = GetInstanceId();
    if (!id.ok()) {
      return id.status();
    }
    return std::vector<InstanceInfo>{InstanceInfo{.id = *id}};
  }
};

}  // namespace

std::unique_ptr<InstanceClient> InstanceClient::Create(
    MetricsRecorder& metrics_recorder) {
  return std::make_unique<LocalInstanceClient>();
}

}  // namespace kv_server
