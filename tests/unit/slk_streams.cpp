#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "slk/streams.hpp"

class BinaryData {
 public:
  BinaryData(const uint8_t *data, size_t size) : data_(new uint8_t[size]), size_(size) {
    memcpy(data_.get(), data, size);
  }

  BinaryData(std::unique_ptr<uint8_t[]> data, size_t size) : data_(std::move(data)), size_(size) {}

  const uint8_t *data() const { return data_.get(); }
  size_t size() const { return size_; }

  bool operator==(const BinaryData &other) const {
    if (size_ != other.size_) return false;
    for (size_t i = 0; i < size_; ++i) {
      if (data_[i] != other.data_[i]) return false;
    }
    return true;
  }

 private:
  std::unique_ptr<uint8_t[]> data_;
  size_t size_;
};

BinaryData operator+(const BinaryData &a, const BinaryData &b) {
  std::unique_ptr<uint8_t[]> data(new uint8_t[a.size() + b.size()]);
  memcpy(data.get(), a.data(), a.size());
  memcpy(data.get() + a.size(), b.data(), b.size());
  return BinaryData(std::move(data), a.size() + b.size());
}

BinaryData GetRandomData(size_t size) {
  std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<uint8_t> dis(0, 255);
  std::unique_ptr<uint8_t[]> ret(new uint8_t[size]);
  auto data = ret.get();
  for (size_t i = 0; i < size; ++i) {
    data[i] = dis(gen);
  }
  return BinaryData(std::move(ret), size);
}

std::vector<BinaryData> BufferToBinaryData(const uint8_t *data, size_t size, std::vector<size_t> sizes) {
  std::vector<BinaryData> ret;
  ret.reserve(sizes.size());
  size_t pos = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    EXPECT_GE(size, pos + sizes[i]);
    ret.push_back({data + pos, sizes[i]});
    pos += sizes[i];
  }
  return ret;
}

BinaryData SizeToBinaryData(slk::SegmentSize size) {
  return BinaryData(reinterpret_cast<const uint8_t *>(&size), sizeof(slk::SegmentSize));
}

TEST(Builder, SingleSegment) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(5);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  ASSERT_EQ(buffer.size(), input.size() + 2 * sizeof(slk::SegmentSize));

  auto splits = BufferToBinaryData(buffer.data(), buffer.size(),
                                   {sizeof(slk::SegmentSize), input.size(), sizeof(slk::SegmentSize)});

  auto header_expected = SizeToBinaryData(input.size());
  ASSERT_EQ(splits[0], header_expected);

  ASSERT_EQ(splits[1], input);

  auto footer_expected = SizeToBinaryData(0);
  ASSERT_EQ(splits[2], footer_expected);
}

TEST(Builder, MultipleSegments) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(slk::kSegmentMaxDataSize + 100);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  ASSERT_EQ(buffer.size(), input.size() + 3 * sizeof(slk::SegmentSize));

  auto splits = BufferToBinaryData(buffer.data(), buffer.size(),
                                   {sizeof(slk::SegmentSize), slk::kSegmentMaxDataSize, sizeof(slk::SegmentSize),
                                    input.size() - slk::kSegmentMaxDataSize, sizeof(slk::SegmentSize)});

  auto datas = BufferToBinaryData(input.data(), input.size(),
                                  {slk::kSegmentMaxDataSize, input.size() - slk::kSegmentMaxDataSize});

  auto header1_expected = SizeToBinaryData(slk::kSegmentMaxDataSize);
  ASSERT_EQ(splits[0], header1_expected);

  ASSERT_EQ(splits[1], datas[0]);

  auto header2_expected = SizeToBinaryData(input.size() - slk::kSegmentMaxDataSize);
  ASSERT_EQ(splits[2], header2_expected);

  ASSERT_EQ(splits[3], datas[1]);

  auto footer_expected = SizeToBinaryData(0);
  ASSERT_EQ(splits[4], footer_expected);
}

TEST(Reader, SingleSegment) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(5);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  // test with missing data
  for (size_t i = 0; i < buffer.size(); ++i) {
    slk::Reader reader(buffer.data(), i);
    uint8_t block[slk::kSegmentMaxDataSize];
    ASSERT_THROW(
        {
          reader.Load(block, input.size());
          reader.Finalize();
        },
        slk::SlkReaderException);
  }

  // test with complete data
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    reader.Load(block, input.size());
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // test with leftover data
  {
    auto extended_buffer = BinaryData(buffer.data(), buffer.size()) + GetRandomData(5);
    slk::Reader reader(extended_buffer.data(), extended_buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    reader.Load(block, input.size());
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // read more data than there is in the stream
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    ASSERT_THROW(reader.Load(block, slk::kSegmentMaxDataSize), slk::SlkReaderException);
  }

  // don't consume all data from the stream
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    reader.Load(block, input.size() / 2);
    ASSERT_THROW(reader.Finalize(), slk::SlkReaderException);
  }

  // read data with several loads
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    for (size_t i = 0; i < input.size(); ++i) {
      reader.Load(block + i, 1);
    }
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // modify the end mark
  buffer[buffer.size() - 1] = 1;
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize];
    reader.Load(block, input.size());
    ASSERT_THROW(reader.Finalize(), slk::SlkReaderException);
  }
}

TEST(Reader, MultipleSegments) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(slk::kSegmentMaxDataSize + 100);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  // test with missing data
  for (size_t i = 0; i < buffer.size(); ++i) {
    slk::Reader reader(buffer.data(), i);
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    ASSERT_THROW(
        {
          reader.Load(block, input.size());
          reader.Finalize();
        },
        slk::SlkReaderException);
  }

  // test with complete data
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    reader.Load(block, input.size());
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // test with leftover data
  {
    auto extended_buffer = BinaryData(buffer.data(), buffer.size()) + GetRandomData(5);
    slk::Reader reader(extended_buffer.data(), extended_buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    reader.Load(block, input.size());
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // read more data than there is in the stream
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    ASSERT_THROW(reader.Load(block, slk::kSegmentMaxDataSize * 2), slk::SlkReaderException);
  }

  // don't consume all data from the stream
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    reader.Load(block, input.size() / 2);
    ASSERT_THROW(reader.Finalize(), slk::SlkReaderException);
  }

  // read data with several loads
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    for (size_t i = 0; i < input.size(); ++i) {
      reader.Load(block + i, 1);
    }
    reader.Finalize();
    auto output = BinaryData(block, input.size());
    ASSERT_EQ(output, input);
  }

  // modify the end mark
  buffer[buffer.size() - 1] = 1;
  {
    slk::Reader reader(buffer.data(), buffer.size());
    uint8_t block[slk::kSegmentMaxDataSize * 2];
    reader.Load(block, input.size());
    ASSERT_THROW(reader.Finalize(), slk::SlkReaderException);
  }
}

TEST(CheckStreamComplete, SingleSegment) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(5);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  // test with missing data
  for (size_t i = 0; i < sizeof(slk::SegmentSize); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize);
    ASSERT_EQ(data_size, 0);
  }
  for (size_t i = sizeof(slk::SegmentSize); i < sizeof(slk::SegmentSize) + input.size(); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize + sizeof(slk::SegmentSize));
    ASSERT_EQ(data_size, 0);
  }
  for (size_t i = sizeof(slk::SegmentSize) + input.size(); i < buffer.size(); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize + sizeof(slk::SegmentSize) + input.size());
    ASSERT_EQ(data_size, input.size());
  }

  // test with complete data
  {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), buffer.size());
    ASSERT_EQ(status, slk::StreamStatus::COMPLETE);
    ASSERT_EQ(stream_size, buffer.size());
    ASSERT_EQ(data_size, input.size());
  }

  // test with leftover data
  {
    auto extended_buffer = BinaryData(buffer.data(), buffer.size()) + GetRandomData(5);
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(extended_buffer.data(), extended_buffer.size());
    ASSERT_EQ(status, slk::StreamStatus::COMPLETE);
    ASSERT_EQ(stream_size, buffer.size());
    ASSERT_EQ(data_size, input.size());
  }
}

TEST(CheckStreamComplete, MultipleSegments) {
  std::vector<uint8_t> buffer;
  slk::Builder builder([&buffer](const uint8_t *data, size_t size, bool have_more) {
    for (size_t i = 0; i < size; ++i) buffer.push_back(data[i]);
  });

  auto input = GetRandomData(slk::kSegmentMaxDataSize + 100);
  builder.Save(input.data(), input.size());
  builder.Finalize();

  // test with missing data
  for (size_t i = 0; i < sizeof(slk::SegmentSize); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize);
    ASSERT_EQ(data_size, 0);
  }
  for (size_t i = sizeof(slk::SegmentSize); i < sizeof(slk::SegmentSize) + slk::kSegmentMaxDataSize; ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize + sizeof(slk::SegmentSize));
    ASSERT_EQ(data_size, 0);
  }
  for (size_t i = sizeof(slk::SegmentSize) + slk::kSegmentMaxDataSize;
       i < sizeof(slk::SegmentSize) * 2 + slk::kSegmentMaxDataSize; ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, sizeof(slk::SegmentSize) + slk::kSegmentMaxDataSize + slk::kSegmentMaxTotalSize);
    ASSERT_EQ(data_size, slk::kSegmentMaxDataSize);
  }
  for (size_t i = sizeof(slk::SegmentSize) * 2 + slk::kSegmentMaxDataSize;
       i < sizeof(slk::SegmentSize) * 2 + input.size(); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, sizeof(slk::SegmentSize) * 2 + slk::kSegmentMaxDataSize + slk::kSegmentMaxTotalSize);
    ASSERT_EQ(data_size, slk::kSegmentMaxDataSize);
  }
  for (size_t i = sizeof(slk::SegmentSize) * 2 + input.size(); i < buffer.size(); ++i) {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), i);
    ASSERT_EQ(status, slk::StreamStatus::PARTIAL);
    ASSERT_EQ(stream_size, slk::kSegmentMaxTotalSize + sizeof(slk::SegmentSize) * 2 + input.size());
    ASSERT_EQ(data_size, input.size());
  }

  // test with complete data
  {
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(buffer.data(), buffer.size());
    ASSERT_EQ(status, slk::StreamStatus::COMPLETE);
    ASSERT_EQ(stream_size, buffer.size());
    ASSERT_EQ(data_size, input.size());
  }

  // test with leftover data
  {
    auto extended_buffer = BinaryData(buffer.data(), buffer.size()) + GetRandomData(5);
    auto [status, stream_size, data_size] = slk::CheckStreamComplete(extended_buffer.data(), extended_buffer.size());
    ASSERT_EQ(status, slk::StreamStatus::COMPLETE);
    ASSERT_EQ(stream_size, buffer.size());
    ASSERT_EQ(data_size, input.size());
  }
}

TEST(CheckStreamComplete, InvalidSegment) {
  auto input = SizeToBinaryData(0);
  auto [status, stream_size, data_size] = slk::CheckStreamComplete(input.data(), input.size());
  ASSERT_EQ(status, slk::StreamStatus::INVALID);
  ASSERT_EQ(stream_size, 0);
  ASSERT_EQ(data_size, 0);
}
