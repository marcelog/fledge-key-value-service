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

#include "components/data_server/request_handler/get_values_handler.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "components/data_server/request_handler/get_values_adapter.h"
#include "glog/logging.h"
#include "grpcpp/grpcpp.h"
#include "public/constants.h"
#include "public/query/get_values.grpc.pb.h"
#include "src/cpp/telemetry/metrics_recorder.h"
#include "src/cpp/telemetry/telemetry.h"
#include "src/google/protobuf/message.h"
#include "src/google/protobuf/struct.pb.h"

constexpr char* kCacheKeyHit = "CacheKeyHit";
constexpr char* kCacheKeyMiss = "CacheKeyMiss";

namespace kv_server {
namespace {
using google::protobuf::RepeatedPtrField;
using google::protobuf::Struct;
using google::protobuf::Value;
using grpc::StatusCode;
using privacy_sandbox::server_common::GetTracer;
using privacy_sandbox::server_common::MetricsRecorder;
using v1::GetValuesRequest;
using v1::GetValuesResponse;
using v1::KeyValueService;

absl::flat_hash_set<std::string_view> GetKeys(
    const RepeatedPtrField<std::string>& keys) {
  absl::flat_hash_set<std::string_view> key_list;
  for (const auto& key : keys) {
    for (absl::string_view individual_key :
         absl::StrSplit(key, kQueryArgDelimiter)) {
      key_list.insert(individual_key);
    }
  }
  return key_list;
}

void ProcessKeys(const RepeatedPtrField<std::string>& keys, const Cache& cache,
                 MetricsRecorder& metrics_recorder, Struct& result_struct) {
  if (keys.empty()) return;
  auto kv_pairs = cache.GetKeyValuePairs(GetKeys(keys));

  if (kv_pairs.empty())
    metrics_recorder.IncrementEventCounter(kCacheKeyMiss);
  else
    metrics_recorder.IncrementEventCounter(kCacheKeyHit);

  for (auto&& [k, v] : std::move(kv_pairs)) {
    Value value_proto;
    absl::Status status =
        google::protobuf::util::JsonStringToMessage(v, &value_proto);
    if (status.ok()) {
      (*result_struct.mutable_fields())[std::move(k)] = value_proto;
    } else {
      // If string is not a Json string that can be parsed into Value proto,
      // simply set it as pure string value to the response.
      (*result_struct.mutable_fields())[std::move(k)].set_string_value(
          std::move(v));
    }
  }
}

}  // namespace

grpc::Status GetValuesHandler::GetValues(const GetValuesRequest& request,
                                         GetValuesResponse* response) const {
  if (use_v2_) {
    VLOG(5) << "Using V2 adapter for " << request.DebugString();
    return adapter_.CallV2Handler(request, *response);
  }

  if (!request.kv_internal().empty()) {
    VLOG(5) << "Processing kv_internal for " << request.DebugString();
    ProcessKeys(request.kv_internal(), cache_, metrics_recorder_,
                *response->mutable_kv_internal());
  }
  if (!request.keys().empty()) {
    VLOG(5) << "Processing keys for " << request.DebugString();
    ProcessKeys(request.keys(), cache_, metrics_recorder_,
                *response->mutable_keys());
  }
  if (!request.render_urls().empty()) {
    VLOG(5) << "Processing render_urls for " << request.DebugString();
    ProcessKeys(request.render_urls(), cache_, metrics_recorder_,
                *response->mutable_render_urls());
  }
  if (!request.ad_component_render_urls().empty()) {
    VLOG(5) << "Processing ad_component_render_urls for "
            << request.DebugString();
    ProcessKeys(request.ad_component_render_urls(), cache_, metrics_recorder_,
                *response->mutable_ad_component_render_urls());
  }
  return grpc::Status::OK;
}

}  // namespace kv_server
