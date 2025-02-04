// Copyright 2023 Google LLC
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

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "components/cloud_config/parameter_client.h"
#include "glog/logging.h"
#include "scp/cc/public/core/interface/errors.h"
#include "scp/cc/public/core/interface/execution_result.h"
#include "scp/cc/public/cpio/interface/parameter_client/parameter_client_interface.h"

namespace kv_server {
namespace {
using google::cmrt::sdk::parameter_service::v1::GetParameterRequest;
using google::cmrt::sdk::parameter_service::v1::GetParameterResponse;
using google::scp::core::ExecutionResult;
using google::scp::core::GetErrorMessage;
using google::scp::cpio::ParameterClientFactory;
using google::scp::cpio::ParameterClientInterface;
using google::scp::cpio::ParameterClientOptions;

class GcpParameterClient : public ParameterClient {
 public:
  explicit GcpParameterClient(ParameterClient::ClientOptions client_options) {
    if (client_options.client_for_unit_testing_ == nullptr) {
      parameter_client_ =
          ParameterClientFactory::Create(ParameterClientOptions());
    } else {
      parameter_client_.reset(std::move(
          (ParameterClientInterface*)client_options.client_for_unit_testing_));
    }
    auto execution_result = parameter_client_->Init();
    CHECK(execution_result.Successful())
        << "Cannot init parameter client!"
        << GetErrorMessage(execution_result.status_code);
    execution_result = parameter_client_->Run();
    CHECK(execution_result.Successful())
        << "Cannot run parameter client!"
        << GetErrorMessage(execution_result.status_code);
  }

  ~GcpParameterClient() {
    auto execution_result = parameter_client_->Stop();
    if (!execution_result.Successful()) {
      LOG(ERROR) << "Cannot stop parameter client!"
                 << GetErrorMessage(execution_result.status_code);
    }
  }

  absl::StatusOr<std::string> GetParameter(
      std::string_view parameter_name) const override {
    GetParameterRequest get_parameter_request;
    get_parameter_request.set_parameter_name(parameter_name);
    std::string param_value;
    absl::BlockingCounter counter(1);
    auto execution_result = parameter_client_->GetParameter(
        std::move(get_parameter_request),
        [&param_value, &counter](const ExecutionResult result,
                                 GetParameterResponse response) {
          if (!result.Successful()) {
            LOG(ERROR) << "GetParameter failed: "
                       << GetErrorMessage(result.status_code);
          } else {
            param_value = response.parameter_value() != "EMPTY_STRING"
                              ? response.parameter_value()
                              : "";
            // GCP secret manager does not support empty string natively.
          }
          counter.DecrementCount();
        });
    counter.Wait();
    if (!execution_result.Successful()) {
      return absl::UnavailableError(
          GetErrorMessage(execution_result.status_code));
    }
    return param_value;
  }

  absl::StatusOr<int32_t> GetInt32Parameter(
      std::string_view parameter_name) const override {
    absl::StatusOr<std::string> parameter = GetParameter(parameter_name);
    if (!parameter.ok()) {
      return parameter.status();
    }
    int32_t parameter_int32;
    if (!absl::SimpleAtoi(*parameter, &parameter_int32)) {
      const std::string error =
          absl::StrFormat("Failed converting %s parameter: %s to int32.",
                          parameter_name, *parameter);
      LOG(ERROR) << error;
      return absl::InvalidArgumentError(error);
    }

    return parameter_int32;
  }

  absl::StatusOr<bool> GetBoolParameter(
      std::string_view parameter_name) const override {
    absl::StatusOr<std::string> parameter = GetParameter(parameter_name);

    if (!parameter.ok()) {
      return parameter.status();
    }

    bool parameter_bool;
    if (!absl::SimpleAtob(*parameter, &parameter_bool)) {
      const std::string error =
          absl::StrFormat("Failed converting %s parameter: %s to bool.",
                          parameter_name, *parameter);
      LOG(ERROR) << error;
      return absl::InvalidArgumentError(error);
    }

    return parameter_bool;
  }

 private:
  std::unique_ptr<ParameterClientInterface> parameter_client_;
};

}  // namespace

std::unique_ptr<ParameterClient> ParameterClient::Create(
    ParameterClient::ClientOptions client_options) {
  return std::make_unique<GcpParameterClient>(std::move(client_options));
}

}  // namespace kv_server
