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

#include "components/data_server/request_handler/get_values_adapter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "components/data_server/request_handler/v2_response_data.pb.h"
#include "glog/logging.h"
#include "google/protobuf/util/json_util.h"
#include "public/api_schema.pb.h"
#include "public/applications/pa/api_overlay.pb.h"
#include "public/applications/pa/response_utils.h"
#include "src/cpp/util/status_macro/status_macros.h"

namespace kv_server {
namespace {
using google::protobuf::RepeatedPtrField;
using google::protobuf::Struct;
using google::protobuf::Value;
using google::protobuf::util::JsonStringToMessage;

constexpr char kKeysTag[] = "keys";
constexpr char kRenderUrlsTag[] = "renderUrls";
constexpr char kAdComponentRenderUrlsTag[] = "adComponentRenderUrls";
constexpr char kKvInternalTag[] = "kvInternal";
constexpr char kCustomTag[] = "custom";

constexpr int kUdfInputApiVersion = 1;

UDFArgument BuildArgument(const RepeatedPtrField<std::string>& keys,
                          std::string namespace_tag) {
  UDFArgument arg;
  arg.mutable_tags()->add_values()->set_string_value(kCustomTag);
  arg.mutable_tags()->add_values()->set_string_value(namespace_tag);
  auto* key_list = arg.mutable_data()->mutable_list_value();
  for (const auto& key : keys) {
    key_list->add_values()->set_string_value(key);
  }
  return arg;
}

v2::GetValuesRequest BuildV2Request(const v1::GetValuesRequest& v1_request) {
  v2::GetValuesRequest v2_request;
  (*v2_request.mutable_metadata()->mutable_fields())["hostname"]
      .set_string_value(v1_request.subkey());
  auto* partition = v2_request.add_partitions();

  if (v1_request.keys_size() > 0) {
    *partition->add_arguments() = BuildArgument(v1_request.keys(), kKeysTag);
  }
  if (v1_request.render_urls_size() > 0) {
    *partition->add_arguments() =
        BuildArgument(v1_request.render_urls(), kRenderUrlsTag);
  }
  if (v1_request.ad_component_render_urls_size() > 0) {
    *partition->add_arguments() = BuildArgument(
        v1_request.ad_component_render_urls(), kAdComponentRenderUrlsTag);
  }
  if (v1_request.kv_internal_size() > 0) {
    *partition->add_arguments() =
        BuildArgument(v1_request.kv_internal(), kKvInternalTag);
  }
  return v2_request;
}

// Add key value pairs to the result struct
void ProcessKeyValues(application_pa::KeyGroupOutput key_group_output,
                      Struct& result_struct) {
  for (auto&& [k, v] : std::move(key_group_output.key_values())) {
    if (v.value().has_string_value()) {
      Value value_proto;
      absl::Status status =
          JsonStringToMessage(v.value().string_value(), &value_proto);
      if (status.ok()) {
        (*result_struct.mutable_fields())[std::move(k)] = value_proto;
      }
    }
    (*result_struct.mutable_fields())[std::move(k)] = v.value();
  }
}

// Find the namespace tag that is paired with the "custom" tag.
absl::StatusOr<std::string> FindNamespace(RepeatedPtrField<std::string> tags) {
  if (tags.size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected 2 tags, found ", tags.size()));
  }

  bool has_custom_tag = false;
  std::string maybe_namespace_tag;
  for (auto&& tag : std::move(tags)) {
    if (tag == kCustomTag) {
      has_custom_tag = true;
    } else {
      maybe_namespace_tag = std::move(tag);
    }
  }

  if (has_custom_tag) {
    return maybe_namespace_tag;
  }
  return absl::InvalidArgumentError("No namespace tags found");
}

void ProcessKeyGroupOutput(application_pa::KeyGroupOutput key_group_output,
                           v1::GetValuesResponse& v1_response) {
  // Ignore if no valid namespace tag that is paired with a 'custom' tag
  auto tag_namespace_status_or =
      FindNamespace(std::move(key_group_output.tags()));
  if (!tag_namespace_status_or.ok()) {
    return;
  }
  if (tag_namespace_status_or.value() == kKeysTag) {
    ProcessKeyValues(std::move(key_group_output), *v1_response.mutable_keys());
  }
  if (tag_namespace_status_or.value() == kRenderUrlsTag) {
    ProcessKeyValues(std::move(key_group_output),
                     *v1_response.mutable_render_urls());
  }
  if (tag_namespace_status_or.value() == kAdComponentRenderUrlsTag) {
    ProcessKeyValues(std::move(key_group_output),
                     *v1_response.mutable_ad_component_render_urls());
  }
  if (tag_namespace_status_or.value() == kKvInternalTag) {
    ProcessKeyValues(std::move(key_group_output),
                     *v1_response.mutable_kv_internal());
  }
}

// Converts a v2 response into v1 response.
absl::Status ConvertToV1Response(const v2::GetValuesResponse& v2_response,
                                 v1::GetValuesResponse& v1_response) {
  if (!v2_response.has_single_partition()) {
    // This should not happen. V1 request always maps to 1 partition so the
    // output should always have 1 partition.
    return absl::InternalError(
        "Bug in KV server! response does not have single_partition set for V1 "
        "response.");
  }
  if (v2_response.single_partition().has_status()) {
    return absl::Status(static_cast<absl::StatusCode>(
                            v2_response.single_partition().status().code()),
                        v2_response.single_partition().status().message());
  }
  const std::string& string_output =
      v2_response.single_partition().string_output();
  // string_output should be a JSON object
  PS_ASSIGN_OR_RETURN(application_pa::KeyGroupOutputs outputs,
                      application_pa::KeyGroupOutputsFromJson(string_output));
  for (const auto& key_group_output : outputs.key_group_outputs()) {
    ProcessKeyGroupOutput(key_group_output, v1_response);
  }

  return absl::OkStatus();
}

}  // namespace

class GetValuesAdapterImpl : public GetValuesAdapter {
 public:
  explicit GetValuesAdapterImpl(std::unique_ptr<GetValuesV2Handler> v2_handler)
      : v2_handler_(std::move(v2_handler)) {}

  grpc::Status CallV2Handler(const v1::GetValuesRequest& v1_request,
                             v1::GetValuesResponse& v1_response) const {
    v2::GetValuesRequest v2_request = BuildV2Request(v1_request);
    VLOG(7) << "Converting V1 request " << v1_request.DebugString()
            << " to v2 request " << v2_request.DebugString();
    v2::GetValuesResponse v2_response;
    if (auto status = v2_handler_->GetValues(v2_request, &v2_response);
        !status.ok()) {
      return status;
    }
    VLOG(7) << "Received v2 response: " << v2_response.DebugString();
    return privacy_sandbox::server_common::FromAbslStatus(
        ConvertToV1Response(v2_response, v1_response));
  }

 private:
  std::unique_ptr<GetValuesV2Handler> v2_handler_;
};

std::unique_ptr<GetValuesAdapter> GetValuesAdapter::Create(
    std::unique_ptr<GetValuesV2Handler> v2_handler) {
  return std::make_unique<GetValuesAdapterImpl>(std::move(v2_handler));
}

}  // namespace kv_server
