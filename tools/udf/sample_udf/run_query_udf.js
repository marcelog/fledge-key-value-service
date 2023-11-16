/**
 * Copyright 2022 Google LLC
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

function HandleRequest(executionMetadata, ...input) {
    let keyGroupOutputs = [];
    for (const keyGroup of input) {
        let keyGroupOutput = {};
        if (!keyGroup.tags.includes("custom") || !keyGroup.tags.includes("queries")) {
            continue;
        }
        keyGroupOutput.tags = keyGroup.tags;
        if (!Array.isArray(keyGroup.data) || !keyGroup.data.length) {
            continue;
        }

        // Get the first key in the data.
        const runQueryArray = runQuery(keyGroup.data[0]);
        // runQuery returns an array of strings when successful and "code" on failure.
        // Ignore failures and only add successful runQuery results to output.
        if (Array.isArray(runQueryArray) && runQueryArray.length) {
            const keyValuesOutput = {};
            keyValuesOutput["result"] = { "value": runQueryArray };
            keyGroupOutput.keyValues = keyValuesOutput;
            keyGroupOutputs.push(keyGroupOutput);
        }
    }
    return { keyGroupOutputs, udfOutputApiVersion: 1 };
}
