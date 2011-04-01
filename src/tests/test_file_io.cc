// Copyright 2011 Emir Habul, see file COPYING

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "tests/mock_file_io.h"

using ::testing::_;
using ::testing::Gt;
using ::testing::Truly;
using ::testing::Return;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::InSequence;

namespace wikigraph {

/* BufferedWriter */

TEST(BufferedWriter, empty) {
  MockSystemFile s;
  // EXPECT_CALL(s, close()).Times(AtLeast(1));
  BufferedWriter<MockSystemFile> b(&s);
}

TEST(BufferedWriter, uint) {
  InSequence seq;
  MockSystemFile s;
  EXPECT_CALL(s, write(_, 4, 1)).Times(1).WillOnce(Return(1));
  // EXPECT_CALL(s, close()).Times(AtLeast(1));
  BufferedWriter<MockSystemFile> b(&s);
  b.write_uint(45);
}

TEST(BufferedWriter, uint_buf2) {
  InSequence seq;
  MockSystemFile s;
  EXPECT_CALL(s, write(_, 4, kBufferSize)).Times(2)
    .WillRepeatedly(Return(kBufferSize));
  // EXPECT_CALL(s, close()).Times(AtLeast(1));
  BufferedWriter<MockSystemFile> b(&s);
  for (uint32_t i = 0; i < 2*kBufferSize; i++)
    b.write_uint(i*1234);
}

TEST(BufferedWriter, write_bit) {
  InSequence seq;
  MockSystemFile s;
  EXPECT_CALL(s, write(_, 4, 1)).Times(1).WillOnce(Return(1));
  BufferedWriter<MockSystemFile> b(&s);
  b.write_bit(true);
  b.write_bit(false);
}

/* BufferedReader */

TEST(TestBufferedReader, empty) {
  MockSystemFile s;
  // EXPECT_CALL(s, close()).Times(AtLeast(1));
  BufferedReader<MockSystemFile, uint32_t> b(&s);
}

size_t ReadAction1(void *buff, size_t sz, size_t nmemb) {
  uint32_t *arg1 = reinterpret_cast<uint32_t*>(buff);
  arg1[0] = 23;
  arg1[1] = 24;
  arg1[2] = 25;
  return 3;
}

TEST(TestBufferedReader, read1) {
  InSequence seq;
  MockSystemFile s;
  EXPECT_CALL(s, read(_, 4, Gt(3u))).Times(1).WillOnce(Invoke(ReadAction1));
  // EXPECT_CALL(s, close()).Times(AtLeast(1));
  BufferedReader<MockSystemFile, uint32_t> b(&s);
  ASSERT_EQ(23u, b.peek_unit());
  ASSERT_EQ(23u, b.read_unit());
  ASSERT_EQ(24u, b.read_unit());
  ASSERT_EQ(25u, b.peek_unit());
  ASSERT_EQ(25u, b.read_unit());
}

size_t ReadAction2(void *buff, size_t size, size_t nmemb) {
  uint32_t *arg1 = reinterpret_cast<uint32_t*>(buff);
  for (size_t i = 0; i < nmemb; i++) {
    arg1[i] = i;
  }
  return nmemb;
}

TEST(TestBufferedReader, read2) {
  InSequence seq;
  MockSystemFile s;
  EXPECT_CALL(s, read(_, 4, kBufferSize))
    .Times(2).WillRepeatedly(Invoke(ReadAction2));
  // EXPECT_CALL(s, close()).Times(AtLeast(1));

  BufferedReader<MockSystemFile, uint32_t> b(&s);
  for (size_t i = 0; i < 2*kBufferSize; i++) {
    ASSERT_EQ(i % kBufferSize, b.peek_unit() );
    ASSERT_EQ(i % kBufferSize, b.read_unit() );
  }
}

TEST(TestBufferedReader, using_stub) {
  uint32_t data[5] = {1, 2, 3, 4, 5};
  StubFileSystem fs(data, sizeof(data));
  BufferedReader<StubFileSystem, uint32_t> b(&fs);
  ASSERT_EQ(1u, b.peek_unit());
  uint32_t tmp[2];
  b.read_from_back(tmp, 2);
  ASSERT_EQ(tmp[0], 4u);
  ASSERT_EQ(tmp[1], 5u);
  ASSERT_EQ(1u, b.read_unit());
  ASSERT_EQ(2u, b.read_unit());
  b.read_from_back(tmp, 1);
  ASSERT_EQ(tmp[0], 5u);
  ASSERT_EQ(3u, b.read_unit());
}

}  // namespace wikigraph
