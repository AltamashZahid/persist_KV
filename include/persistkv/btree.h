#pragma once

#include <functional>
#include <string>
#include <vector>

#include "persistkv/common.h"
#include "persistkv/pager.h"

namespace pkv {

// Nodes are sized in bytes, not entries: a node is full when its serialized
// body exceeds kNodeCapacity and underfull (if it is not the root) when it
// uses fewer than kNodeMinFill bytes. Small entries pack hundreds to a page.
constexpr uint32_t kNodeCapacity = kPageSize - kPageHeaderSize;
constexpr uint32_t kNodeMinFill = kNodeCapacity / 4;

// Values longer than this are stored in a chain of overflow pages; the leaf
// keeps only a fixed-size reference to the chain.
constexpr uint32_t kMaxInlineValue = 512;
constexpr uint32_t kOverflowPayload = kPageSize - kPageHeaderSize;

// Worst-case entry sizes. Keeping each at most half a page guarantees that
// splitting or redistributing always yields two nodes that fit and are at
// least kNodeMinFill bytes.
constexpr uint32_t kMaxLeafEntry = 2 + kMaxKeySize + 1 + 2 + kMaxInlineValue;
constexpr uint32_t kMaxInternalEntry = 2 + kMaxKeySize + 4;
static_assert(kMaxLeafEntry <= kNodeCapacity / 2, "leaf entries too large for the page size");
static_assert(kMaxInternalEntry <= kNodeCapacity / 2, "keys too large for the page size");

// Deserialized view of one tree page.
//   Leaf:     keys[i] -> cells[i], `next` links to the right sibling leaf.
//             A cell is the encoded value: [0][vlen u16][bytes] inline, or
//             [1][vlen u32][first overflow page u32] for large values.
//   Internal: children.size() == keys.size() + 1. Subtree children[i] holds
//             keys in [keys[i-1], keys[i]).
struct Node {
  PageId id = kInvalidPage;
  bool leaf = true;
  PageId next = kInvalidPage;
  std::vector<std::string> keys;
  std::vector<std::string> cells;
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
  // One put (value != nullptr) or delete travelling down the tree.
  struct Mutation {
    const std::string* key;
    const std::string* value;
    bool inserted = false;
    bool removed = false;
  };

  Node load(PageId id);
  void store(const Node& n);

  void apply(Mutation& m);
  bool modify(PageId id, Mutation& m, Node* out);
  void fixChild(Node& parent, size_t idx, Node& child);
  void splitNode(Node& n, Node& right, std::string* separator);
  void rebalance(Node& parent, size_t idx, Node& child);

  std::string makeCell(const std::string& value);
  void readCell(const std::string& cell, std::string* value);
  void freeCell(const std::string& cell);

  struct CheckState;
  void checkNode(PageId id, const std::string* lo, const std::string* hi, uint32_t depth, bool is_root,
                 CheckState& st);
  void checkCell(PageId leaf, const std::string& cell, CheckState& st);

  Pager& pager_;
};

}  // namespace pkv
