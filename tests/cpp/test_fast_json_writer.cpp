#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "vector_otel/core/fast_json_writer.hpp"

namespace vector_otel {
namespace core {

TEST(FastJsonWriterTest, EmptyObject) {
  FastJsonWriter writer;
  writer.StartObject();
  writer.EndObject();
  EXPECT_EQ(writer.Str(), "{}");
}

TEST(FastJsonWriterTest, EmptyArray) {
  FastJsonWriter writer;
  writer.StartArray();
  writer.EndArray();
  EXPECT_EQ(writer.Str(), "[]");
}

TEST(FastJsonWriterTest, SimpleObject) {
  FastJsonWriter writer;
  writer.StartObject();
  writer.Key("name");
  writer.String("vector");
  writer.Key("version");
  writer.Int64(1);
  writer.Key("active");
  writer.Bool(true);
  writer.EndObject();

  EXPECT_EQ(writer.Str(), "{\"name\":\"vector\",\"version\":1,\"active\":true}");
}

TEST(FastJsonWriterTest, Int64Boundaries) {
  FastJsonWriter writer;
  writer.StartArray();
  writer.Int64(std::numeric_limits<int64_t>::min());  // -9223372036854775808
  writer.Int64(std::numeric_limits<int64_t>::max());  // 9223372036854775807
  writer.Int64(0);
  writer.Int64(-1);
  writer.Uint64(std::numeric_limits<uint64_t>::max());  // 18446744073709551615
  writer.EndArray();

  EXPECT_EQ(writer.Str(), "[-9223372036854775808,9223372036854775807,0,-1,18446744073709551615]");
}

TEST(FastJsonWriterTest, Escaping) {
  FastJsonWriter writer;
  writer.StartObject();
  // Key escaping
  writer.Key("quote\"key");
  // Value escaping: quotes, backslash, control chars, tab, newline, cr, bs, ff
  writer.String(
      "Quotes: \"hello\", Backslash: \\, Tabs: \t, Newline: \n, CR: \r, BS: "
      "\b, FF: \f");
  writer.Key("ctrl");
  // Control char \x01 and \x1f
  writer.String(std::string_view("\x01\x1f", 2));
  writer.EndObject();

  EXPECT_EQ(writer.Str(),
            "{\"quote\\\"key\":\"Quotes: \\\"hello\\\", Backslash: \\\\, Tabs: "
            "\\t, Newline: \\n, "
            "CR: \\r, BS: \\b, FF: \\f\",\"ctrl\":\"\\u0001\\u001f\"}");
}

TEST(FastJsonWriterTest, DoubleFormatting) {
  FastJsonWriter writer;
  writer.StartArray();
  writer.Double(0.0);
  writer.Double(123.456);
  writer.Double(-42.5);
  writer.Double(std::nan(""));
  writer.Double(INFINITY);
  writer.EndArray();

  EXPECT_EQ(writer.Str(), "[0,123.456,-42.5,null,null]");
}

TEST(FastJsonWriterTest, BoolAndNull) {
  FastJsonWriter writer;
  writer.StartArray();
  writer.Bool(true);
  writer.Bool(false);
  writer.Null();
  writer.EndArray();

  EXPECT_EQ(writer.Str(), "[true,false,null]");
}

TEST(FastJsonWriterTest, NestedStructures) {
  FastJsonWriter writer;
  writer.StartObject();
  writer.Key("events");
  writer.StartArray();

  writer.StartObject();
  writer.Key("id");
  writer.Int64(1);
  writer.Key("tags");
  writer.StartArray();
  writer.String("prod");
  writer.String("us-east");
  writer.EndArray();
  writer.EndObject();

  writer.StartObject();
  writer.Key("id");
  writer.Int64(2);
  writer.Key("tags");
  writer.StartArray();
  writer.EndArray();
  writer.EndObject();

  writer.EndArray();
  writer.EndObject();

  EXPECT_EQ(writer.Str(),
            "{\"events\":[{\"id\":1,\"tags\":[\"prod\",\"us-east\"]},{\"id\":2,"
            "\"tags\":[]}]}");
}

TEST(FastJsonWriterTest, RawJson) {
  FastJsonWriter writer;
  writer.StartObject();
  writer.Key("raw_obj");
  writer.RawJson("{\"nested\":[1,2,3]}");
  writer.Key("raw_arr");
  writer.StartArray();
  writer.RawJson("{\"item\":1}");
  writer.RawJson("{\"item\":2}");
  writer.EndArray();
  writer.EndObject();

  EXPECT_EQ(writer.Str(),
            "{\"raw_obj\":{\"nested\":[1,2,3]},\"raw_arr\":[{\"item\":1},{"
            "\"item\":2}]}");
}

TEST(FastJsonWriterTest, ResetAndReuse) {
  FastJsonWriter writer;
  writer.StartObject();
  writer.Key("first");
  writer.Int64(1);
  writer.EndObject();
  EXPECT_EQ(writer.Str(), "{\"first\":1}");

  writer.Reset();
  EXPECT_EQ(writer.Str(), "");

  writer.StartArray();
  writer.String("second");
  writer.EndArray();
  EXPECT_EQ(writer.Str(), "[\"second\"]");
}

TEST(FastJsonWriterTest, InitialReservationAndTrimOversizedBuffer) {
  core::FastJsonWriter writer;
  EXPECT_GE(writer.Capacity(), 4096u);

  std::string huge(17 * 1024 * 1024, 'a');
  writer.String(huge);
  EXPECT_GT(writer.Capacity(), core::FastJsonWriter::kMaxRetainedCapacity);

  writer.Trim();
  // Allocators round reserve() up to a size class. This must still fail if
  // Trim() stops shrinking (the preceding oversize step is >16 MiB).
  EXPECT_LE(writer.Capacity(), 4096u + 64u);
  EXPECT_GE(writer.Capacity(), 4096u);
  EXPECT_EQ(writer.Str(), "");
}

TEST(FastJsonWriterTest, WriteEscapedStringMixedChunksAndControlChars) {
  FastJsonWriter writer;
  writer.StartArray();
  writer.String("plain_ascii_span_without_escapes");
  writer.String("chunk1\"chunk2\\chunk3\nchunk4\tchunk5\rchunk6\bchunk7\fend");
  writer.String(std::string_view("\x00prefix\x07middle\x1bsuffix\x1f", 22));
  writer.String("\"\\");
  writer.String("");
  writer.EndArray();

  EXPECT_EQ(writer.Str(),
            "[\"plain_ascii_span_without_escapes\","
            "\"chunk1\\\"chunk2\\\\chunk3\\nchunk4\\tchunk5\\rchunk6\\bchunk7\\"
            "fend\","
            "\"\\u0000prefix\\u0007middle\\u001bsuffix\\u001f\","
            "\"\\\"\\\\\","
            "\"\"]");
}

}  // namespace core
}  // namespace vector_otel
