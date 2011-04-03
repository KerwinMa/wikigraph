// Copyright 2011 Emir Habul, see file COPYING

#include <cstdio>
#include <cctype>
#include <cstdarg>

#include "config.h"
#include "sql_parser.h"
#include "file_io.h"
#include "hiredis/hiredis.h"
#include "hash.h"
#include "graph.h"

namespace wikigraph {

// MediaWiki namespaces for articles and categories
enum WikiNamespaces {
  NS_MAIN = 0,
  NS_CATEGORY = 14,
};

namespace {  // unnamed

/**************
 * I would normally use redis for this data, but I was primarily trying to
 * reduce memory usage of redis server by storing certain keys in an array
 * rather than the database.
 */

#pragma pack(push)
#pragma pack(1)     // set alignment to 1 byte boundary
struct WikiStatus {
  enum {
    UNKNOWN = 0,
    REGULAR = 1,
    REDIRECT = 2,
    RESOLVED = 3  // resolved redirect
  };
  unsigned int type:3;
  unsigned int is_category:1;  // 0 - article, 1 - category
};
#pragma pack(pop)

WikiStatus g_wikistatus[MAX_WIKI_PAGEID+1];

uint32_t g_wikigraphId[MAX_WIKI_PAGEID+1];

// Global struct for storing info about graph and conversion process
struct WikiGraphInfo {
  int graph_nodes_count, article_count, category_count;
  int art_redirect_count, cat_redirect_count;
  int article_links_count, pagelink_rows_count;
  int skipped_catlinks, skipped_fromcat_links;
  int category_links_count;
} g_info;

}  // namespace

// For some reason redisCommand does not return redisReply*
// Hiding this ugly syntax.
// TODO(user): should I make interface for redis?
redisReply* redisCmd(redisContext *c, const char *format, ...) {
  va_list argptr;
  va_start(argptr, format);
  redisReply* reply = reinterpret_cast<redisReply*>(
      redisvCommand(c, format, argptr));
  va_end(argptr);
  return reply;
}

/**************
 * STAGE 1
 * Create graph nodes and store relevant data about nodes
 */
namespace stage1 {

class PageHandler : public DataHandler {  // for Stage1
 private:
  // SQL schema
  enum Page {
    page_id = 0,  // int(8) unsigned NOT NULL AUTO_INCREMENT,
    page_namespace = 1,  // int(11) NOT NULL DEFAULT '0',
    page_title = 2,  // varbinary(255) NOT NULL DEFAULT '',
    page_restrictions = 3,  // tinyblob NOT NULL,
    page_counter = 4,  // bigint(20) unsigned NOT NULL DEFAULT '0',
    page_is_redirect = 5,  // tinyint(1) unsigned NOT NULL DEFAULT '0',
    page_is_new = 6,  // tinyint(1) unsigned NOT NULL DEFAULT '0',
    page_random = 7,  // double unsigned NOT NULL DEFAULT '0',
    page_touched = 8,  // varbinary(14) NOT NULL DEFAULT '',
    page_latest = 9,  // int(8) unsigned NOT NULL DEFAULT '0',
    page_len = 10  // int(8) unsigned NOT NULL DEFAULT '0',
  };
 public:
  explicit PageHandler(redisContext *redis)
  :redis_(redis), is_cat_(NULL) { }
  void init() {
    file_.open("graph_nodeiscat.bin", "wb");
    is_cat_ = new BufferedWriter(&file_);
    is_cat_->write_bit(0);  // graphId starts from 1
  }
  ~PageHandler() {
    delete is_cat_;
    file_.close();
  }
  void data(const vector<string> &data);
 private:
  redisContext *redis_;
  BufferedWriter *is_cat_;
  SystemFile file_;
  DISALLOW_COPY_AND_ASSIGN(PageHandler);
};

void main_stage1(redisContext *redis) {
  g_info.graph_nodes_count = g_info.article_count = g_info.category_count = 0;
  g_info.art_redirect_count = g_info.cat_redirect_count = 0;

  PageHandler data_handler(redis);
  data_handler.init();

  const char *fname = DUMPFILES"page.sql";
  const char *gzname = DUMPFILES"page.sql.gz";

  SystemFile file;
  if (file.open(fname, "rb")) {
    BufferedReader<char> reader(&file);
    reader.set_print_progress(true);
    SqlParser parser(&reader, &data_handler);
    parser.run();
    file.close();
  } else {
    GzipFile gzfile;
    if (gzfile.open(gzname, "rb")) {
      BufferedReader<char> reader(&gzfile);
      reader.set_print_progress(true);
      SqlParser parser(&reader, &data_handler);
      parser.run();
    } else {
      fprintf(stderr, "failed to open file '%s' and '%s'\n", fname, gzname);
    }
    gzfile.close();
  }
  redisReply *reply;
  reply = redisCmd(redis,
      "SET s:count:Articles %d", g_info.article_count);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Article_redirects %d", g_info.art_redirect_count);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Categories %d", g_info.category_count);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Category_redirects %d", g_info.cat_redirect_count);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Graph_nodes %d", g_info.graph_nodes_count);
  freeReplyObject(reply);
}

// mysql table 'page'
void PageHandler::data(const vector<string> &data) {
  bool is_redir = data[page_is_redirect][0] == '1';
  int namespc = atoi(data[page_namespace].c_str());

  int wikiId = atoi(data[page_id].c_str());
  assert(wikiId <= MAX_WIKI_PAGEID);

  const char *prefix;
  if (namespc == NS_MAIN) {  // Articles
    prefix = "a:";
    if (is_redir)
      g_info.art_redirect_count++;
    else
      g_info.article_count++;
  } else if (namespc == NS_CATEGORY) {  // Categories
    prefix = "c:";
    if (is_redir)
      g_info.cat_redirect_count++;
    else
      g_info.category_count++;
    g_wikistatus[wikiId].is_category = 1;
  } else {
    return;  // Other namespaces are not interesting
  }

  ComputeHash hash;
  const char *title = data[page_title].c_str();
  hash.ProcessString(title, data[page_title].size());

  redisReply *reply;

  int graphId = -1;
  if (is_redir) {
    reply = redisCmd(redis_,
        "SETNX %s%s w:%d", prefix, hash.get_hash(), wikiId);
#ifdef DEBUG
    printf("redirect %s%s  key=%s%s (wikiId=%d)\n",
        prefix, title, prefix, hash.get_hash(), wikiId);
#endif
  } else {
    graphId = ++g_info.graph_nodes_count;
    is_cat_->write_bit(namespc == NS_CATEGORY);

    reply = redisCmd(redis_,
        "SETNX %s%s %d", prefix, hash.get_hash(), graphId);
#ifdef DEBUG
    printf("graph[%d] = %s%s (wikiId=%d)\n", graphId, prefix, title, wikiId);
#endif
  }
  if (reply->integer != 1) {
    freeReplyObject(reply);
    printf("Collission found!\nHow to fix?\n 1.Empty the database,\n");
    printf("2. Try adding salt to md5, or increase keylen.\n");
    printf("Title:'%s' hash_key=%s\n", title, hash.get_hash());

    exit(1);
  }
  freeReplyObject(reply);

  if (is_redir) {
    g_wikistatus[wikiId].type = WikiStatus::REDIRECT;
    reply = redisCmd(redis_,
        "SET w:%d %s%s", wikiId, prefix, hash.get_hash());
    freeReplyObject(reply);
  } else {
    g_wikistatus[wikiId].type = WikiStatus::REGULAR;
    g_wikigraphId[wikiId] = graphId;

    if ((graphId % (1024*4)) == 0) {
      printf(" %d\n", graphId);
    }
  }
  return;
}  // DataHandler::data

}  // namespace stage1

/**************
 * STAGE 2
 * Resolve redirects
 */
namespace stage2 {

class RedirectHandler : public DataHandler {
 private:
  // SQL schema
  enum Redirect {
    rd_from = 0,  // int(8) unsigned NOT NULL DEFAULT '0',
    rd_namespace = 1,  // int(11) NOT NULL DEFAULT '0',
    rd_title = 2  // varbinary(255) NOT NULL DEFAULT '',
  };
  redisContext *redis_;
 public:
  int unresolved_redir_count;
  explicit RedirectHandler(redisContext *redis)
  :redis_(redis) {
    unresolved_redir_count = 0;
  }
  ~RedirectHandler() { }
  void init() { }
  void data(const vector<string> &data);
 private:
  DISALLOW_COPY_AND_ASSIGN(RedirectHandler);
};

void main_stage2(redisContext *redis) {
  // Can handle multiple redirects (up to 4 levels)
  const int REDIR_MAX = 4;
  RedirectHandler data_handler(redis);
  data_handler.init();

  const char *fname = DUMPFILES"redirect.sql";
  const char *gzname = DUMPFILES"redirect.sql.gz";

  int iter = 0;
  do {
    data_handler.unresolved_redir_count = 0;

    // Open redirect.sql
    SystemFile file;
    if (file.open(fname, "rb")) {
      BufferedReader<char> reader(&file);
      reader.set_print_progress(true);
      SqlParser parser(&reader, &data_handler);
      parser.run();
      file.close();
    } else {
      GzipFile gzfile;
      if (gzfile.open(gzname, "rb")) {
        BufferedReader<char> reader(&gzfile);
        reader.set_print_progress(true);
        SqlParser parser(&reader, &data_handler);
        parser.run();
      } else {
        fprintf(stderr, "failed to open file '%s' and '%s'\n", fname, gzname);
      }
      gzfile.close();
    }
    printf("Unresolved redirects: %d\n", data_handler.unresolved_redir_count);
    iter++;
  }
  while (data_handler.unresolved_redir_count && iter < REDIR_MAX);
}

// mysql table 'redirect'
void RedirectHandler::data(const vector<string> &data) {
  int wikiId = atoi(data[rd_from].c_str());
  // Check is redirect was resolved previously
  if (g_wikistatus[wikiId].type != 2)
    return;
  int namespc = atoi(data[rd_namespace].c_str());

  const char *prefix;
  if (namespc == NS_MAIN)  // Articles
    prefix = "a:";
  else if (namespc == NS_CATEGORY)  // Categories
    prefix = "c:";
  else
    return;  // Other namespaces are not interesting

  ComputeHash hash;
  const char *title = data[rd_title].c_str();
  hash.ProcessString(title, data[rd_title].size());

#ifdef DEBUG
  printf("(wikiId=%d) redirected to %s%s\n", wikiId, prefix, title);
#endif

  // Check target
  redisReply *reply;
  reply = redisCmd(redis_,
      "GET %s%s", prefix, hash.get_hash());

  if (reply->type == REDIS_REPLY_NIL || !isdigit(reply->str[0])) {
    freeReplyObject(reply);
    // Target is still a redirect
    unresolved_redir_count++;
  } else {
    // Target is valid page (or resolved redirect)
    int graphId = atoi(reply->str);
    freeReplyObject(reply);
    // Need to get prefix:hash_key of a page based on its wikiId
    reply = redisCmd(redis_, "GET w:%d", wikiId);
    assert(reply->type == REDIS_REPLY_STRING && reply->str[1] == ':');
    string prefix_hash(reply->str);
    freeReplyObject(reply);

    reply = redisCmd(redis_,
        "SET %s %d", prefix_hash.c_str(), graphId);
    freeReplyObject(reply);

    g_wikistatus[wikiId].type = WikiStatus::RESOLVED;  // Resolved redirect
    g_wikigraphId[wikiId] = graphId;

    // This key is no longer needed
    reply = redisCmd(redis_, "DEL w:%d", wikiId);
    freeReplyObject(reply);
  }
}  // DataHandler::data

}  // namespace stage2

/**************
 * STAGE 3
 * Handle pagelinks, generate directed graph between articles
 */
namespace stage3 {

class PageLinkHandler : public DataHandler {
 private:
  enum PageLinks {  // SQL schema
    pl_from = 0,  // int(8) unsigned NOT NULL DEFAULT '0',
    pl_namespace = 1,  // int(11) NOT NULL DEFAULT '0',
    pl_title = 2  // varbinary(255) NOT NULL DEFAULT '',
  };
  redisContext *redis_;
  GraphWriter *graph_;
  BufferedWriter *buff_writer_;
  SystemFile file_;
 public:
  explicit PageLinkHandler(redisContext *redis)
  :redis_(redis) { }
  void init() {
    file_.open("artlinks.graph", "wb");
    buff_writer_ = new BufferedWriter(&file_);
    graph_ = new GraphBuffWriter(buff_writer_, g_info.graph_nodes_count);
  }
  ~PageLinkHandler() {
    delete graph_;
    delete buff_writer_;
    file_.close();
  }
  void data(const vector<string> &data);
 private:
  DISALLOW_COPY_AND_ASSIGN(PageLinkHandler);
};

void main_stage3(redisContext *redis) {
  g_info.article_links_count = g_info.pagelink_rows_count = 0;
  g_info.skipped_catlinks = g_info.skipped_fromcat_links = 0;

  PageLinkHandler data_handler(redis);
  data_handler.init();

  const char *fname = DUMPFILES"pagelinks.sql";
  const char *gzname = DUMPFILES"pagelinks.sql.gz";

  // Open pagelinks.sql
  SystemFile file;
  if (file.open(fname, "rb")) {
    BufferedReader<char> reader(&file);
    reader.set_print_progress(true);
    SqlParser parser(&reader, &data_handler);
    parser.run();
    file.close();
  } else {
    GzipFile gzfile;
    if (gzfile.open(gzname, "rb")) {
      BufferedReader<char> reader(&gzfile);
      reader.set_print_progress(true);
      SqlParser parser(&reader, &data_handler);
      parser.run();
      gzfile.close();
    } else {
      fprintf(stderr, "failed to open file '%s' and '%s'\n", fname, gzname);
    }
  }

  redisReply *reply;
  reply = redisCmd(redis,
      "SET s:count:Article_links %d", g_info.article_links_count);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Ignored_links_from_category %d",
      g_info.skipped_fromcat_links);
  freeReplyObject(reply);
  reply = redisCmd(redis,
      "SET s:count:Ignored_links_to_category %d", g_info.skipped_catlinks);
  freeReplyObject(reply);
}

// mysql table 'pagelink'
void PageLinkHandler::data(const vector<string> &data) {
  g_info.pagelink_rows_count++;
  int wikiId = atoi(data[pl_from].c_str());

  // Check if this page exists and is regular (not redirect)
  if (g_wikistatus[wikiId].type != WikiStatus::REGULAR)
    return;  // If it is not regular page skip it
  if (g_wikistatus[wikiId].is_category) {
    g_info.skipped_fromcat_links++;
    return;  // Skip links from categories
  }
  int from_graphId = g_wikigraphId[wikiId];

  graph_->start_node(from_graphId);

  int namespc = atoi(data[pl_namespace].c_str());

  const char *prefix;
  if (namespc == NS_MAIN) {  // Articles
    prefix = "a:";
  } else if (namespc == NS_CATEGORY) {   // Categories
    // Links to categories are ignored
    // I only focus on inter-article links and category inclusion links
    g_info.skipped_catlinks++;
    return;
  } else {
    return;  // Other namespaces are not interesting
  }

  ComputeHash hash;
  const char *title = data[pl_title].c_str();
  hash.ProcessString(title, data[pl_title].size());

  redisReply *reply;
  reply = redisCmd(redis_,
      "GET %s%s", prefix, hash.get_hash());
  if (reply->type != REDIS_REPLY_NIL && isdigit(reply->str[0])) {
    int to_graphId = atoi(reply->str);

#ifdef DEBUG
  printf("link from graphId=%d (wikiId=%d)  to=%s%s graphId=%d  type=%d\n",
      from_graphId, wikiId, prefix, title, to_graphId,
      g_wikistatus[to_graphId].type);
#endif
    graph_->add_edge(to_graphId);
  }
  freeReplyObject(reply);
}  // DataHandler::data

}  // namespace stage3

/**************
 * STAGE 4
 * Genereate _forward_ edges in category inclusion links
 */
namespace stage4 {

class CategoryLinksHandler : public DataHandler {
  // SQL schema
  enum CategoryLinks {
    cl_from = 0,  // int(10) unsigned NOT NULL DEFAULT '0',
    cl_to = 1,  // varbinary(255) NOT NULL DEFAULT '',
    cl_sortkey = 2,  // varbinary(70) NOT NULL DEFAULT '',
    cl_timestamp = 3  // timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  };
  redisContext *redis_;
  GraphWriter *graph_;
  BufferedWriter *buff_writer_;
  SystemFile file_;
 public:
  explicit CategoryLinksHandler(redisContext *redis)
  :redis_(redis) { }
  void init() {
    file_.open("tmp_catlinks_fw.graph", "wb");
    buff_writer_ = new BufferedWriter(&file_);
    graph_ = new GraphBuffWriter(buff_writer_, g_info.graph_nodes_count);
  }
  ~CategoryLinksHandler() {
    delete graph_;
    delete buff_writer_;
    file_.close();
  }
  void data(const vector<string> &data);
 private:
  DISALLOW_COPY_AND_ASSIGN(CategoryLinksHandler);
};

void main_stage4(redisContext *redis) {
  g_info.category_links_count = 0;
  CategoryLinksHandler data_handler(redis);
  data_handler.init();

  const char *fname = DUMPFILES"categorylinks.sql";
  const char *gzname = DUMPFILES"categorylinks.sql.gz";

  // Open categorylinks.sql
  SystemFile file;
  if (file.open(fname, "rb")) {
    BufferedReader<char> reader(&file);
    reader.set_print_progress(true);
    SqlParser parser(&reader, &data_handler);
    parser.run();
    file.close();
  } else {
    GzipFile gzfile;
    if (gzfile.open(gzname, "rb")) {
      BufferedReader<char> reader(&gzfile);
      reader.set_print_progress(true);
      SqlParser parser(&reader, &data_handler);
      parser.run();
      gzfile.close();
    } else {
      fprintf(stderr, "failed to open file '%s' and '%s'\n", fname, gzname);
    }
  }

  redisReply *reply;
  reply = redisCmd(redis,
        "SET s:count:Category_links %d", g_info.category_links_count);
  freeReplyObject(reply);
}

// mysql table 'categorylinks'
void CategoryLinksHandler::data(const vector<string> &data) {
  int wikiId = atoi(data[cl_from].c_str());
  // If it is not regular page skip it
  if (g_wikistatus[wikiId].type != WikiStatus::REGULAR)
    return;
  int from_graphId = g_wikigraphId[wikiId];

  graph_->start_node(from_graphId);

  const char *prefix = "c:";  // target is always a category

  ComputeHash hash;
  const char *title = data[cl_to].c_str();
  hash.ProcessString(title, data[cl_to].size());

  redisReply *reply;
  reply = redisCmd(redis_, "GET %s%s", prefix, hash.get_hash());
  if (reply->type != REDIS_REPLY_NIL && isdigit(reply->str[0])) {
    int to_graphId = atoi(reply->str);
    freeReplyObject(reply);

#ifdef DEBUG
  printf("categorylink: graphId=%d (wikiId=%d)  to=%s%s graphId=%d  type=%d\n",
    from_graphId, wikiId, prefix, title, to_graphId,
    g_wikistatus[to_graphId].type);
#endif
    g_info.category_links_count++;

    graph_->add_edge(to_graphId);
  } else {
    freeReplyObject(reply);
  }
}  // DataHandler::data

}  // namespace stage4

/**************
 * STAGE 5
 * Transpose _forward_ edges into _backwards_
 * for category inclusion links.
 */
namespace stage5 {

void main_stage5() {
  // Setup output graph
  SystemFile f_out;
  f_out.open("tmp_catlinks_bw.graph", "wb");
  if (true) {  // Destroy objects before closing the file
    BufferedWriter writer(&f_out);
    GraphBuffWriter graph_out(&writer, g_info.graph_nodes_count);
    // NODES_PER_PASS: How much nodes to process in one pass

    node_t nodes = 0;
    node_t last_node = static_cast<node_t>(g_info.graph_nodes_count);
    for (int pass = 1; nodes < last_node; pass++) {
      // Open graph with forward links
      SystemFile f_in;
      f_in.open("tmp_catlinks_fw.graph", "rb");
      BufferedReader<uint32_t> reader(&f_in);
      reader.set_print_progress(true);
      StreamGraphReader graph_in(&reader);
      graph_in.init();

      printf("Pass %d ...\n", pass);

      TransposeGraphPartially transpose(&graph_in,
          nodes+1, nodes+NODES_PER_PASS, &graph_out);
      transpose.run();
      f_in.close();

      nodes += NODES_PER_PASS;  // Progress to next pass
    }
  }
  f_out.close();
}

}  // namespace stage5

/**************
 * STAGE 6
 * Merge graph with _forward_ and graph with _backward_
 * edges into a single one.
 */
namespace stage6 {

void main_stage6() {
  // Open graph with forward links
  SystemFile f_in1;
  f_in1.open("tmp_catlinks_fw.graph", "rb");
  BufferedReader<uint32_t> reader1(&f_in1);
  StreamGraphReader graph_in1(&reader1);
  graph_in1.init();

  // Open graph with backward links
  SystemFile f_in2;
  f_in2.open("tmp_catlinks_bw.graph", "rb");
  BufferedReader<uint32_t> reader2(&f_in2);
  StreamGraphReader graph_in2(&reader2);
  graph_in2.init();

  // Setup output graph
  SystemFile f_out;
  f_out.open("catlinks.graph", "wb");
  if (1) {  // Destroy objects before closing files
    BufferedWriter writer(&f_out);
    GraphBuffWriter graph_out(&writer, g_info.graph_nodes_count);
    AddGraphs merge(&graph_in1, &graph_in2, &graph_out);
    merge.run();
  }
  f_out.close();
  f_in2.close();
  f_in1.close();
  printf("You can delete 'tmp_*.graph'.\n");
}

}  // namespace stage6

}  // namespace wikigraph

int main(int argc, char *argv[]) {
  redisContext *redis = NULL;
#ifdef REDIS_UNIXSOCKET
  redis = redisConnectUnix(REDIS_UNIXSOCKET);
#endif

  if (redis == NULL || redis->err) {
    // Retry to connect to network port if socket fails
    struct timeval timeout = {1, 500000};  // 1.5 seconds
    redis = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, timeout);

    if (redis->err) {
      printf("Connection error: %s\n", redis->errstr);
      exit(1);
    }
  }
  // Select database
  redisReply *reply;
  reply = wikigraph::redisCmd(redis, "SELECT %d", REDIS_DATABASE);
  freeReplyObject(reply);

  // Run though stages
  printf("Starting stage 1 (pages)\n");
  wikigraph::stage1::main_stage1(redis);

  printf("Starting stage 2 (redirects)\n");
  wikigraph::stage2::main_stage2(redis);

  printf("Starting stage 3 (article links)\n");
  wikigraph::stage3::main_stage3(redis);

  printf("Starting stage 4 (category inclusions)\n");
  wikigraph::stage4::main_stage4(redis);

  printf("Starting stage 5 (transpose graph)\n");
  wikigraph::stage5::main_stage5();

  printf("Starting stage 6 (merge fw and bw edges)\n");
  wikigraph::stage6::main_stage6();

  // Save database to disk
  reply = wikigraph::redisCmd(redis, "SAVE");
  freeReplyObject(reply);

  redisFree(redis);
  return 0;
}

