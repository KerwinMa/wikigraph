// Copyright 2011 Emir Habul, see file COPYING

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "graph_algo.h"
#include "tests/mock_file_io.h"

using ::testing::_;
using ::testing::Gt;
using ::testing::Truly;
using ::testing::Return;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::InSequence;

namespace wikigraph {

TEST(count_items, simple) {
  uint32_t data[6] = {1, 2, 3, 4, 1, 3};
  vector<uint32_t> vdata(data, data+6);
  vector<pii> res = util::count_items(vdata);
  ASSERT_EQ(4u, res.size());

  ASSERT_EQ(1u, res[0].first);
  ASSERT_EQ(2u, res[0].second);

  ASSERT_EQ(2u, res[1].first);
  ASSERT_EQ(1u, res[1].second);

  ASSERT_EQ(3u, res[2].first);
  ASSERT_EQ(2u, res[2].second);

  ASSERT_EQ(4u, res[3].first);
  ASSERT_EQ(1u, res[3].second);
}

TEST(to_json, VI) {
  uint32_t data[6] = {1, 2, 3, 4, 1, 3};
  vector<uint32_t> vdata(data, data+6);
  ASSERT_EQ("[1,2,3,4,1,3]", util::to_json(vdata));
}

TEST(to_json, VPII) {
  pii data[3] = {pii(1, 2), pii(3, 4), pii(1, 3)};
  vector<pii> vdata(data, data+3);
  ASSERT_EQ("[[1,2],[3,4],[1,3]]", util::to_json(vdata));
}

TEST(CompleteGraphAlgo, BFSsimple) {
  uint32_t data[10] = {
    1, 3,
    1,
    0,  // n0
    0,  // n1
    0,  // n2
    2,  // n3
    3,  // extra
    3, 3,
  };
  StubFile fs(data, sizeof(data));
  CompleteGraphAlgo algo(&fs);
  algo.init();
  vector<uint32_t> res;
  // node 1
  res = algo.GetDistances(1);
  ASSERT_EQ(1u, res.size());
  ASSERT_EQ(1u, res[0]);
  // node 2
  res = algo.GetDistances(2);
  ASSERT_EQ(2u, res.size());
  ASSERT_EQ(1u, res[0]);
  ASSERT_EQ(2u, res[1]);
  // node 3
  res = algo.GetDistances(3);
  ASSERT_EQ(2u, res.size());
  ASSERT_EQ(1u, res[0]);
  ASSERT_EQ(1u, res[1]);

  vector<pii> res2 = algo.Scc();
  ASSERT_EQ(1u, res2.size());
  ASSERT_EQ(1u, res2[0].first);
  ASSERT_EQ(3u, res2[0].second);
}

TEST(CompleteGraphAlgo, PageRankSimple) {
  uint32_t data[10] = {
    1, 3,
    1,
    0,  // n0
    0,  // n1
    0,  // n2
    2,  // n3
    3,  // extra
    3, 3,
  };
  StubFile fs(data, sizeof(data));
  CompleteGraphAlgo algo(&fs);
  algo.init();
  vector<pair<double, node_t> > res;

  res = algo.PageRank(3, 1);
  ASSERT_EQ(1u, res.size());
  ASSERT_EQ(1u, res[0].second);

  res = algo.PageRank(3, 2);
  ASSERT_EQ(2u, res.size());
  ASSERT_EQ(1u, res[0].second);
  ASSERT_EQ(3u, res[1].second);

  res = algo.PageRank(3, 3);
  ASSERT_EQ(3u, res.size());
  ASSERT_EQ(1u, res[0].second);
  ASSERT_EQ(3u, res[1].second);
  ASSERT_EQ(2u, res[2].second);
}

}  // namespace wikigraph

