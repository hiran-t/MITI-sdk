// Little-endian CDR as ROS 2 writes it, enough for the messages transforms read and write.
// Alignment counts from the end of the 4-byte encapsulation header.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "miti/server.hpp"

namespace miti::cdr
{

constexpr size_t HEADER = 4;

class Reader
{
public:
  Reader(const uint8_t * data, size_t size)
  : data_(data), size_(size)
  {
    if (size < HEADER) throw std::runtime_error("CDR message is shorter than its header");
    if (data[1] != 0x01) throw std::runtime_error("Only little-endian CDR is supported");
  }

  uint8_t u8() {return *take(1);}
  bool boolean() {return u8() != 0;}
  uint32_t u32() {return little<uint32_t>(4);}
  int32_t i32() {return static_cast<int32_t>(u32());}

  std::string string()
  {
    const uint32_t length = u32();
    const uint8_t * text = take(length);
    return std::string(reinterpret_cast<const char *>(text), length ? length - 1 : 0);
  }

  /// A uint8[]: where its bytes start, and how many.
  std::pair<const uint8_t *, size_t> bytes()
  {
    const uint32_t length = u32();
    return {take(length), length};
  }

private:
  template<typename T>
  T little(size_t size)
  {
    offset_ += (size - ((offset_ - HEADER) % size)) % size;
    const uint8_t * in = take(size);
    T value = 0;
    for (size_t i = 0; i < size; ++i) value |= static_cast<T>(in[i]) << (8 * i);
    return value;
  }

  const uint8_t * take(size_t count)
  {
    if (offset_ + count > size_) throw std::runtime_error("CDR message ends early");
    const uint8_t * at = data_ + offset_;
    offset_ += count;
    return at;
  }

  const uint8_t * data_;
  size_t size_;
  size_t offset_ = HEADER;
};

class Writer
{
public:
  Writer & u8(uint8_t value) {bytes_.push_back(value); return *this;}
  Writer & boolean(bool value) {return u8(value ? 1 : 0);}
  Writer & i32(int32_t value) {return u32(static_cast<uint32_t>(value));}
  Writer & u32(uint32_t value)
  {
    while ((bytes_.size() - HEADER) % 4) bytes_.push_back(0);
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>(value >> (8 * i)));
    return *this;
  }
  Writer & string(const std::string & text)
  {
    u32(static_cast<uint32_t>(text.size() + 1));
    bytes_.insert(bytes_.end(), text.begin(), text.end());
    return u8(0);
  }
  Writer & bytes(const uint8_t * data, size_t size)
  {
    u32(static_cast<uint32_t>(size));
    bytes_.insert(bytes_.end(), data, data + size);
    return *this;
  }
  Bytes take() {return std::move(bytes_);}

private:
  Bytes bytes_{0x00, 0x01, 0x00, 0x00};
};

struct Header
{
  int32_t sec = 0;
  uint32_t nanosec = 0;
  std::string frame_id;
};

inline Header read_header(Reader & reader)
{
  Header header;
  header.sec = reader.i32();
  header.nanosec = reader.u32();
  header.frame_id = reader.string();
  return header;
}

inline void write_header(Writer & writer, const Header & header)
{
  writer.i32(header.sec).u32(header.nanosec).string(header.frame_id);
}

}  // namespace miti::cdr
