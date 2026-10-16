#!/usr/bin/env python3
"""Generate deterministic OTLP/JSON ExportLogsServiceRequest fixtures (NDJSON).

One line == one batch (what Vector's stdin source / a Kafka message would carry).

  perf   : realistic batches for throughput measurement
  edge   : small hand-built set exercising the data-type pitfalls
           (string int64s, AnyValue variants, missing service.name, ...)
"""
import argparse
import json
import random
import sys

BASE_NS = 1_293_840_000_123_456_789  # 2011-01-01T00:00:00.123456789Z


def kv(key, **value):
    return {"key": key, "value": value}


def perf_batch(rng, batch_idx, records):
    resource = {
        "attributes": [
            kv("service.name", stringValue=f"svc-{batch_idx % 20}"),
            kv("service.version", stringValue="1.4.2"),
            kv("k8s.cluster.name", stringValue="prod-ap-southeast-2"),
            kv("k8s.namespace.name", stringValue="payments"),
            kv("k8s.pod.name", stringValue=f"pod-{batch_idx % 50}-abcde"),
            kv("k8s.node.name", stringValue=f"node-{batch_idx % 12}"),
            kv("deployment.environment.name", stringValue="production"),
            kv("process.pid", intValue=str(1000 + batch_idx % 500)),
        ]
    }
    recs = []
    for i in range(records):
        t = BASE_NS + (batch_idx * records + i) * 1_000_003
        recs.append(
            {
                "timeUnixNano": str(t),
                "observedTimeUnixNano": str(t + 1500),
                "severityNumber": rng.choice([5, 9, 9, 9, 13, 17]),
                "severityText": rng.choice(["DEBUG", "INFO", "INFO", "INFO", "WARN", "ERROR"]),
                "body": {"stringValue": f"request {i} handled in {rng.randint(1, 900)}ms path=/api/v1/items/{rng.randint(1, 10**6)}"},
                "attributes": [
                    kv("http.method", stringValue=rng.choice(["GET", "POST", "PUT"])),
                    kv("http.status_code", intValue=str(rng.choice([200, 200, 201, 404, 500]))),
                    kv("http.route", stringValue="/api/v1/items/{id}"),
                    kv("net.peer.ip", stringValue=f"10.{rng.randint(0, 255)}.{rng.randint(0, 255)}.{rng.randint(1, 254)}"),
                    kv("retry", boolValue=rng.random() < 0.1),
                ],
                "traceId": f"{rng.getrandbits(128):032x}",
                "spanId": f"{rng.getrandbits(64):016x}",
                "flags": 1,
            }
        )
    return {
        "resourceLogs": [
            {
                "resource": resource,
                "schemaUrl": "https://opentelemetry.io/schemas/1.26.0",
                "scopeLogs": [
                    {
                        "scope": {"name": "com.acme.payments", "version": "2.0.1", "attributes": [kv("scope.attr", stringValue="x")]},
                        "schemaUrl": "https://opentelemetry.io/schemas/1.26.0",
                        "logRecords": recs,
                    }
                ],
            }
        ]
    }


def edge_batches():
    big = "9223372036854775807"  # int64 max: must survive as-is
    return [
        # 1. every AnyValue variant + int64 extremes + structured body
        {"resourceLogs": [{
            "resource": {"attributes": [kv("service.name", stringValue="edge-svc")]},
            "scopeLogs": [{
                "scope": {"name": "edge", "version": "1"},
                "logRecords": [{
                    "timeUnixNano": str(BASE_NS),
                    "severityNumber": 9, "severityText": "INFO",
                    "body": {"kvlistValue": {"values": [kv("k", stringValue="v")]}},
                    "attributes": [
                        kv("s", stringValue="str"),
                        kv("i", intValue=big),
                        kv("neg", intValue="-9223372036854775808"),
                        kv("d", doubleValue=3.14159),
                        kv("b_true", boolValue=True),
                        kv("b_false", boolValue=False),
                        kv("arr", arrayValue={"values": [{"stringValue": "a"}, {"intValue": "2"}]}),
                        kv("map", kvlistValue={"values": [kv("n", intValue="1")]}),
                        kv("bytes", bytesValue="AAEC"),
                        kv("unicode", stringValue="héllo \"quoted\" \\ \n tab\t ✓"),
                    ],
                    "traceId": "5b8efff798038103d269b633813fc60c",
                    "spanId": "eee19b7ec3c1b174",
                    "flags": 1,
                }],
            }],
        }]},
        # 2. missing service.name, no scope, timeUnixNano absent -> observed fallback, empty ids
        {"resourceLogs": [{
            "resource": {"attributes": []},
            "scopeLogs": [{"logRecords": [{
                "observedTimeUnixNano": str(BASE_NS + 5),
                "body": {"stringValue": ""},
            }]}],
        }]},
        # 3. timeUnixNano "0" (unset per spec) + numeric (non-string) timestamp encoding
        {"resourceLogs": [{
            "resource": {"attributes": [kv("service.name", stringValue="ts-svc")]},
            "scopeLogs": [{"scope": {"name": "ts"}, "logRecords": [
                {"timeUnixNano": "0", "observedTimeUnixNano": str(BASE_NS), "body": {"stringValue": "zero-ts"}},
                {"timeUnixNano": BASE_NS, "body": {"stringValue": "numeric-ts"}},
            ]}],
        }]},
        # 4. empty batches
        {"resourceLogs": []},
        {"resourceLogs": [{"resource": {}, "scopeLogs": []}]},
        # 5. multi resource / multi scope fan-out (2 resources x 2 scopes x 1 record = 4 events)
        {"resourceLogs": [
            {"resource": {"attributes": [kv("service.name", stringValue=f"r{r}")]},
             "scopeLogs": [{"scope": {"name": f"s{s}"}, "logRecords": [
                 {"timeUnixNano": str(BASE_NS + r * 10 + s), "body": {"stringValue": f"r{r}s{s}"}}
             ]} for s in range(2)]}
            for r in range(2)
        ]},
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=["perf", "edge"])
    ap.add_argument("--batches", type=int, default=1000)
    ap.add_argument("--records", type=int, default=100)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()
    out = sys.stdout
    if args.kind == "edge":
        for b in edge_batches():
            out.write(json.dumps(b, separators=(",", ":")) + "\n")
        return
    rng = random.Random(args.seed)
    for i in range(args.batches):
        out.write(json.dumps(perf_batch(rng, i, args.records), separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
