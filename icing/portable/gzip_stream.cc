// Copyright (C) 2009 Google LLC
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

// This file contains the implementation of classes GzipInputStream and
// GzipOutputStream. It is forked from protobuf because these classes are only
// provided in libprotobuf-full but we would like to link libicing against the
// smaller libprotobuf-lite instead.

#include "icing/portable/gzip_stream.h"

#include <cstddef>
#include <cstdint>

#include "icing/portable/zlib.h"
#include "icing/util/logging.h"

namespace icing {
namespace lib {
namespace protobuf_ports {

GzipInputStream::GzipInputStream(ZeroCopyInputStream* sub_stream, Format format,
                                 void* output_buffer, size_t buffer_size)
    : format_(format),
      sub_stream_(sub_stream),
      zerror_(Z_OK),
      output_buffer_(output_buffer),
      output_buffer_length_(buffer_size),
      byte_count_(0) {
  zcontext_.state = Z_NULL;
  zcontext_.zalloc = Z_NULL;
  zcontext_.zfree = Z_NULL;
  zcontext_.opaque = Z_NULL;
  zcontext_.total_out = 0;
  zcontext_.next_in = NULL;
  zcontext_.avail_in = 0;
  zcontext_.total_in = 0;
  zcontext_.msg = NULL;
  zcontext_.next_out = static_cast<uint8_t*>(output_buffer_);
  zcontext_.avail_out = static_cast<uint32_t>(output_buffer_length_);
  output_position_ = output_buffer_;
}

GzipInputStream::~GzipInputStream() {
  zerror_ = inflateEnd(&zcontext_);
}

void GzipInputStream::Reset(ZeroCopyInputStream* sub_stream,
                            void* output_buffer, size_t buffer_size) {
  sub_stream_ = sub_stream;
  output_buffer_ = output_buffer;
  output_buffer_length_ = buffer_size;
  byte_count_ = 0;
  zcontext_.next_in = NULL;
  zcontext_.avail_in = 0;
  zcontext_.msg = NULL;
  zcontext_.next_out = static_cast<uint8_t*>(output_buffer_);
  zcontext_.avail_out = static_cast<uint32_t>(output_buffer_length_);
  output_position_ = output_buffer_;
  if (zcontext_.state != Z_NULL) {
    // Reuse the already allocated inflate state. This also resets total_in and
    // total_out.
    zerror_ = inflateReset(&zcontext_);
  } else {
    // Inflate() will initialize the zlib state lazily on first use.
    zcontext_.total_in = 0;
    zcontext_.total_out = 0;
    zerror_ = Z_OK;
  }
}

static inline int internalInflateInit2(z_stream* zcontext,
                                       GzipInputStream::Format format) {
  int windowBitsFormat = 0;
  switch (format) {
    case GzipInputStream::GZIP:
      windowBitsFormat = 16;
      break;
    case GzipInputStream::AUTO:
      windowBitsFormat = 32;
      break;
    case GzipInputStream::ZLIB:
      windowBitsFormat = 0;
      break;
  }
  return inflateInit2(zcontext, /* windowBits */ 15 | windowBitsFormat);
}

int GzipInputStream::Inflate(int flush) {
  if ((zerror_ == Z_OK) && (zcontext_.avail_out == 0)) {
    // previous inflate filled output buffer. don't change input params yet.
  } else if (zcontext_.avail_in == 0) {
    const void* in;
    int in_size;
    bool first = zcontext_.state == Z_NULL;
    bool ok = sub_stream_->Next(&in, &in_size);
    if (!ok) {
      zcontext_.next_out = NULL;
      zcontext_.avail_out = 0;
      return Z_STREAM_END;
    }
    zcontext_.next_in = static_cast<uint8_t*>(const_cast<void*>(in));
    zcontext_.avail_in = static_cast<uint32_t>(in_size);
    if (first) {
      int error = internalInflateInit2(&zcontext_, format_);
      if (error != Z_OK) {
        return error;
      }
    }
  }
  zcontext_.next_out = static_cast<uint8_t*>(output_buffer_);
  zcontext_.avail_out = static_cast<uint32_t>(output_buffer_length_);
  output_position_ = output_buffer_;
  int error = inflate(&zcontext_, flush);
  return error;
}

void GzipInputStream::DoNextOutput(const void** data, int* size) {
  *data = output_position_;
  *size = static_cast<int>(reinterpret_cast<uintptr_t>(zcontext_.next_out) -
                           reinterpret_cast<uintptr_t>(output_position_));
  output_position_ = zcontext_.next_out;
}

// implements ZeroCopyInputStream ----------------------------------
bool GzipInputStream::Next(const void** data, int* size) {
  bool ok = (zerror_ == Z_OK) || (zerror_ == Z_STREAM_END) ||
            (zerror_ == Z_BUF_ERROR);
  if ((!ok) || (zcontext_.next_out == NULL)) {
    return false;
  }
  if (zcontext_.next_out != output_position_) {
    DoNextOutput(data, size);
    return true;
  }
  if (zerror_ == Z_STREAM_END) {
    // sub_stream_ may have concatenated streams to follow. Only reset the
    // inflate state if there is more input; otherwise this is the end of the
    // stream and there is no need to reallocate or reset the inflate state.
    while (zcontext_.avail_in == 0) {
      const void* in;
      int in_size;
      if (!sub_stream_->Next(&in, &in_size)) {
        zcontext_.next_out = NULL;
        zcontext_.avail_out = 0;
        return false;
      }
      zcontext_.next_in = static_cast<uint8_t*>(const_cast<void*>(in));
      zcontext_.avail_in = static_cast<uint32_t>(in_size);
    }
    const int64_t prev_total_out = static_cast<int64_t>(zcontext_.total_out);
    // inflateReset also resets total_out.
    zerror_ = inflateReset(&zcontext_);
    if (zerror_ != Z_OK) {
      return false;
    }
    byte_count_ += prev_total_out;
  }
  zerror_ = Inflate(Z_NO_FLUSH);
  if ((zerror_ == Z_STREAM_END) && (zcontext_.next_out == NULL)) {
    // The underlying stream's Next returned false inside Inflate.
    return false;
  }
  ok = (zerror_ == Z_OK) || (zerror_ == Z_STREAM_END) ||
       (zerror_ == Z_BUF_ERROR);
  if (!ok) {
    return false;
  }
  DoNextOutput(data, size);
  return true;
}
void GzipInputStream::BackUp(int count) {
  output_position_ = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(output_position_) - count);
}
bool GzipInputStream::Skip(int count) {
  const void* data;
  int size = 0;
  bool ok = Next(&data, &size);
  while (ok && (size < count)) {
    count -= size;
    ok = Next(&data, &size);
  }
  if (size > count) {
    BackUp(size - count);
  }
  return ok;
}
int64_t GzipInputStream::ByteCount() const {
  int64_t ret = byte_count_ + static_cast<int64_t>(zcontext_.total_out);
  if (zcontext_.next_out != NULL && output_position_ != NULL) {
    ret +=
        static_cast<int64_t>(reinterpret_cast<uintptr_t>(zcontext_.next_out) -
                             reinterpret_cast<uintptr_t>(output_position_));
  }
  return ret;
}

// =========================================================================

GzipOutputStream::Options::Options()
    : format(GZIP),
      buffer_size(kDefaultBufferSize),
      compression_level(Z_DEFAULT_COMPRESSION),
      compression_strategy(Z_DEFAULT_STRATEGY),
      mem_level(kDefaultMemLevel),
      window_bits(kDefaultWindowBits) {}

GzipOutputStream::GzipOutputStream(ZeroCopyOutputStream* sub_stream) {
  Init(sub_stream, Options());
}

GzipOutputStream::GzipOutputStream(ZeroCopyOutputStream* sub_stream,
                                   const Options& options) {
  Init(sub_stream, options);
}

void GzipOutputStream::Init(ZeroCopyOutputStream* sub_stream,
                            const Options& options) {
  input_buffer_length_ =
      options.buffer_size > 0 ? static_cast<size_t>(options.buffer_size) : 0;
  input_buffer_capacity_ = input_buffer_length_;
  input_buffer_ = operator new(input_buffer_capacity_);

  zcontext_.state = Z_NULL;
  zcontext_.zalloc = Z_NULL;
  zcontext_.zfree = Z_NULL;
  zcontext_.opaque = Z_NULL;
  zcontext_.total_out = 0;
  zcontext_.total_in = 0;
  ResetStreamState(sub_stream);
  zerror_ = DeflateInit(options);
}

void GzipOutputStream::ResetStreamState(ZeroCopyOutputStream* sub_stream) {
  sub_stream_ = sub_stream;
  sub_data_ = NULL;
  sub_data_size_ = 0;

  zcontext_.next_out = NULL;
  zcontext_.avail_out = 0;
  zcontext_.next_in = NULL;
  zcontext_.avail_in = 0;
  zcontext_.msg = NULL;
}

int GzipOutputStream::DeflateInit(const Options& options) {
  format_ = options.format;
  mem_level_ = options.mem_level;
  window_bits_ = options.window_bits;
  if (options.window_bits < 9 || options.window_bits > 15) {
    return Z_STREAM_ERROR;
  }
  // default to GZIP format
  int windowBitsFormat = 16;
  if (options.format == ZLIB) {
    windowBitsFormat = 0;
  }
  return deflateInit2(&zcontext_, options.compression_level, Z_DEFLATED,
                      /* windowBits */ options.window_bits | windowBitsFormat,
                      /* memLevel */ options.mem_level,
                      options.compression_strategy);
}

bool GzipOutputStream::Reset(ZeroCopyOutputStream* sub_stream,
                             const Options& options) {
  const size_t new_length =
      options.buffer_size > 0 ? static_cast<size_t>(options.buffer_size) : 0;
  if (new_length > input_buffer_capacity_) {
    operator delete(input_buffer_);
    input_buffer_capacity_ = new_length;
    input_buffer_ = operator new(input_buffer_capacity_);
  }
  // Keep the backing allocation while honoring the requested buffer_size so
  // stored-block chunking at compression_level == 0 stays byte-identical to a
  // newly constructed GzipOutputStream.
  input_buffer_length_ = new_length;

  ResetStreamState(sub_stream);
  if (zcontext_.state != Z_NULL && format_ == options.format &&
      mem_level_ == options.mem_level && window_bits_ == options.window_bits) {
    // deflateReset also resets total_in and total_out.
    zerror_ = deflateReset(&zcontext_);
    if (zerror_ == Z_OK) {
      // Nothing has been compressed since deflateReset, so deflateParams does
      // not emit any data here.
      zerror_ = deflateParams(&zcontext_, options.compression_level,
                              options.compression_strategy);
    }
  } else {
    if (zcontext_.state != Z_NULL) {
      deflateEnd(&zcontext_);
    }
    zcontext_.total_out = 0;
    zcontext_.total_in = 0;
    zerror_ = DeflateInit(options);
  }
  return zerror_ == Z_OK;
}

GzipOutputStream::~GzipOutputStream() {
  Close();
  operator delete(input_buffer_);
}

// private
int GzipOutputStream::Deflate(int flush) {
  int error = Z_OK;
  do {
    if ((sub_data_ == NULL) || (zcontext_.avail_out == 0)) {
      bool ok = sub_stream_->Next(&sub_data_, &sub_data_size_);
      if (!ok) {
        sub_data_ = NULL;
        sub_data_size_ = 0;
        return Z_BUF_ERROR;
      }
      if (sub_data_size_ <= 0) {
        ICING_LOG(FATAL) << "Failed to advance underlying stream";
      }
      zcontext_.next_out = static_cast<uint8_t*>(sub_data_);
      zcontext_.avail_out = static_cast<uint32_t>(sub_data_size_);
    }
    error = deflate(&zcontext_, flush);
  } while (error == Z_OK && zcontext_.avail_out == 0);
  if ((flush == Z_FULL_FLUSH) || (flush == Z_FINISH)) {
    // Notify lower layer of data.
    sub_stream_->BackUp(static_cast<int>(zcontext_.avail_out));
    // We don't own the buffer anymore.
    sub_data_ = NULL;
    sub_data_size_ = 0;
  }
  return error;
}

// implements ZeroCopyOutputStream ---------------------------------
bool GzipOutputStream::Next(void** data, int* size) {
  if (sub_stream_ == NULL || ((zerror_ != Z_OK) && (zerror_ != Z_BUF_ERROR))) {
    return false;
  }
  if (zcontext_.avail_in != 0) {
    zerror_ = Deflate(Z_NO_FLUSH);
    if (zerror_ != Z_OK) {
      return false;
    }
  }
  if (zcontext_.avail_in == 0) {
    // all input was consumed. reset the buffer.
    zcontext_.next_in = static_cast<uint8_t*>(input_buffer_);
    zcontext_.avail_in = static_cast<uint32_t>(input_buffer_length_);
    *data = input_buffer_;
    *size = static_cast<int>(input_buffer_length_);
  } else {
    // The loop in Deflate should consume all avail_in
    ICING_LOG(ERROR) << "Deflate left bytes unconsumed";
  }
  return true;
}
void GzipOutputStream::BackUp(int count) {
  if (zcontext_.avail_in < static_cast<uint32_t>(count)) {
    ICING_LOG(FATAL) << "Not enough data to back up " << count << " bytes";
  }
  zcontext_.avail_in -= static_cast<uint32_t>(count);
}
int64_t GzipOutputStream::ByteCount() const {
  return static_cast<int64_t>(zcontext_.total_in + zcontext_.avail_in);
}

bool GzipOutputStream::Flush() {
  if (sub_stream_ == NULL || ((zerror_ != Z_OK) && (zerror_ != Z_BUF_ERROR))) {
    return false;
  }
  zerror_ = Deflate(Z_FULL_FLUSH);
  // Return true if the flush succeeded or if it was a no-op.
  return (zerror_ == Z_OK) ||
         (zerror_ == Z_BUF_ERROR && zcontext_.avail_in == 0 &&
          zcontext_.avail_out != 0);
}

bool GzipOutputStream::Close() {
  if (sub_stream_ == NULL || ((zerror_ != Z_OK) && (zerror_ != Z_BUF_ERROR))) {
    // Already closed or finished, or in an error state. Release the zlib state
    // if it is still allocated (e.g. after Finish()).
    if (zcontext_.state != Z_NULL) {
      deflateEnd(&zcontext_);
    }
    sub_stream_ = NULL;
    return false;
  }
  do {
    zerror_ = Deflate(Z_FINISH);
  } while (zerror_ == Z_OK);
  zerror_ = deflateEnd(&zcontext_);
  bool ok = zerror_ == Z_OK;
  zerror_ = Z_STREAM_END;
  sub_stream_ = NULL;
  return ok;
}

bool GzipOutputStream::Finish() {
  if (sub_stream_ == NULL || ((zerror_ != Z_OK) && (zerror_ != Z_BUF_ERROR))) {
    return false;
  }
  do {
    zerror_ = Deflate(Z_FINISH);
  } while (zerror_ == Z_OK);
  bool ok = zerror_ == Z_STREAM_END;
  // Keep the zlib state allocated for Reset(). It is released by Close() or
  // the destructor. Detach sub_stream_ so no dangling pointer remains once the
  // caller destroys its output stream, while preserving zerror_ on failure.
  sub_stream_ = NULL;
  return ok;
}

}  // namespace protobuf_ports
}  // namespace lib
}  // namespace icing
