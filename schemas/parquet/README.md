# OpenTelemetry Parquet Schemas for Vector S3 Lakehouse

This directory provides canonical Apache Parquet schema definition files (`.schema`) designed for DataDog Vector's `aws_s3`, `gcp_cloud_storage`, and `azure_blob` sinks using `batch_encoding.codec = "parquet"`.

These schemas align directly with the table layouts used by the OpenTelemetry ClickHouse exporter and lakehouse query engines (Amazon Athena, Trino, Presto, Apache Spark, and DuckDB).

---

## Schema Files

| Schema File | Telemetry Signal | Record Type | Canonical Alignment |
|:---|:---|:---|:---|
| [`logs.schema`](logs.schema) | Logs | Log records (`LogRecord`) | `logs_json_table.sql` |
| [`traces.schema`](traces.schema) | Traces | Span records (`SpanRecord`) | `traces_json_table.sql` |
| [`metrics.schema`](metrics.schema) | Metrics | Data points (`MetricRecord`) | `metrics_*_table.sql` |

---

## Vector Sink Configuration Example

```yaml
sinks:
  s3_parquet_logs:
    type: aws_s3
    inputs: ["flatten_otel_logs"]
    bucket: "my-telemetry-lakehouse"
    region: "us-east-1"
    key_prefix: "logs/year=%Y/month=%m/day=%d/"
    compression: "none" # Column-level compression is handled by batch_encoding
    encoding:
      codec: "text" # Dummy codec bypassed by batch_encoding
    batch_encoding:
      codec: "parquet"
      schema_file: "schemas/parquet/logs.schema"
      compression:
        algorithm: "zstd"
        level: 3
```

---

## Field Specifications & Types

### 1. Timestamps
- `Timestamp`: Formatted as an ISO-8601 UTC string with 9 fractional digits (nanoseconds), e.g. `2026-10-16T12:00:00.123456789Z`.
- `processed_at`: Ingestion timestamp formatted as ISO-8601 UTC with nanosecond precision.

### 2. Identifiers & 64-bit Integers
- `TraceId`: 16-byte hex string (32 characters).
- `SpanId`: 8-byte hex string (16 characters).
- `Duration`: 64-bit signed integer (`int64`) representing span duration in nanoseconds.
- `TraceFlags`: 64-bit signed integer (`int64`).
- `SeverityNumber`: 64-bit signed integer (`int64`) (range 1–24).

### 3. Attribute Maps
- `ResourceAttributes`, `ScopeAttributes`, `LogAttributes`, and `SpanAttributes` are serialized as compact JSON strings inside Parquet strings (`binary (STRING)`), ensuring full compatibility across query engines without schema evolution issues.

---

## Querying with DuckDB / Amazon Athena

### DuckDB Example:
```sql
SELECT
    Timestamp,
    ServiceName,
    SeverityText,
    Body,
    json_extract_string(LogAttributes, '$.http.status_code') AS status_code
FROM read_parquet('s3://my-telemetry-lakehouse/logs/*/*/*/*.parquet')
WHERE ServiceName = 'payment-service'
ORDER BY Timestamp DESC
LIMIT 50;
```
