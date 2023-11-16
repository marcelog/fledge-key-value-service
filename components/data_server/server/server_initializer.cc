/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "components/data_server/server/server_initializer.h"

#include <utility>

#include "components/internal_server/constants.h"
#include "components/internal_server/local_lookup.h"
#include "components/internal_server/lookup_server_impl.h"
#include "components/internal_server/remote_lookup_client.h"
#include "components/internal_server/sharded_lookup.h"
#include "glog/logging.h"
#include "src/cpp/encryption/key_fetcher/src/key_fetcher_manager.h"

namespace kv_server {
namespace {
using privacy_sandbox::server_common::KeyFetcherManagerInterface;

absl::Status InitializeUdfHooksInternal(
    std::function<std::unique_ptr<Lookup>()> get_lookup,
    GetValuesHook& string_get_values_hook,
    GetValuesHook& binary_get_values_hook, RunQueryHook& run_query_hook) {
  VLOG(9) << "Finishing getValues init";
  string_get_values_hook.FinishInit(get_lookup());
  VLOG(9) << "Finishing getValuesBinary init";
  binary_get_values_hook.FinishInit(get_lookup());
  VLOG(9) << "Finishing runQuery init";
  run_query_hook.FinishInit(get_lookup());
  return absl::OkStatus();
}

class NonshardedServerInitializer : public ServerInitializer {
 public:
  explicit NonshardedServerInitializer(MetricsRecorder& metrics_recorder,
                                       Cache& cache)
      : metrics_recorder_(metrics_recorder), cache_(cache) {}

  RemoteLookup CreateAndStartRemoteLookupServer() override {
    RemoteLookup remote_lookup;
    return remote_lookup;
  }

  absl::StatusOr<ShardManagerState> InitializeUdfHooks(
      GetValuesHook& string_get_values_hook,
      GetValuesHook& binary_get_values_hook,
      RunQueryHook& run_query_hook) override {
    ShardManagerState shard_manager_state;
    auto lookup_supplier = [&cache = cache_,
                            &metrics_recorder = metrics_recorder_]() {
      return CreateLocalLookup(cache, metrics_recorder);
    };
    InitializeUdfHooksInternal(std::move(lookup_supplier),
                               string_get_values_hook, binary_get_values_hook,
                               run_query_hook);
    return shard_manager_state;
  }

 private:
  MetricsRecorder& metrics_recorder_;
  Cache& cache_;
};

class ShardedServerInitializer : public ServerInitializer {
 public:
  explicit ShardedServerInitializer(
      MetricsRecorder& metrics_recorder,
      KeyFetcherManagerInterface& key_fetcher_manager, Lookup& local_lookup,
      std::string environment, int32_t num_shards, int32_t current_shard_num,
      InstanceClient& instance_client, ParameterFetcher& parameter_fetcher)
      : metrics_recorder_(metrics_recorder),
        key_fetcher_manager_(key_fetcher_manager),
        local_lookup_(local_lookup),
        environment_(environment),
        num_shards_(num_shards),
        current_shard_num_(current_shard_num),
        instance_client_(instance_client),
        parameter_fetcher_(parameter_fetcher) {}

  RemoteLookup CreateAndStartRemoteLookupServer() override {
    RemoteLookup remote_lookup;
    remote_lookup.remote_lookup_service = std::make_unique<LookupServiceImpl>(
        local_lookup_, key_fetcher_manager_, metrics_recorder_);
    grpc::ServerBuilder remote_lookup_server_builder;
    auto remoteLookupServerAddress =
        absl::StrCat(kLocalIp, ":", kRemoteLookupServerPort);
    remote_lookup_server_builder.AddListeningPort(
        remoteLookupServerAddress, grpc::InsecureServerCredentials());
    remote_lookup_server_builder.RegisterService(
        remote_lookup.remote_lookup_service.get());
    LOG(INFO) << "Remote lookup server listening on "
              << remoteLookupServerAddress;
    remote_lookup.remote_lookup_server =
        remote_lookup_server_builder.BuildAndStart();
    return std::move(remote_lookup);
  }

  absl::StatusOr<ShardManagerState> InitializeUdfHooks(
      GetValuesHook& string_get_values_hook,
      GetValuesHook& binary_get_values_hook,
      RunQueryHook& run_query_hook) override {
    auto maybe_shard_state = CreateShardManager();
    if (!maybe_shard_state.ok()) {
      return maybe_shard_state.status();
    }
    auto lookup_supplier = [&local_lookup = local_lookup_,
                            num_shards = num_shards_,
                            current_shard_num = current_shard_num_,
                            &shard_manager = *maybe_shard_state->shard_manager,
                            &metrics_recorder = metrics_recorder_]() {
      return CreateShardedLookup(local_lookup, num_shards, current_shard_num,
                                 shard_manager, metrics_recorder);
    };
    InitializeUdfHooksInternal(std::move(lookup_supplier),
                               string_get_values_hook, binary_get_values_hook,
                               run_query_hook);
    return std::move(*maybe_shard_state);
  }

 private:
  absl::StatusOr<ShardManagerState> CreateShardManager() {
    ShardManagerState shard_manager_state;
    VLOG(10) << "Creating shard manager";
    shard_manager_state.cluster_mappings_manager =
        ClusterMappingsManager::Create(environment_, num_shards_,
                                       metrics_recorder_, instance_client_,
                                       parameter_fetcher_);
    shard_manager_state.shard_manager = TraceRetryUntilOk(
        [&cluster_mappings_manager =
             *shard_manager_state.cluster_mappings_manager,
         &num_shards = num_shards_, &key_fetcher_manager = key_fetcher_manager_,
         &metrics_recorder = metrics_recorder_] {
          // It might be that the cluster mappings that are passed don't pass
          // validation. E.g. a particular cluster might not have any
          // replicas
          // specified. In that case, we need to retry the creation. After an
          // exponential backoff, that will trigger`GetClusterMappings` which
          // at that point in time might have new replicas spun up.
          return ShardManager::Create(
              num_shards, key_fetcher_manager,
              cluster_mappings_manager.GetClusterMappings(), metrics_recorder);
        },
        "GetShardManager", &metrics_recorder_);
    auto start_status = shard_manager_state.cluster_mappings_manager->Start(
        *shard_manager_state.shard_manager);
    if (!start_status.ok()) {
      return start_status;
    }
    return std::move(shard_manager_state);
  }

  MetricsRecorder& metrics_recorder_;
  KeyFetcherManagerInterface& key_fetcher_manager_;
  Lookup& local_lookup_;
  std::string environment_;
  int32_t num_shards_;
  int32_t current_shard_num_;
  InstanceClient& instance_client_;
  ParameterFetcher& parameter_fetcher_;
};

}  // namespace

std::unique_ptr<ServerInitializer> GetServerInitializer(
    int64_t num_shards, MetricsRecorder& metrics_recorder,
    KeyFetcherManagerInterface& key_fetcher_manager, Lookup& local_lookup,
    std::string environment, int32_t current_shard_num,
    InstanceClient& instance_client, Cache& cache,
    ParameterFetcher& parameter_fetcher) {
  CHECK_GT(num_shards, 0) << "num_shards must be greater than 0";
  if (num_shards == 1) {
    return std::make_unique<NonshardedServerInitializer>(metrics_recorder,
                                                         cache);
  }

  return std::make_unique<ShardedServerInitializer>(
      metrics_recorder, key_fetcher_manager, local_lookup, environment,
      num_shards, current_shard_num, instance_client, parameter_fetcher);
}
}  // namespace kv_server
