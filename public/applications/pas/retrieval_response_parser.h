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

#ifndef PUBLIC_APPLICATIONS_PAS_RETRIEVAL_RESPONSE_PARSER_H_
#define PUBLIC_APPLICATIONS_PAS_RETRIEVAL_RESPONSE_PARSER_H_

#include <string>

#include "absl/status/statusor.h"
#include "public/query/v2/get_values_v2.pb.h"

namespace kv_server::application_pas {

// Returns the output from retrieval UDF from the response. Error if the
// response contains an error.
absl::StatusOr<std::string> GetRetrievalOutput(
    const v2::GetValuesResponse& response);

}  // namespace kv_server::application_pas

#endif  // PUBLIC_APPLICATIONS_PAS_RETRIEVAL_RESPONSE_PARSER_H_
