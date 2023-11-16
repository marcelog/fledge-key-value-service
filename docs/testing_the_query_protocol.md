# Testing the query protocol

## Example used in this doc

### Request

```json
{
    "metadata": { "hostname": "example.com" },
    "partitions": [
        {
            "id": 0,
            "compressionGroupId": 0,
            "arguments": [
                { "tags": ["structured", "groupNames"], "data": ["hi"] },
                { "tags": ["custom", "keys"], "data": ["hi"] }
            ]
        },
        {
            "id": 1,
            "compressionGroupId": 0,
            "arguments": [
                { "tags": ["structured", "groupNames"], "data": ["hi"] },
                { "tags": ["custom", "keys"], "data": ["hi"] }
            ]
        }
    ]
}
```

### Expected response

```json
[
    {
        "partitions": [
            {
                "id": 0,
                "keyGroupOutputs": [
                    {
                        "keyValues": {
                            "hi": {
                                "value": "Hello, world! If you are seeing this, it means you can query me successfully"
                            }
                        },
                        "tags": ["custom", "keys"]
                    }
                ]
            },
            {
                "id": 1,
                "keyGroupOutputs": [
                    {
                        "keyValues": {
                            "hi": {
                                "value": "Hello, world! If you are seeing this, it means you can query me successfully"
                            }
                        },
                        "tags": ["custom", "keys"]
                    }
                ]
            }
        ]
    }
]
```

## Setting up the test environment

Follow the [developer guide](/docs/developing_the_server.md) to run the server system in docker
containers locally. In addition, run the helper server alongside:

`cd` into the root of the repo.

```sh
bazel run -c opt //infrastructure/testing:protocol_testing_helper_server
```

For more information on how to test the query protocol with the helper server, see also the
[service definition](/infrastructure/testing/protocol_testing_helper_server.proto).

## Plaintext query ("GetValues")

```sh
BODY='{ "metadata": { "hostname": "example.com" }, "partitions": [ { "id": 0, "compressionGroupId": 0, "arguments": [ { "tags": [ "structured", "groupNames" ], "data": [ "hi" ] }, { "tags": [ "custom", "keys" ], "data": [ "hi" ] } ] }, { "id": 1, "compressionGroupId": 0, "arguments": [ { "tags": [ "structured", "groupNames" ], "data": [ "hi" ] }, { "tags": [ "custom", "keys" ], "data": [ "hi" ] } ] } ] }'
```

HTTP:

```sh
curl -vX PUT -d "$BODY"  http://localhost:51052/v2/getvalues
```

Or gRPC:

```sh
grpcurl --protoset dist/query_api_descriptor_set.pb -d '{"raw_body": {"data": "'"$(echo -n $BODY|base64 -w 0)"'"}}' -plaintext localhost:50051 kv_server.v2.KeyValueService/GetValuesHttp
```

For gRPC, use base64 --decode to convert the output to plaintext.

## Binary HTTP query ("BinaryHTTPGetValues")

First convert the request body into Binary HTTP request:

```sh
echo -n '{"is_request": true, "body": "'$(echo -n $BODY|base64 -w 0)'"}'|grpcurl -plaintext -d @   localhost:50050 kv_server.ProtocolTestingHelper/BHTTPEncapsulate
```

The result should be something like:

```json
{
    "bhttp_message": "AAAAAAAAQZZ7ICJjb250ZXh0IjogeyAic3Via2V5IjogImV4YW1wbGUuY29tIiB9LCAicGFydGl0aW9ucyI6IFsgeyAiaWQiOiAwLCAiY29tcHJlc3Npb25Hcm91cCI6IDAsICJrZXlHcm91cHMiOiBbIHsgInRhZ3MiOiBbICJzdHJ1Y3R1cmVkIiwgImdyb3VwTmFtZXMiIF0sICJrZXlMaXN0IjogWyAiaGkiIF0gfSwgeyAidGFncyI6IFsgImN1c3RvbSIsICJrZXlzIiBdLCAia2V5TGlzdCI6IFsgImhpIiBdIH0gXSB9LCB7ICJpZCI6IDEsICJjb21wcmVzc2lvbkdyb3VwIjogMCwgImtleUdyb3VwcyI6IFsgeyAidGFncyI6IFsgInN0cnVjdHVyZWQiLCAiZ3JvdXBOYW1lcyIgXSwgImtleUxpc3QiOiBbICJoaSIgXSB9LCB7ICJ0YWdzIjogWyAiY3VzdG9tIiwgImtleXMiIF0sICJrZXlMaXN0IjogWyAiaGkiIF0gfSBdIH0gXSB9"
}
```

Assign the bhttp_message output to an environment variable:

```sh
BHTTP_REQ=AAAAAAAAQZZ7ICJjb250ZXh0IjogeyAic3Via2V5IjogImV4YW1wbGUuY29tIiB9LCAicGFydGl0aW9ucyI6IFsgeyAiaWQiOiAwLCAiY29tcHJlc3Npb25Hcm91cCI6IDAsICJrZXlHcm91cHMiOiBbIHsgInRhZ3MiOiBbICJzdHJ1Y3R1cmVkIiwgImdyb3VwTmFtZXMiIF0sICJrZXlMaXN0IjogWyAiaGkiIF0gfSwgeyAidGFncyI6IFsgImN1c3RvbSIsICJrZXlzIiBdLCAia2V5TGlzdCI6IFsgImhpIiBdIH0gXSB9LCB7ICJpZCI6IDEsICJjb21wcmVzc2lvbkdyb3VwIjogMCwgImtleUdyb3VwcyI6IFsgeyAidGFncyI6IFsgInN0cnVjdHVyZWQiLCAiZ3JvdXBOYW1lcyIgXSwgImtleUxpc3QiOiBbICJoaSIgXSB9LCB7ICJ0YWdzIjogWyAiY3VzdG9tIiwgImtleXMiIF0sICJrZXlMaXN0IjogWyAiaGkiIF0gfSBdIH0gXSB9
```

Send the request to the k/v server.

HTTP:

```sh
BHTTP_RES=$(curl -svX POST --data-binary @<(echo -n $BHTTP_REQ|base64 --decode) http://localhost:51052/v2/bhttp_getvalues|base64 -w 0);echo $BHTTP_RES
```

Or gRPC:

```sh
grpcurl --protoset dist/query_api_descriptor_set.pb --protoset dist/query_api_descriptor_set.pb -d '{"raw_body": {"data": "'"$BHTTP_REQ"'"}}' -plaintext localhost:50051 kv_server.v2.KeyValueService/BinaryHttpGetValues
```

The result should look similar to:

```json
{
    "data": "AUDIAEFcAAABWHsicGFydGl0aW9ucyI6W3siaWQiOjAsImtleUdyb3VwT3V0cHV0cyI6W3sia2V5VmFsdWVzIjp7ImhpIjp7InZhbHVlIjoiSGVsbG8sIHdvcmxkISBJZiB5b3UgYXJlIHNlZWluZyB0aGlzLCBpdCBtZWFucyB5b3UgY2FuIHF1ZXJ5IG1lIHN1Y2Nlc3NmdWxseSJ9fSwidGFncyI6WyJjdXN0b20iLCJrZXlzIl19XX0seyJpZCI6MSwia2V5R3JvdXBPdXRwdXRzIjpbeyJrZXlWYWx1ZXMiOnsiaGkiOnsidmFsdWUiOiJIZWxsbywgd29ybGQhIElmIHlvdSBhcmUgc2VlaW5nIHRoaXMsIGl0IG1lYW5zIHlvdSBjYW4gcXVlcnkgbWUgc3VjY2Vzc2Z1bGx5In19LCJ0YWdzIjpbImN1c3RvbSIsImtleXMiXX1dfV19"
}
```

Assign the data to `BHTTP_RES` and decode the BHTTP layer:

```sh
echo -n '{"is_request": false, "bhttp_message": "'$BHTTP_RES'"}'|grpcurl -plaintext -d @  localhost:50050 kv_server.ProtocolTestingHelper/BHTTPDecapsulate
```

Result:

```json
{
    "body": "AAABWHsicGFydGl0aW9ucyI6W3siaWQiOjAsImtleUdyb3VwT3V0cHV0cyI6W3sia2V5VmFsdWVzIjp7ImhpIjp7InZhbHVlIjoiSGVsbG8sIHdvcmxkISBJZiB5b3UgYXJlIHNlZWluZyB0aGlzLCBpdCBtZWFucyB5b3UgY2FuIHF1ZXJ5IG1lIHN1Y2Nlc3NmdWxseSJ9fSwidGFncyI6WyJjdXN0b20iLCJrZXlzIl19XX0seyJpZCI6MSwia2V5R3JvdXBPdXRwdXRzIjpbeyJrZXlWYWx1ZXMiOnsiaGkiOnsidmFsdWUiOiJIZWxsbywgd29ybGQhIElmIHlvdSBhcmUgc2VlaW5nIHRoaXMsIGl0IG1lYW5zIHlvdSBjYW4gcXVlcnkgbWUgc3VjY2Vzc2Z1bGx5In19LCJ0YWdzIjpbImN1c3RvbSIsImtleXMiXX1dfV19"
}
```

The returned data is base64 encoded. Decode the content with base64 --decode and you should see the
expected response. Note that the first 32 bits of the response stores the size of the response. That
is part of the compression layer, which is not turned on in this exercise.

## Oblivious HTTP query ("ObliviousGetValues")

Oblivious HTTP request is encrypted with a public key as one of the initial input. The testing
public key can be found here or by calling:

```sh
grpcurl -plaintext localhost:50050 kv_server.ProtocolTestingHelper/GetTestConfig
```

To build the request:

```sh
echo -n '{"body": "'$BHTTP_REQ'", "key_id": 1, "public_key": "MeHwWnQBAhFSIOmvkY9zhnSuyV9U224E63Baro55gVU="}'|grpcurl -plaintext -d @  localhost:50050 kv_server.ProtocolTestingHelper/OHTTPEncapsulate
```

The output is similar to:

```json
{
    "ohttp_request": "AQAgAAEAATVl8Lz2p4B27AbFoIT+R2H7jRCp+Q/c87qruxKbXLRnNdMHGZjJLCaNSs9caPvgHpo4uYB4g9fdL/a+/mJglyME1B7ngo5mJX7puHHl8aoEWeIugq/pJjvrGI38P4z3gQlb4mBinGPhqOTdH+xvfMss5b44PwqacbjZYJ3eb1hDjXsgmsTGa0ZzlFUymqI/9P7ZsdQAwtD9cxuywZsKF9A1aRhwRuA1Y/9iMCmpJlX9SmGeN8FptL4VnoAo4eJwPSS6Z/OHPsfP/d6CQZH4hGudjGgtbzzPItD/drK8MMiCKq3PPffCgcDXP/0u9SWXOim3/gzMDsU/uh47JhbYhjhOQ4DJAaxcG/DQqRqLKd1Z4sHechv9xdoJJbV7laPoxyEFWMiWwSTHL+kZVRBc0uQSWBRgyDxxknjl71g/3SeLOjz9ovC4DOouLFAWbWMpgxRHJRA4GsevdBq3Od3I7AEvtJ2AfIMpo3tsch7iJzcaORV0Ml/TgASSdliaThYj2e/G38GQYdzHQfHmcB6r+2M0DC/bEN29JEJayWIfl7DUOs1U1GLLh0+y7+mH85zFhu4lb4lX0PtzcN/TrNOtB19d/YQ6Mv2n+Dbea6S9hg==",
    "context_token": "1675366132"
}
```

The context_token is used as one input to decode the response later.

Call the k/v server with the request (stored as OHTTP_REQ env var):

HTTP:

```sh
OHTTP_RES=$(curl -svX POST --data-binary @<(echo -n $OHTTP_REQ|base64 --decode) http://localhost:51052/v2/oblivious_getvalues|base64 -w 0);echo $OHTTP_RES
```

Or gRPC:

```sh
grpcurl --protoset dist/query_api_descriptor_set.pb -d '{"raw_body": {"data": "'"$OHTTP_REQ"'"}}' -plaintext localhost:50051 kv_server.v2.KeyValueService/ObliviousGetValues
```

Result is similar to:

```json
{
    "contentType": "message/ohttp-res",
    "data": "TFZDIlvBIBUfq4fzHvWwa58pjRrMmyE8mkfQshA4N9SDD6Ts28KigYIU3OcV30/+ZrCmStdCg/BcgY59Rod6TCLkSfI32Gk25oY+9I+vVxpj7FG67vWoQdbee7FUvn7TxsrdCSd9ulwpixbE7KtSw7MmX6Y0y0I7xHkx9N7zKSu/cmabg9ZgdQFipDUdBaBNPScNOrwh6b6nZhWHbW/oUWCFMHtDa9sLVP5cNi9oMjb7AFdK5NKeq1qiCuhKTi3RZ7bKNbk98JnmyGI6OwAs2631Gl+S0npPR/KDblWQJ2ZCI0maek0zIVPhWLs2/kA+etwOCmRzB7syxDwwT3MRDo6wWJdcKKHC8Y48XgKEv5NvTLC39tsEniSvPdymevNfG2PTLJDKaAocb/WVLj5wm08UNjAv+Pxu8a+wRDxP+kxm+TnKMCPapnRcplU4D3+VH4YdhQbF2V1kwsyfBQxQMr4XX1w6n87ah8qUBucjveKPSa6kqKVSk2w261McQobJW54="
}
```

Decode the response:

```sh
echo -n '{"context_token": 1675366132, "ohttp_response": "'$OHTTP_RES'"}'|grpcurl -plaintext -d @  localhost:50050 kv_server.ProtocolTestingHelper/OHTTPDecapsulate
```

Result example:

```json
{
    "body": "AUDIAEFcAAABWHsicGFydGl0aW9ucyI6W3siaWQiOjAsImtleUdyb3VwT3V0cHV0cyI6W3sia2V5VmFsdWVzIjp7ImhpIjp7InZhbHVlIjoiSGVsbG8sIHdvcmxkISBJZiB5b3UgYXJlIHNlZWluZyB0aGlzLCBpdCBtZWFucyB5b3UgY2FuIHF1ZXJ5IG1lIHN1Y2Nlc3NmdWxseSJ9fSwidGFncyI6WyJjdXN0b20iLCJrZXlzIl19XX0seyJpZCI6MSwia2V5R3JvdXBPdXRwdXRzIjpbeyJrZXlWYWx1ZXMiOnsiaGkiOnsidmFsdWUiOiJIZWxsbywgd29ybGQhIElmIHlvdSBhcmUgc2VlaW5nIHRoaXMsIGl0IG1lYW5zIHlvdSBjYW4gcXVlcnkgbWUgc3VjY2Vzc2Z1bGx5In19LCJ0YWdzIjpbImN1c3RvbSIsImtleXMiXX1dfV19"
}
```

The body here is in Binary HTTP format. Decapsulate it the same way as the Binary HTTP response:

```sh
echo -n '{"is_request": false, "bhttp_message": "'$BHTTP_RES'"}'|grpcurl -plaintext -d @  localhost:50050 kv_server.ProtocolTestingHelper/BHTTPDecapsulate
```

The returned data is base64 encoded. Decode the content with base64 --decode and you should see the
expected response. Note that the first 32 bits of the response stores the size of the response. That
is part of the compression layer, which is not turned on in this exercise.
