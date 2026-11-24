// Copyright (C) 2026 Google LLC
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

#include "icing/portable/gzip_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/portable/zlib.h"
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace icing {
namespace lib {
namespace protobuf_ports {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Ne;

constexpr size_t kOutputBufferSize = 1024;

std::string MakePayload(int seed, int size) {
  std::string payload;
  payload.reserve(size);
  while (payload.size() < static_cast<size_t>(size)) {
    payload += "icing_gzip_stream_test_payload_" +
               std::to_string((payload.size() * 7 + seed) % 101) + "_";
  }
  payload.resize(size);
  return payload;
}

GzipOutputStream::Options MakeOptions(GzipOutputStream::Format format,
                                      int compression_level, int mem_level,
                                      int buffer_size) {
  GzipOutputStream::Options options;
  options.format = format;
  options.compression_level = compression_level;
  options.mem_level = mem_level;
  options.buffer_size = buffer_size;
  return options;
}

bool WriteAll(google::protobuf::io::ZeroCopyOutputStream* stream,
              const std::string& data) {
  size_t written = 0;
  while (written < data.size()) {
    void* buffer;
    int size;
    if (!stream->Next(&buffer, &size)) {
      return false;
    }
    size_t to_copy = std::min(static_cast<size_t>(size), data.size() - written);
    std::memcpy(buffer, data.data() + written, to_copy);
    written += to_copy;
    stream->BackUp(static_cast<int>(size - to_copy));
  }
  return true;
}

std::string CompressWithNewStream(const std::string& data,
                                  const GzipOutputStream::Options& options) {
  std::string compressed;
  google::protobuf::io::StringOutputStream string_stream(&compressed);
  GzipOutputStream gzip_stream(&string_stream, options);
  EXPECT_THAT(WriteAll(&gzip_stream, data), IsTrue());
  EXPECT_THAT(gzip_stream.Close(), IsTrue());
  return compressed;
}

std::string ReadAll(GzipInputStream* stream) {
  std::string out;
  const void* data;
  int size;
  while (stream->Next(&data, &size)) {
    out.append(static_cast<const char*>(data), size);
  }
  return out;
}

std::string Decompress(const std::string& compressed) {
  google::protobuf::io::ArrayInputStream array_stream(
      compressed.data(), static_cast<int>(compressed.size()));
  std::vector<char> buffer(kOutputBufferSize);
  GzipInputStream gzip_stream(&array_stream, GzipInputStream::AUTO,
                              buffer.data(), buffer.size());
  return ReadAll(&gzip_stream);
}

TEST(GzipStreamTest, SingleStreamRoundTrip) {
  std::string payload = MakePayload(/*seed=*/1, /*size=*/10000);
  for (GzipOutputStream::Format format :
       {GzipOutputStream::ZLIB, GzipOutputStream::GZIP}) {
    std::string compressed = CompressWithNewStream(
        payload, MakeOptions(format, /*compression_level=*/3,
                             /*mem_level=*/8, /*buffer_size=*/4096));
    EXPECT_THAT(Decompress(compressed), Eq(payload));
  }
}

TEST(GzipStreamTest, NextAfterEndOfStreamKeepsReturningFalse) {
  std::string payload = MakePayload(/*seed=*/2, /*size=*/5000);
  std::string compressed = CompressWithNewStream(
      payload, MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                           /*mem_level=*/8, /*buffer_size=*/4096));

  google::protobuf::io::ArrayInputStream array_stream(
      compressed.data(), static_cast<int>(compressed.size()));
  std::vector<char> buffer(kOutputBufferSize);
  GzipInputStream gzip_stream(&array_stream, GzipInputStream::AUTO,
                              buffer.data(), buffer.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload));
  EXPECT_THAT(gzip_stream.ByteCount(),
              Eq(static_cast<int64_t>(payload.size())));

  const void* data;
  int size;
  EXPECT_THAT(gzip_stream.Next(&data, &size), IsFalse());
  EXPECT_THAT(gzip_stream.Next(&data, &size), IsFalse());
  EXPECT_THAT(gzip_stream.ByteCount(),
              Eq(static_cast<int64_t>(payload.size())));
  EXPECT_THAT(gzip_stream.ZlibErrorCode(), Eq(Z_STREAM_END));
}

TEST(GzipStreamTest, ConcatenatedStreams) {
  std::string payload1 = MakePayload(/*seed=*/3, /*size=*/3000);
  std::string payload2 = MakePayload(/*seed=*/4, /*size=*/7000);
  std::string compressed =
      CompressWithNewStream(
          payload1, MakeOptions(GzipOutputStream::ZLIB,
                                /*compression_level=*/3,
                                /*mem_level=*/8, /*buffer_size=*/4096)) +
      CompressWithNewStream(payload2,
                            MakeOptions(GzipOutputStream::GZIP,
                                        /*compression_level=*/0,
                                        /*mem_level=*/8, /*buffer_size=*/4096));

  for (int block_size : {1, 7, 64, 1 << 20}) {
    google::protobuf::io::ArrayInputStream array_stream(
        compressed.data(), static_cast<int>(compressed.size()), block_size);
    std::vector<char> buffer(kOutputBufferSize);
    GzipInputStream gzip_stream(&array_stream, GzipInputStream::AUTO,
                                buffer.data(), buffer.size());
    EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload1 + payload2))
        << "block_size=" << block_size;
    EXPECT_THAT(gzip_stream.ByteCount(),
                Eq(static_cast<int64_t>(payload1.size() + payload2.size())))
        << "block_size=" << block_size;
  }
}

TEST(GzipStreamTest, InputStreamResetReadsIndependentStreams) {
  std::string payload1 = MakePayload(/*seed=*/5, /*size=*/9000);
  std::string payload2 = MakePayload(/*seed=*/6, /*size=*/100);
  std::string compressed1 = CompressWithNewStream(
      payload1, MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                            /*mem_level=*/1, /*buffer_size=*/4096));
  std::string compressed2 = CompressWithNewStream(
      payload2, MakeOptions(GzipOutputStream::GZIP, /*compression_level=*/9,
                            /*mem_level=*/8, /*buffer_size=*/4096));
  std::string corrupted = compressed1;
  corrupted[corrupted.size() / 2] ^= 0xFF;

  std::vector<char> buffer1(kOutputBufferSize);
  std::vector<char> buffer2(2 * kOutputBufferSize);
  google::protobuf::io::ArrayInputStream array_stream1(
      compressed1.data(), static_cast<int>(compressed1.size()));
  GzipInputStream gzip_stream(&array_stream1, GzipInputStream::AUTO,
                              buffer1.data(), buffer1.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload1));

  google::protobuf::io::ArrayInputStream array_stream2(
      compressed2.data(), static_cast<int>(compressed2.size()));
  gzip_stream.Reset(&array_stream2, buffer2.data(), buffer2.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload2));
  EXPECT_THAT(gzip_stream.ByteCount(),
              Eq(static_cast<int64_t>(payload2.size())));

  // A corrupted stream must not prevent reuse.
  google::protobuf::io::ArrayInputStream corrupted_stream(
      corrupted.data(), static_cast<int>(corrupted.size()));
  gzip_stream.Reset(&corrupted_stream, buffer1.data(), buffer1.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Ne(payload1));

  google::protobuf::io::ArrayInputStream array_stream3(
      compressed1.data(), static_cast<int>(compressed1.size()));
  gzip_stream.Reset(&array_stream3, buffer1.data(), buffer1.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload1));
  EXPECT_THAT(gzip_stream.ByteCount(),
              Eq(static_cast<int64_t>(payload1.size())));
}

TEST(GzipStreamTest, InputStreamResetBeforeFirstRead) {
  std::string payload = MakePayload(/*seed=*/7, /*size=*/2000);
  std::string compressed = CompressWithNewStream(
      payload, MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                           /*mem_level=*/8, /*buffer_size=*/4096));

  google::protobuf::io::ArrayInputStream unused_stream(compressed.data(), 0);
  std::vector<char> buffer(kOutputBufferSize);
  GzipInputStream gzip_stream(&unused_stream, GzipInputStream::AUTO,
                              buffer.data(), buffer.size());
  google::protobuf::io::ArrayInputStream array_stream(
      compressed.data(), static_cast<int>(compressed.size()));
  gzip_stream.Reset(&array_stream, buffer.data(), buffer.size());
  EXPECT_THAT(ReadAll(&gzip_stream), Eq(payload));
}

TEST(GzipStreamTest, OutputStreamResetProducesSameBytesAsNewStream) {
  struct Write {
    GzipOutputStream::Options options;
    std::string payload;
  };
  std::vector<Write> writes = {
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                   /*mem_level=*/1, /*buffer_size=*/100),
       MakePayload(/*seed=*/10, /*size=*/100)},
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/0,
                   /*mem_level=*/1, /*buffer_size=*/64 * 1024),
       MakePayload(/*seed=*/11, /*size=*/100000)},
      // Regression test: shrinking buffer_size at level 0 after a larger write
      // must preserve stored-block chunking and stay byte-identical to a fresh
      // GzipOutputStream.
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/0,
                   /*mem_level=*/1, /*buffer_size=*/4096),
       MakePayload(/*seed=*/16, /*size=*/10000)},
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/9,
                   /*mem_level=*/1, /*buffer_size=*/50),
       MakePayload(/*seed=*/12, /*size=*/50)},
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                   /*mem_level=*/1, /*buffer_size=*/4096),
       MakePayload(/*seed=*/13, /*size=*/4096)},
      {MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                   /*mem_level=*/8, /*buffer_size=*/4096),
       MakePayload(/*seed=*/14, /*size=*/30000)},
      {MakeOptions(GzipOutputStream::GZIP, /*compression_level=*/1,
                   /*mem_level=*/8, /*buffer_size=*/4096),
       MakePayload(/*seed=*/15, /*size=*/30000)},
      {MakeOptions(GzipOutputStream::GZIP, /*compression_level=*/1,
                   /*mem_level=*/8, /*buffer_size=*/0),
       ""},
  };

  std::unique_ptr<GzipOutputStream> reused_stream;
  for (size_t i = 0; i < writes.size(); ++i) {
    const Write& write = writes[i];
    std::string expected = CompressWithNewStream(write.payload, write.options);

    std::string actual;
    google::protobuf::io::StringOutputStream string_stream(&actual);
    if (reused_stream == nullptr) {
      reused_stream =
          std::make_unique<GzipOutputStream>(&string_stream, write.options);
    } else {
      ASSERT_THAT(reused_stream->Reset(&string_stream, write.options),
                  IsTrue());
    }
    ASSERT_THAT(WriteAll(reused_stream.get(), write.payload), IsTrue());
    ASSERT_THAT(reused_stream->Finish(), IsTrue());

    EXPECT_THAT(actual, Eq(expected)) << "write #" << i;
    EXPECT_THAT(Decompress(actual), Eq(write.payload)) << "write #" << i;
  }
}

TEST(GzipStreamTest, OutputStreamFinishedStreamRejectsWriteAndFlushUntilReset) {
  std::string payload = MakePayload(/*seed=*/20, /*size=*/1000);
  GzipOutputStream::Options options =
      MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                  /*mem_level=*/8, /*buffer_size=*/1024);

  std::string compressed;
  std::unique_ptr<GzipOutputStream> gzip_stream;
  {
    // Destroy sub_stream immediately after Finish() to verify that Flush(),
    // Next(), Finish(), and Close() do not dereference a dangling sub_stream_.
    google::protobuf::io::StringOutputStream string_stream(&compressed);
    gzip_stream = std::make_unique<GzipOutputStream>(&string_stream, options);
    ASSERT_THAT(WriteAll(gzip_stream.get(), payload), IsTrue());
    ASSERT_THAT(gzip_stream->Finish(), IsTrue());
  }

  void* data;
  int size;
  EXPECT_THAT(gzip_stream->Next(&data, &size), IsFalse());
  EXPECT_THAT(gzip_stream->Flush(), IsFalse());
  EXPECT_THAT(gzip_stream->Finish(), IsFalse());
  EXPECT_THAT(gzip_stream->Close(), IsFalse());
  EXPECT_THAT(Decompress(compressed), Eq(payload));

  std::string compressed2;
  google::protobuf::io::StringOutputStream string_stream2(&compressed2);
  ASSERT_THAT(gzip_stream->Reset(&string_stream2, options), IsTrue());
  ASSERT_THAT(WriteAll(gzip_stream.get(), payload), IsTrue());
  ASSERT_THAT(gzip_stream->Close(), IsTrue());
  EXPECT_THAT(compressed2, Eq(compressed));
}

TEST(GzipStreamTest, WindowBits14ProducesValidZlibHeaderAndRoundTrips) {
  std::string payload = MakePayload(/*seed=*/30, /*size=*/24 * 1024);
  GzipOutputStream::Options options =
      MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                  /*mem_level=*/1, /*buffer_size=*/4096);
  options.window_bits = 14;

  std::string compressed = CompressWithNewStream(payload, options);
  ASSERT_THAT(compressed.size(), Ge(2u));
  // RFC 1950 CMF byte encodes CINFO = window_bits - 8 = 6, CM = 8 -> 0x68.
  EXPECT_THAT(static_cast<uint8_t>(compressed[0]), Eq(0x68));
  EXPECT_THAT(Decompress(compressed), Eq(payload));
}

TEST(GzipStreamTest, OutputStreamResetReinitializesWhenWindowBitsChanges) {
  std::string payload = MakePayload(/*seed=*/31, /*size=*/20 * 1024);
  GzipOutputStream::Options win15_opts =
      MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                  /*mem_level=*/1, /*buffer_size=*/4096);
  win15_opts.window_bits = 15;
  GzipOutputStream::Options win14_opts = win15_opts;
  win14_opts.window_bits = 14;

  std::string expected_win15 = CompressWithNewStream(payload, win15_opts);
  std::string expected_win14 = CompressWithNewStream(payload, win14_opts);
  ASSERT_THAT(static_cast<uint8_t>(expected_win15[0]), Eq(0x78));
  ASSERT_THAT(static_cast<uint8_t>(expected_win14[0]), Eq(0x68));

  std::string actual1;
  google::protobuf::io::StringOutputStream stream1(&actual1);
  GzipOutputStream reused(&stream1, win15_opts);
  ASSERT_THAT(WriteAll(&reused, payload), IsTrue());
  ASSERT_THAT(reused.Finish(), IsTrue());
  EXPECT_THAT(actual1, Eq(expected_win15));

  std::string actual2;
  google::protobuf::io::StringOutputStream stream2(&actual2);
  ASSERT_THAT(reused.Reset(&stream2, win14_opts), IsTrue());
  ASSERT_THAT(WriteAll(&reused, payload), IsTrue());
  ASSERT_THAT(reused.Finish(), IsTrue());
  EXPECT_THAT(actual2, Eq(expected_win14));

  std::string actual3;
  google::protobuf::io::StringOutputStream stream3(&actual3);
  ASSERT_THAT(reused.Reset(&stream3, win15_opts), IsTrue());
  ASSERT_THAT(WriteAll(&reused, payload), IsTrue());
  ASSERT_THAT(reused.Close(), IsTrue());
  EXPECT_THAT(actual3, Eq(expected_win15));
}

TEST(GzipStreamTest, OutputStreamRejectsInvalidWindowBitsSafely) {
  for (int bad_window_bits : {8, 16, -1}) {
    GzipOutputStream::Options bad_opts =
        MakeOptions(GzipOutputStream::ZLIB, /*compression_level=*/3,
                    /*mem_level=*/1, /*buffer_size=*/1024);
    bad_opts.window_bits = bad_window_bits;

    std::string out;
    google::protobuf::io::StringOutputStream string_stream(&out);
    GzipOutputStream gzip_stream(&string_stream, bad_opts);
    EXPECT_THAT(gzip_stream.ZlibErrorCode(), Eq(Z_STREAM_ERROR));

    void* data;
    int size;
    EXPECT_THAT(gzip_stream.Next(&data, &size), IsFalse());
    EXPECT_THAT(gzip_stream.Flush(), IsFalse());
    EXPECT_THAT(gzip_stream.Close(), IsFalse());

    // Resetting with valid options after a failed Init/Close recovers cleanly.
    GzipOutputStream::Options good_opts = bad_opts;
    good_opts.window_bits = 14;
    std::string recovered;
    google::protobuf::io::StringOutputStream recovered_stream(&recovered);
    std::string payload = MakePayload(/*seed=*/32, /*size=*/4096);
    ASSERT_THAT(gzip_stream.Reset(&recovered_stream, good_opts), IsTrue());
    ASSERT_THAT(WriteAll(&gzip_stream, payload), IsTrue());
    ASSERT_THAT(gzip_stream.Close(), IsTrue());
    EXPECT_THAT(Decompress(recovered), Eq(payload));
  }
}

}  // namespace
}  // namespace protobuf_ports
}  // namespace lib
}  // namespace icing
