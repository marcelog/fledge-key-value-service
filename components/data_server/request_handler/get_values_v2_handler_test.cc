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

#include "components/data_server/request_handler/get_values_v2_handler.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "components/data_server/cache/cache.h"
#include "components/data_server/cache/mocks.h"
#include "components/udf/mocks.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "grpcpp/grpcpp.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "public/constants.h"
#include "public/test_util/proto_matcher.h"
#include "quiche/binary_http/binary_http_message.h"
#include "quiche/oblivious_http/common/oblivious_http_header_key_config.h"
#include "quiche/oblivious_http/oblivious_http_client.h"
#include "src/cpp/encryption/key_fetcher/src/fake_key_fetcher_manager.h"
#include "src/cpp/telemetry/metrics_recorder.h"
#include "src/cpp/telemetry/mocks.h"

namespace kv_server {
namespace {

using google::protobuf::TextFormat;
using grpc::StatusCode;
using privacy_sandbox::server_common::MockMetricsRecorder;
using testing::_;
using testing::Return;
using testing::ReturnRef;
using testing::UnorderedElementsAre;
using v2::BinaryHttpGetValuesRequest;
using v2::GetValuesHttpRequest;
using v2::ObliviousGetValuesRequest;

enum class ProtocolType {
  kPlain = 0,
  kBinaryHttp,
  kObliviousHttp,
};

class GetValuesHandlerTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<ProtocolType> {
 protected:
  template <ProtocolType protocol_type>
  bool IsUsing() {
    return GetParam() == protocol_type;
  }

  class PlainRequest {
   public:
    explicit PlainRequest(std::string plain_request_body)
        : plain_request_body_(std::move(plain_request_body)) {}

    GetValuesHttpRequest Build() const {
      GetValuesHttpRequest request;
      request.mutable_raw_body()->set_data(plain_request_body_);
      return request;
    }

    const std::string& RequestBody() const { return plain_request_body_; }

   private:
    std::string plain_request_body_;
  };

  class BHTTPRequest {
   public:
    explicit BHTTPRequest(PlainRequest plain_request) {
      quiche::BinaryHttpRequest req_bhttp_layer({});
      req_bhttp_layer.set_body(plain_request.RequestBody());
      auto maybe_serialized = req_bhttp_layer.Serialize();
      EXPECT_TRUE(maybe_serialized.ok());
      serialized_bhttp_request_ = *maybe_serialized;
    }

    BinaryHttpGetValuesRequest Build() const {
      BinaryHttpGetValuesRequest brequest;
      brequest.mutable_raw_body()->set_data(serialized_bhttp_request_);
      return brequest;
    }

    const std::string& SerializedBHTTPRequest() const {
      return serialized_bhttp_request_;
    }

   private:
    std::string serialized_bhttp_request_;
  };

  class BHTTPResponse {
   public:
    google::api::HttpBody& RawResponse() { return response_; }
    int16_t ResponseCode() const {
      const absl::StatusOr<quiche::BinaryHttpResponse> maybe_res_bhttp_layer =
          quiche::BinaryHttpResponse::Create(response_.data());
      EXPECT_TRUE(maybe_res_bhttp_layer.ok())
          << "quiche::BinaryHttpResponse::Create failed: "
          << maybe_res_bhttp_layer.status();
      return maybe_res_bhttp_layer->status_code();
    }

    std::string Unwrap() const {
      const absl::StatusOr<quiche::BinaryHttpResponse> maybe_res_bhttp_layer =
          quiche::BinaryHttpResponse::Create(response_.data());
      EXPECT_TRUE(maybe_res_bhttp_layer.ok())
          << "quiche::BinaryHttpResponse::Create failed: "
          << maybe_res_bhttp_layer.status();
      return std::string(maybe_res_bhttp_layer->body());
    }

   private:
    google::api::HttpBody response_;
  };

  class OHTTPRequest;
  class OHTTPResponseUnwrapper {
   public:
    google::api::HttpBody& RawResponse() { return response_; }

    BHTTPResponse Unwrap() {
      uint8_t key_id = 64;
      auto maybe_config = quiche::ObliviousHttpHeaderKeyConfig::Create(
          key_id, kKEMParameter, kKDFParameter, kAEADParameter);
      EXPECT_TRUE(maybe_config.ok());

      auto client =
          quiche::ObliviousHttpClient::Create(public_key_, *maybe_config);
      EXPECT_TRUE(client.ok());
      auto decrypted_response =
          client->DecryptObliviousHttpResponse(response_.data(), context_);
      BHTTPResponse bhttp_response;
      bhttp_response.RawResponse().set_data(
          decrypted_response->GetPlaintextData());
      return bhttp_response;
    }

   private:
    explicit OHTTPResponseUnwrapper(
        quiche::ObliviousHttpRequest::Context context)
        : context_(std::move(context)) {}

    google::api::HttpBody response_;
    quiche::ObliviousHttpRequest::Context context_;
    const std::string public_key_ = absl::HexStringToBytes(kTestPublicKey);

    friend class OHTTPRequest;
  };

  class OHTTPRequest {
   public:
    explicit OHTTPRequest(BHTTPRequest bhttp_request)
        : bhttp_request_(std::move(bhttp_request)) {}

    std::pair<ObliviousGetValuesRequest, OHTTPResponseUnwrapper> Build() const {
      // matches the test key pair, see common repo:
      // ../encryption/key_fetcher/src/fake_key_fetcher_manager.h
      uint8_t key_id = 64;
      auto maybe_config = quiche::ObliviousHttpHeaderKeyConfig::Create(
          key_id, 0x0020, 0x0001, 0x0001);
      EXPECT_TRUE(maybe_config.ok());

      auto client =
          quiche::ObliviousHttpClient::Create(public_key_, *maybe_config);
      EXPECT_TRUE(client.ok());
      auto encrypted_req = client->CreateObliviousHttpRequest(
          bhttp_request_.SerializedBHTTPRequest());
      EXPECT_TRUE(encrypted_req.ok());
      auto serialized_encrypted_req = encrypted_req->EncapsulateAndSerialize();
      ObliviousGetValuesRequest ohttp_req;
      ohttp_req.mutable_raw_body()->set_data(serialized_encrypted_req);

      OHTTPResponseUnwrapper response_unwrapper(
          std::move(encrypted_req.value()).ReleaseContext());
      return {std::move(ohttp_req), std::move(response_unwrapper)};
    }

   private:
    const std::string public_key_ = absl::HexStringToBytes(kTestPublicKey);
    BHTTPRequest bhttp_request_;
  };

  // For Non-plain protocols, test request and response data are converted
  // to/from the corresponding request/responses.
  grpc::Status GetValuesBasedOnProtocol(std::string request_body,
                                        google::api::HttpBody* response,
                                        int16_t* bhttp_response_code,
                                        GetValuesV2Handler* handler) {
    PlainRequest plain_request(std::move(request_body));

    if (IsUsing<ProtocolType::kPlain>()) {
      *bhttp_response_code = 200;
      return handler->GetValuesHttp(plain_request.Build(), response);
    }

    BHTTPRequest bhttp_request(std::move(plain_request));
    BHTTPResponse bresponse;

    if (IsUsing<ProtocolType::kBinaryHttp>()) {
      if (const auto s = handler->BinaryHttpGetValues(bhttp_request.Build(),
                                                      &bresponse.RawResponse());
          !s.ok()) {
        LOG(ERROR) << "BinaryHttpGetValues failed: " << s.error_message();
        return s;
      }
      *bhttp_response_code = bresponse.ResponseCode();
    } else if (IsUsing<ProtocolType::kObliviousHttp>()) {
      OHTTPRequest ohttp_request(std::move(bhttp_request));
      // get ObliviousGetValuesRequest, OHTTPResponseUnwrapper
      auto [request, response_unwrapper] = ohttp_request.Build();
      if (const auto s = handler->ObliviousGetValues(
              request, &response_unwrapper.RawResponse());
          !s.ok()) {
        LOG(ERROR) << "ObliviousGetValues failed: " << s.error_message();
        return s;
      }
      bresponse = response_unwrapper.Unwrap();
      *bhttp_response_code = bresponse.ResponseCode();
    }

    response->set_data(bresponse.Unwrap());
    return grpc::Status::OK;
  }

  MockUdfClient mock_udf_client_;
  MockMetricsRecorder mock_metrics_recorder_;
  privacy_sandbox::server_common::FakeKeyFetcherManager
      fake_key_fetcher_manager_;
};

INSTANTIATE_TEST_SUITE_P(GetValuesHandlerTest, GetValuesHandlerTest,
                         testing::Values(ProtocolType::kPlain,
                                         ProtocolType::kBinaryHttp,
                                         ProtocolType::kObliviousHttp));

TEST_P(GetValuesHandlerTest, Success) {
  UDFExecutionMetadata udf_metadata;
  TextFormat::ParseFromString(R"(
request_metadata {
  fields {
    key: "hostname"
    value {
      string_value: "example.com"
    }
  }
}
  )",
                              &udf_metadata);
  UDFArgument arg1, arg2;
  TextFormat::ParseFromString(R"(
tags {
  values {
    string_value: "structured"
  }
  values {
    string_value: "groupNames"
  }
}
data {
  list_value {
    values {
      string_value: "hello"
    }
  }
})",
                              &arg1);
  TextFormat::ParseFromString(R"(
tags {
  values {
    string_value: "custom"
  }
  values {
    string_value: "keys"
  }
}
data {
  list_value {
    values {
      string_value: "key1"
    }
  }
})",
                              &arg2);
  nlohmann::json output = nlohmann::json::parse(R"(
{
  "keyGroupOutputs": [
      {
          "keyValues": {
              "key1": "value1"
          },
          "tags": [
              "custom",
              "keys"
          ]
      },
      {
          "keyValues": {
              "hello": "world"
          },
          "tags": [
              "structured",
              "groupNames"
          ]
      }
  ]
}
  )");
  EXPECT_CALL(
      mock_udf_client_,
      ExecuteCode(EqualsProto(udf_metadata),
                  testing::ElementsAre(EqualsProto(arg1), EqualsProto(arg2))))
      .WillOnce(Return(output.dump()));

  const std::string core_request_body = R"(
{
    "metadata": {
        "hostname": "example.com"
    },
    "partitions": [
        {
            "id": 0,
            "compressionGroupId": 0,
            "arguments": [
                {
                    "tags": [
                        "structured",
                        "groupNames"
                    ],
                    "data": [
                        "hello"
                    ]
                },
                {
                    "tags": [
                        "custom",
                        "keys"
                    ],
                    "data": [
                        "key1"
                    ]
                }
            ]
        }
    ]
}
  )";

  google::api::HttpBody response;
  GetValuesV2Handler handler(mock_udf_client_, mock_metrics_recorder_,
                             fake_key_fetcher_manager_);
  int16_t bhttp_response_code = 0;
  const auto result = GetValuesBasedOnProtocol(core_request_body, &response,
                                               &bhttp_response_code, &handler);
  ASSERT_EQ(bhttp_response_code, 200);
  ASSERT_TRUE(result.ok()) << "code: " << result.error_code()
                           << ", msg: " << result.error_message();

  v2::GetValuesResponse actual_response, expected_response;
  expected_response.mutable_single_partition()->set_string_output(
      output.dump());

  ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(response.data(),
                                                          &actual_response)
                  .ok());
  EXPECT_THAT(actual_response, EqualsProto(expected_response));
}

TEST_P(GetValuesHandlerTest, NoPartition) {
  const std::string core_request_body = R"(
{
    "metadata": {
        "hostname": "example.com"
    }
})";
  google::api::HttpBody response;
  GetValuesV2Handler handler(mock_udf_client_, mock_metrics_recorder_,
                             fake_key_fetcher_manager_);
  int16_t bhttp_response_code = 0;
  const auto result = GetValuesBasedOnProtocol(core_request_body, &response,
                                               &bhttp_response_code, &handler);
  if (IsUsing<ProtocolType::kPlain>()) {
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error_code(), grpc::StatusCode::INTERNAL);
  } else {
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(bhttp_response_code, 500);
  }
}

TEST_P(GetValuesHandlerTest, UdfFailureForOnePartition) {
  EXPECT_CALL(mock_udf_client_, ExecuteCode(_, testing::IsEmpty()))
      .WillOnce(Return(absl::InternalError("UDF execution error")));

  const std::string core_request_body = R"(
{
    "partitions": [
        {
            "id": 0,
        }
    ]
}
  )";

  google::api::HttpBody response;
  GetValuesV2Handler handler(mock_udf_client_, mock_metrics_recorder_,
                             fake_key_fetcher_manager_);
  int16_t bhttp_response_code = 0;
  const auto result = GetValuesBasedOnProtocol(core_request_body, &response,
                                               &bhttp_response_code, &handler);
  ASSERT_EQ(bhttp_response_code, 200);
  ASSERT_TRUE(result.ok()) << "code: " << result.error_code()
                           << ", msg: " << result.error_message();

  v2::GetValuesResponse actual_response, expected_response;
  auto* resp_status =
      expected_response.mutable_single_partition()->mutable_status();
  resp_status->set_code(13);
  resp_status->set_message("UDF execution error");

  ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(response.data(),
                                                          &actual_response)
                  .ok());
  EXPECT_THAT(actual_response, EqualsProto(expected_response));
}

TEST_F(GetValuesHandlerTest, PureGRPCTest) {
  v2::GetValuesRequest req;
  TextFormat::ParseFromString(
      R"pb(partitions {
             id: 9
             arguments { data { string_value: "ECHO" } }
           })pb",
      &req);
  GetValuesV2Handler handler(mock_udf_client_, mock_metrics_recorder_,
                             fake_key_fetcher_manager_);
  EXPECT_CALL(mock_udf_client_,
              ExecuteCode(testing::_, testing::ElementsAre(EqualsProto(
                                          req.partitions(0).arguments(0)))))
      .WillOnce(Return("ECHO"));
  v2::GetValuesResponse resp;
  const auto result = handler.GetValues(req, &resp);
  ASSERT_TRUE(result.ok()) << "code: " << result.error_code()
                           << ", msg: " << result.error_message();

  v2::GetValuesResponse res;
  TextFormat::ParseFromString(
      R"pb(single_partition { id: 9 string_output: "ECHO" })pb", &res);
  EXPECT_THAT(resp, EqualsProto(res));
}

TEST_F(GetValuesHandlerTest, PureGRPCTestFailure) {
  v2::GetValuesRequest req;
  TextFormat::ParseFromString(
      R"pb(partitions {
             id: 9
             arguments { data { string_value: "ECHO" } }
           })pb",
      &req);
  GetValuesV2Handler handler(mock_udf_client_, mock_metrics_recorder_,
                             fake_key_fetcher_manager_);
  EXPECT_CALL(mock_udf_client_,
              ExecuteCode(testing::_, testing::ElementsAre(EqualsProto(
                                          req.partitions(0).arguments(0)))))
      .WillOnce(Return(absl::InternalError("UDF execution error")));
  v2::GetValuesResponse resp;
  const auto result = handler.GetValues(req, &resp);
  ASSERT_TRUE(result.ok()) << "code: " << result.error_code()
                           << ", msg: " << result.error_message();

  v2::GetValuesResponse res;
  TextFormat::ParseFromString(
      R"pb(single_partition {
             id: 9
             status: { code: 13 message: "UDF execution error" }
           })pb",
      &res);
  EXPECT_THAT(resp, EqualsProto(res));
}

}  // namespace
}  // namespace kv_server
