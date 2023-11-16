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

#include "components/udf/noop_udf_client.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "components/udf/code_config.h"
#include "components/udf/udf_client.h"
#include "roma/config/src/config.h"

namespace kv_server {

namespace {
class NoopUdfClientImpl : public UdfClient {
 public:
  absl::StatusOr<std::string> ExecuteCode(std::vector<std::string> keys) const {
    return "";
  }
  absl::StatusOr<std::string> ExecuteCode(
      UDFExecutionMetadata&&,
      const google::protobuf::RepeatedPtrField<UDFArgument>& arguments) const {
    return "";
  }

  absl::Status Stop() { return absl::OkStatus(); }

  absl::Status SetCodeObject(CodeConfig code_config) {
    return absl::OkStatus();
  }

  absl::Status SetWasmCodeObject(CodeConfig code_config) {
    return absl::OkStatus();
  }
};

}  // namespace

std::unique_ptr<UdfClient> NewNoopUdfClient() {
  return std::make_unique<NoopUdfClientImpl>();
}

}  // namespace kv_server
