// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "public/applications/pas/retrieval_request_builder.h"

namespace kv_server::application_pas {

v2::GetValuesRequest BuildRetrievalRequest(
    std::string protected_signals,
    absl::flat_hash_map<std::string, std::string> device_metadata,
    std::string contextual_signals, std::vector<std::string> optional_ad_ids) {
  static const std::string* kClient = new std::string("Retrieval.20231018");

  v2::GetValuesRequest req;
  req.set_client_version(*kClient);
  v2::RequestPartition* partition = req.add_partitions();
  {
    auto* protected_signals_arg = partition->add_arguments();
    protected_signals_arg->mutable_data()->set_string_value(
        std::move(protected_signals));
  }
  {
    auto* device_metadata_arg = partition->add_arguments();
    for (auto&& [key, value] : std::move(device_metadata)) {
      (*device_metadata_arg->mutable_data()
            ->mutable_struct_value()
            ->mutable_fields())[std::move(key)]
          .set_string_value(value);
    }
  }
  {
    auto* contextual_signals_arg = partition->add_arguments();
    contextual_signals_arg->mutable_data()->set_string_value(
        std::move(contextual_signals));
  }
  {
    auto* ad_id_arg = partition->add_arguments();
    for (auto&& item : std::move(optional_ad_ids)) {
      ad_id_arg->mutable_data()
          ->mutable_list_value()
          ->add_values()
          ->set_string_value(std::move(item));
    }
  }
  return req;
}

}  // namespace kv_server::application_pas
