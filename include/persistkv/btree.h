#pragma once

#include <functional>
#include <string>
#include <vector>

#include "persistkv/common.h"
#include "persistkv/pager.h"

namespace pkv {

// Node capacities, derived so that a full node always fits in one page.
constexpr uint32_t kLeafEntryMaxSize = 2 + kMaxKeySize + 2 + kMaxValueSize;
constexpr uint32_t kLeafMaxKeys = (kPageSize - kPageHeaderSize) / kLeafEntryMaxSize;
constexpr uint32_t kLeafMinKeys = kLeafMaxKeys / 2;
constexpr uint32_t kInternalMaxKeys = (kPageSize - kPageHeaderSize - 4) / (4 + 2 + kMaxKeySize);
constexpr uint32_t kInternalMinKeys = kInternalMaxKeys / 2;

static_assert(kLeafMaxKeys >= 4, "page too small for leaf fan-out");
static_assert(kInternalMaxKeys >= 4, "page too small for internal fan-out");

// Deserialized view of one tree page.
//   Leaf:     keys[i] -> values[i], `next` links to the right sibling leaf.
//   Internal: children.size() == keys.size() + 1. Subtree children[i] holds
//             keys in [keys[i-1], keys[i]).
struct Node {
  PageId id = kInvalidPage;
  bool leaf = true;
  PageId next = kInvalidPage;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  std::vector<PageId> children;
};

using ScanFn = std::function<bool(const std::string& key, const std::string& value)>;

class BTree {
 public:
  explicit BTree(Pager& pager) : pager_(pager) {}

  void init();  // creates an empty root leaf for a new database

  bool get(const std::string& key, std::string* value);
  bool put(const std::string& key, const std::string& value);  // true if key was new
  bool remove(const std::string& key);                         // true if key existed

  // Visits keys >= lo (and <= *hi when hi is non-null) in ascending order
  // until fn returns false.
  void scan(const std::string& lo, const std::string* hi, const ScanFn& fn);

  uint32_t height();

  // Walks the whole tree and verifies every structural invariant. Returns an
  // empty string if the tree is healthy, otherwise a description of the
  // first violation found.
  std::string check();

 private:
  struct Split {
    bool happened = false;
    std::string separator;
    PageId right = kInvalidPage;
  };

  Node load(PageId id);
  void store(const Node& n);

  static bool underflows(const Node& n);
  static bool canLend(const Node& n);

  Split insertInto(PageId id, const std::string& key, const std::string& value, bool* inserted);
  bool removeFrom(PageId id, const std::string& key);
  void rebalance(Node& parent, size_t idx, Node& child);
  void merge(Node& parent, size_t left_idx, Node& left, Node& right);

  struct CheckState;
  void checkNode(PageId id, const std::string* lo, const std::string* hi, uint32_t depth, bool is_root,
                 CheckState& st);

  Pager& pager_;
};

}  // namespace pkv
