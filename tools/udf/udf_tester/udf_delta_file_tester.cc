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

#include <fstream>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/substitute.h"
#include "components/data_server/cache/cache.h"
#include "components/data_server/cache/key_value_cache.h"
#include "components/internal_server/local_lookup.h"
#include "components/udf/hooks/get_values_hook.h"
#include "components/udf/udf_client.h"
#include "components/udf/udf_config_builder.h"
#include "glog/logging.h"
#include "google/protobuf/util/json_util.h"
#include "public/data_loading/data_loading_generated.h"
#include "public/data_loading/readers/delta_record_stream_reader.h"
#include "public/query/v2/get_values_v2.pb.h"
#include "public/udf/constants.h"
#include "src/cpp/telemetry/metrics_recorder.h"
#include "src/cpp/telemetry/telemetry_provider.h"
#include "src/cpp/util/status_macro/status_macros.h"

ABSL_FLAG(std::string, kv_delta_file_path, "",
          "Path to delta file with KV pairs.");
ABSL_FLAG(std::string, udf_delta_file_path, "", "Path to UDF delta file.");
ABSL_FLAG(std::string, input_arguments, "",
          "List of input arguments in JSON format. Each input argument should "
          "be equivalent to a UDFArgument.");

namespace kv_server {

using google::protobuf::util::JsonStringToMessage;

// If the arg is const&, the Span construction complains about converting const
// string_view to non-const string_view. Since this tool is for simple testing,
// the current solution is to pass by value.
absl::Status LoadCacheFromKVMutationRecord(KeyValueMutationRecordStruct record,
                                           Cache& cache) {
  switch (record.mutation_type) {
    case KeyValueMutationType::Update: {
      LOG(INFO) << "Updating cache with key " << record.key
                << ", logical commit time " << record.logical_commit_time;
      std::visit(
          [&cache, &record](auto& value) {
            using VariantT = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<VariantT, std::string_view>) {
              cache.UpdateKeyValue(record.key, value,
                                   record.logical_commit_time);
              return;
            }
            constexpr bool is_list =
                (std::is_same_v<VariantT, std::vector<std::string_view>>);
            if constexpr (is_list) {
              cache.UpdateKeyValueSet(record.key, absl::MakeSpan(value),
                                      record.logical_commit_time);
              return;
            }
          },
          record.value);
      break;
    }
    case KeyValueMutationType::Delete: {
      cache.DeleteKey(record.key, record.logical_commit_time);
      break;
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid mutation type: ",
                       EnumNameKeyValueMutationType(record.mutation_type)));
  }
  return absl::OkStatus();
}

absl::Status LoadCacheFromFile(std::string file_path, Cache& cache) {
  std::ifstream delta_file(file_path);
  DeltaRecordStreamReader record_reader(delta_file);
  absl::Status status =
      record_reader.ReadRecords([&cache](const DataRecordStruct& data_record) {
        // Only load KVMutationRecords into cache.
        if (std::holds_alternative<KeyValueMutationRecordStruct>(
                data_record.record)) {
          return LoadCacheFromKVMutationRecord(
              std::get<KeyValueMutationRecordStruct>(data_record.record),
              cache);
        }
        return absl::OkStatus();
      });
  return status;
}

void ReadCodeConfigFromUdfConfig(
    const UserDefinedFunctionsConfigStruct& udf_config,
    CodeConfig& code_config) {
  code_config.js = udf_config.code_snippet;
  code_config.logical_commit_time = udf_config.logical_commit_time;
  code_config.udf_handler_name = udf_config.handler_name;
  code_config.version = udf_config.version;
}

absl::Status ReadCodeConfigFromFile(std::string file_path,
                                    CodeConfig& code_config) {
  std::ifstream delta_file(file_path);
  DeltaRecordStreamReader record_reader(delta_file);
  return record_reader.ReadRecords(
      [&code_config](const DataRecordStruct& data_record) {
        if (std::holds_alternative<UserDefinedFunctionsConfigStruct>(
                data_record.record)) {
          ReadCodeConfigFromUdfConfig(
              std::get<UserDefinedFunctionsConfigStruct>(data_record.record),
              code_config);
          return absl::OkStatus();
        }
        return absl::InvalidArgumentError("Invalid record type.");
      });
}

void ShutdownUdf(UdfClient& udf_client) {
  auto udf_client_stop = udf_client.Stop();
  if (!udf_client_stop.ok()) {
    LOG(ERROR) << "Error shutting down UDF execution engine: "
               << udf_client_stop;
  }
}

absl::Status TestUdf(const std::string& kv_delta_file_path,
                     const std::string& udf_delta_file_path,
                     const std::string& input_arguments) {
  LOG(INFO) << "Loading cache from delta file: " << kv_delta_file_path;
  auto noop_metrics_recorder = MetricsRecorder::CreateNoop();
  std::unique_ptr<Cache> cache = KeyValueCache::Create(*noop_metrics_recorder);
  PS_RETURN_IF_ERROR(LoadCacheFromFile(kv_delta_file_path, *cache))
      << "Error loading cache from file";

  LOG(INFO) << "Loading udf code config from delta file: "
            << udf_delta_file_path;
  CodeConfig code_config;
  PS_RETURN_IF_ERROR(ReadCodeConfigFromFile(udf_delta_file_path, code_config))
      << "Error loading UDF code from file";

  LOG(INFO) << "Starting UDF client";
  UdfConfigBuilder config_builder;
  auto string_get_values_hook =
      GetValuesHook::Create(GetValuesHook::OutputType::kString);
  string_get_values_hook->FinishInit(
      CreateLocalLookup(*cache, *noop_metrics_recorder));
  auto binary_get_values_hook =
      GetValuesHook::Create(GetValuesHook::OutputType::kBinary);
  binary_get_values_hook->FinishInit(
      CreateLocalLookup(*cache, *noop_metrics_recorder));
  auto run_query_hook = RunQueryHook::Create();
  run_query_hook->FinishInit(CreateLocalLookup(*cache, *noop_metrics_recorder));
  absl::StatusOr<std::unique_ptr<UdfClient>> udf_client = UdfClient::Create(
      config_builder.RegisterStringGetValuesHook(*string_get_values_hook)
          .RegisterBinaryGetValuesHook(*binary_get_values_hook)
          .RegisterRunQueryHook(*run_query_hook)
          .RegisterLoggingHook()
          .SetNumberOfWorkers(1)
          .Config());
  PS_RETURN_IF_ERROR(udf_client.status())
      << "Error starting UDF execution engine";

  auto code_object_status = udf_client.value()->SetCodeObject(code_config);
  if (!code_object_status.ok()) {
    LOG(ERROR) << "Error setting UDF code object: " << code_object_status;
    ShutdownUdf(*udf_client.value());
    return code_object_status;
  }

  v2::RequestPartition req_partition;
  std::string req_partition_json =
      absl::StrCat("{arguments: ", input_arguments, "}");
  LOG(INFO) << "req_partition_json: " << req_partition_json;

  JsonStringToMessage(req_partition_json, &req_partition);

  LOG(INFO) << "Calling UDF for partition: " << req_partition.DebugString();
  auto udf_result =
      udf_client.value()->ExecuteCode({}, req_partition.arguments());
  if (!udf_result.ok()) {
    LOG(ERROR) << "UDF execution failed: " << udf_result.status();
    ShutdownUdf(*udf_client.value());
    return udf_result.status();
  }
  ShutdownUdf(*udf_client.value());

  LOG(INFO) << "UDF execution result: " << udf_result.value();
  std::cout << "UDF execution result: " << udf_result.value() << std::endl;

  return absl::OkStatus();
}

}  // namespace kv_server

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  const std::string kv_delta_file_path =
      absl::GetFlag(FLAGS_kv_delta_file_path);
  const std::string udf_delta_file_path =
      absl::GetFlag(FLAGS_udf_delta_file_path);
  const std::string input_arguments = absl::GetFlag(FLAGS_input_arguments);

  auto status = kv_server::TestUdf(kv_delta_file_path, udf_delta_file_path,
                                   input_arguments);
  if (!status.ok()) {
    return -1;
  }
  return 0;
}
