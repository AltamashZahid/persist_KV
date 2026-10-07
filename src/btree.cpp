#include "persistkv/btree.h"

#include <algorithm>
#include <unordered_set>

namespace pkv {

// ---------------------------------------------------------------------------
// Serialization
//
// Leaf page body:     repeated [klen u16][key][vlen u16][value]
// Internal page body: [child u32] * (n + 1), then repeated [klen u16][key]
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void corrupt(PageId id, const char* why) {
  throw CorruptionError("page " + std::to_string(id) + ": " + why);
}

// Validates the header of a serialized tree page; returns true for a leaf.
bool isLeafPage(const char* page, PageId id) {
  PageType type = static_cast<PageType>(page[kOffType]);
  if (type != PageType::Leaf && type != PageType::Internal) corrupt(id, "not a tree node");
  bool leaf = type == PageType::Leaf;
  if (get16(page + kOffNumKeys) > (leaf ? kLeafMaxKeys : kInternalMaxKeys)) corrupt(id, "too many keys");
  return leaf;
}

// Reads the length-prefixed byte string at `off` and advances past it.
// Returns a pointer into the page; no allocation.
const char* nextBytes(const char* page, PageId id, size_t& off, uint16_t& len) {
  if (off + 2 > kPageSize) corrupt(id, "entry overflows page");
  len = get16(page + off);
  off += 2;
  if (off + len > kPageSize) corrupt(id, "entry overflows page");
  const char* p = page + off;
  off += len;
  return p;
}

// Read-path fast lookups that work directly on the serialized bytes, so the
// hot paths never build a Node (which costs one heap allocation per key).

// Internal page: the child to descend into for `key` (upper-bound rule).
PageId rawChildFor(const char* page, PageId id, const std::string& key, size_t* index = nullptr) {
  uint16_t count = get16(page + kOffNumKeys);
  size_t off = kPageHeaderSize + 4 * (static_cast<size_t>(count) + 1);
  size_t i = 0;
  for (; i < count; i++) {
    uint16_t len;
    const char* k = nextBytes(page, id, off, len);
    if (key.compare(0, key.size(), k, len) < 0) break;
  }
  if (index) *index = i;
  return get32(page + kPageHeaderSize + 4 * i);
}

// Leaf page: finds `key`, copying its value out if `value` is non-null.
bool rawLeafFind(const char* page, PageId id, const std::string& key, std::string* value) {
  uint16_t count = get16(page + kOffNumKeys);
  size_t off = kPageHeaderSize;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t klen, vlen;
    const char* k = nextBytes(page, id, off, klen);
    const char* v = nextBytes(page, id, off, vlen);
    int cmp = key.compare(0, key.size(), k, klen);
    if (cmp == 0) {
      if (value) value->assign(v, vlen);
      return true;
    }
    if (cmp < 0) return false;  // keys are sorted, so it is not here
  }
  return false;
}

Node decode(const char* page, PageId id) {
  Node n;
  n.id = id;
  n.leaf = isLeafPage(page, id);
  n.next = get32(page + kOffNext);
  uint16_t count = get16(page + kOffNumKeys);

  size_t off = kPageHeaderSize;
  auto readBytes = [&](std::string& out) {
    uint16_t len;
    const char* p = nextBytes(page, id, off, len);
    out.assign(p, len);
  };

  n.keys.resize(count);
  if (n.leaf) {
    n.values.resize(count);
    for (uint16_t i = 0; i < count; i++) {
      readBytes(n.keys[i]);
      readBytes(n.values[i]);
    }
  } else {
    n.children.resize(count + 1);
    for (uint16_t i = 0; i <= count; i++) {
      n.children[i] = get32(page + off);
      off += 4;
    }
    for (uint16_t i = 0; i < count; i++) readBytes(n.keys[i]);
  }
  return n;
}

}  // namespace

Node BTree::load(PageId id) {
  char page[kPageSize];
  pager_.read(id, page);
  return decode(page, id);
}

void BTree::store(const Node& n) {
  char page[kPageSize];
  std::memset(page, 0, kPageSize);
  page[kOffType] = static_cast<char>(n.leaf ? PageType::Leaf : PageType::Internal);
  put16(page + kOffNumKeys, static_cast<uint16_t>(n.keys.size()));
  put32(page + kOffNext, n.next);

  size_t off = kPageHeaderSize;
  auto writeBytes = [&](const std::string& s) {
    put16(page + off, static_cast<uint16_t>(s.size()));
    std::memcpy(page + off + 2, s.data(), s.size());
    off += 2 + s.size();
  };
  if (n.leaf) {
    for (size_t i = 0; i < n.keys.size(); i++) {
      writeBytes(n.keys[i]);
      writeBytes(n.values[i]);
    }
  } else {
    for (PageId c : n.children) {
      put32(page + off, c);
      off += 4;
    }
    for (const std::string& k : n.keys) writeBytes(k);
  }
  pager_.write(n.id, page);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool BTree::underflows(const Node& n) {
  return n.keys.size() < (n.leaf ? kLeafMinKeys : kInternalMinKeys);
}

bool BTree::canLend(const Node& n) {
  return n.keys.size() > (n.leaf ? kLeafMinKeys : kInternalMinKeys);
}

void BTree::init() {
  if (pager_.meta().root != kInvalidPage) return;
  Node root;
  root.id = pager_.allocate();
  root.leaf = true;
  store(root);
  pager_.meta().root = root.id;
}

uint32_t BTree::height() {
  uint32_t h = 1;
  Node n = load(pager_.meta().root);
  while (!n.leaf) {
    n = load(n.children[0]);
    h++;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Lookup and range scan
// ---------------------------------------------------------------------------

bool BTree::get(const std::string& key, std::string* value) {
  char page[kPageSize];
  PageId id = pager_.meta().root;
  while (true) {
    pager_.read(id, page);
    if (isLeafPage(page, id)) return rawLeafFind(page, id, key, value);
    id = rawChildFor(page, id, key);
  }
}

void BTree::scan(const std::string& lo, const std::string* hi, const ScanFn& fn) {
  char page[kPageSize];
  PageId id = pager_.meta().root;
  while (true) {
    pager_.read(id, page);
    if (isLeafPage(page, id)) break;
    id = rawChildFor(page, id, lo);
  }

  Node n = decode(page, id);
  size_t i = std::lower_bound(n.keys.begin(), n.keys.end(), lo) - n.keys.begin();
  while (true) {
    for (; i < n.keys.size(); i++) {
      if (hi && n.keys[i] > *hi) return;
      if (!fn(n.keys[i], n.values[i])) return;
    }
    if (n.next == kInvalidPage) return;
    n = load(n.next);  // follow the leaf chain instead of re-descending
    i = 0;
  }
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

bool BTree::put(const std::string& key, const std::string& value) {
  Meta& meta = pager_.meta();
  bool inserted = false;
  Split s = insertInto(meta.root, key, value, &inserted);
  if (s.happened) {
    // The root split: grow the tree by one level.
    Node root;
    root.id = pager_.allocate();
    root.leaf = false;
    root.keys.push_back(s.separator);
    root.children.push_back(meta.root);
    root.children.push_back(s.right);
    store(root);
    meta.root = root.id;
  }
  if (inserted) meta.key_count++;
  return inserted;
}

BTree::Split BTree::insertInto(PageId id, const std::string& key, const std::string& value, bool* inserted) {
  char page[kPageSize];
  pager_.read(id, page);
  Split result;

  if (!isLeafPage(page, id)) {
    // Descend without decoding; this node only changes if its child splits.
    size_t idx;
    PageId child_id = rawChildFor(page, id, key, &idx);
    Split child = insertInto(child_id, key, value, inserted);
    if (!child.happened) return result;

    Node n = decode(page, id);
    n.keys.insert(n.keys.begin() + idx, child.separator);
    n.children.insert(n.children.begin() + idx + 1, child.right);
    if (n.keys.size() <= kInternalMaxKeys) {
      store(n);
      return result;
    }

    // Split the overfull internal node; the middle key moves up (it is not
    // kept in either half).
    Node right;
    right.id = pager_.allocate();
    right.leaf = false;
    size_t mid = n.keys.size() / 2;
    result.separator = n.keys[mid];
    right.keys.assign(n.keys.begin() + mid + 1, n.keys.end());
    right.children.assign(n.children.begin() + mid + 1, n.children.end());
    n.keys.resize(mid);
    n.children.resize(mid + 1);
    store(n);
    store(right);

    result.happened = true;
    result.right = right.id;
    return result;
  }

  // Leaf: insert or overwrite in place, splitting if it overflows.
  Node n = decode(page, id);
  size_t i = std::lower_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
  if (i < n.keys.size() && n.keys[i] == key) {
    n.values[i] = value;
    store(n);
    return result;
  }
  n.keys.insert(n.keys.begin() + i, key);
  n.values.insert(n.values.begin() + i, value);
  *inserted = true;
  if (n.keys.size() <= kLeafMaxKeys) {
    store(n);
    return result;
  }

  // Split the overfull leaf in half; the right half's first key is copied
  // up as the separator (it stays in the leaf too, B+Tree style).
  Node right;
  right.id = pager_.allocate();
  right.leaf = true;
  size_t mid = n.keys.size() / 2;
  right.keys.assign(n.keys.begin() + mid, n.keys.end());
  right.values.assign(n.values.begin() + mid, n.values.end());
  n.keys.resize(mid);
  n.values.resize(mid);
  right.next = n.next;
  n.next = right.id;
  store(n);
  store(right);

  result.happened = true;
  result.separator = right.keys.front();
  result.right = right.id;
  return result;
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

bool BTree::remove(const std::string& key) {
  Meta& meta = pager_.meta();
  if (!removeFrom(meta.root, key)) return false;
  meta.key_count--;

  // If merges emptied the root, the tree shrinks by one level.
  Node root = load(meta.root);
  if (!root.leaf && root.keys.empty()) {
    meta.root = root.children[0];
    pager_.free(root.id);
  }
  return true;
}

bool BTree::removeFrom(PageId id, const std::string& key) {
  char page[kPageSize];
  pager_.read(id, page);

  if (isLeafPage(page, id)) {
    if (!rawLeafFind(page, id, key, nullptr)) return false;
    Node n = decode(page, id);
    size_t i = std::lower_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
    n.keys.erase(n.keys.begin() + i);
    n.values.erase(n.values.begin() + i);
    store(n);
    return true;
  }

  size_t idx;
  PageId child_id = rawChildFor(page, id, key, &idx);
  if (!removeFrom(child_id, key)) return false;

  // Only decode this node and the child if the child actually underflowed.
  char child_page[kPageSize];
  pager_.read(child_id, child_page);
  bool child_leaf = isLeafPage(child_page, child_id);
  if (get16(child_page + kOffNumKeys) >= (child_leaf ? kLeafMinKeys : kInternalMinKeys)) return true;

  Node n = decode(page, id);
  Node child = decode(child_page, child_id);
  rebalance(n, idx, child);
  store(n);
  return true;
}

// Fixes an underfull child by borrowing one entry from a sibling that has
// spare keys, or else merging it with a sibling.
void BTree::rebalance(Node& parent, size_t idx, Node& child) {
  Node left, right;
  bool has_left = idx > 0;
  bool has_right = idx + 1 < parent.children.size();

  if (has_left) {
    left = load(parent.children[idx - 1]);
    if (canLend(left)) {
      if (child.leaf) {
        child.keys.insert(child.keys.begin(), left.keys.back());
        child.values.insert(child.values.begin(), left.values.back());
        left.keys.pop_back();
        left.values.pop_back();
        parent.keys[idx - 1] = child.keys.front();
      } else {
        // Rotate right through the parent separator.
        child.keys.insert(child.keys.begin(), parent.keys[idx - 1]);
        child.children.insert(child.children.begin(), left.children.back());
        parent.keys[idx - 1] = left.keys.back();
        left.keys.pop_back();
        left.children.pop_back();
      }
      store(left);
      store(child);
      return;
    }
  }

  if (has_right) {
    right = load(parent.children[idx + 1]);
    if (canLend(right)) {
      if (child.leaf) {
        child.keys.push_back(right.keys.front());
        child.values.push_back(right.values.front());
        right.keys.erase(right.keys.begin());
        right.values.erase(right.values.begin());
        parent.keys[idx] = right.keys.front();
      } else {
        // Rotate left through the parent separator.
        child.keys.push_back(parent.keys[idx]);
        child.children.push_back(right.children.front());
        parent.keys[idx] = right.keys.front();
        right.keys.erase(right.keys.begin());
        right.children.erase(right.children.begin());
      }
      store(right);
      store(child);
      return;
    }
  }

  // Neither sibling can lend, so both are at minimum occupancy and a merge
  // is guaranteed to fit in one node.
  if (has_left) {
    merge(parent, idx - 1, left, child);
  } else {
    merge(parent, idx, child, right);
  }
}

// Folds `right` (parent.children[left_idx + 1]) into `left` and frees it.
void BTree::merge(Node& parent, size_t left_idx, Node& left, Node& right) {
  if (left.leaf) {
    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.values.insert(left.values.end(), right.values.begin(), right.values.end());
    left.next = right.next;
  } else {
    left.keys.push_back(parent.keys[left_idx]);  // pull the separator down
    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.children.insert(left.children.end(), right.children.begin(), right.children.end());
  }
  parent.keys.erase(parent.keys.begin() + left_idx);
  parent.children.erase(parent.children.begin() + left_idx + 1);
  store(left);
  pager_.free(right.id);
}

// ---------------------------------------------------------------------------
// Integrity check
// ---------------------------------------------------------------------------

struct BTree::CheckState {
  uint32_t leaf_depth = 0;
  std::vector<PageId> leaves;
  std::unordered_set<PageId> seen;
  uint64_t keys = 0;
};

void BTree::checkNode(PageId id, const std::string* lo, const std::string* hi, uint32_t depth, bool is_root,
                      CheckState& st) {
  auto fail = [&](const std::string& why) {
    throw CorruptionError("page " + std::to_string(id) + ": " + why);
  };
  if (!st.seen.insert(id).second) fail("reachable twice (cycle or shared child)");

  Node n = load(id);
  for (size_t i = 0; i < n.keys.size(); i++) {
    if (i > 0 && !(n.keys[i - 1] < n.keys[i])) fail("keys not strictly sorted");
    if (lo && n.keys[i] < *lo) fail("key below parent separator");
    if (hi && !(n.keys[i] < *hi)) fail("key at or above parent separator");
  }
  if (!is_root && underflows(n)) fail("underfull node");

  if (n.leaf) {
    if (st.leaf_depth == 0) st.leaf_depth = depth;
    if (depth != st.leaf_depth) fail("leaves at different depths");
    st.leaves.push_back(id);
    st.keys += n.keys.size();
    return;
  }

  if (n.children.size() != n.keys.size() + 1) fail("child count != key count + 1");
  if (n.keys.empty()) fail("internal node with no keys");
  for (size_t i = 0; i < n.children.size(); i++) {
    const std::string* clo = i == 0 ? lo : &n.keys[i - 1];
    const std::string* chi = i == n.keys.size() ? hi : &n.keys[i];
    checkNode(n.children[i], clo, chi, depth + 1, false, st);
  }
}

std::string BTree::check() {
  try {
    const Meta& meta = pager_.meta();
    CheckState st;
    checkNode(meta.root, nullptr, nullptr, 1, true, st);

    // The leaf sibling chain must visit exactly the leaves, in key order.
    PageId cur = st.leaves.front();
    for (size_t i = 0; i < st.leaves.size(); i++) {
      if (cur != st.leaves[i]) return "leaf chain out of order at leaf " + std::to_string(i);
      cur = load(cur).next;
    }
    if (cur != kInvalidPage) return "leaf chain continues past the last leaf";

    if (st.keys != meta.key_count) {
      return "key count mismatch: tree has " + std::to_string(st.keys) + ", meta says " +
             std::to_string(meta.key_count);
    }

    // Every page is either the meta page, in the tree, or on the free list.
    uint64_t free_pages = 0;
    for (PageId f = meta.free_head; f != kInvalidPage;) {
      if (st.seen.count(f)) return "page " + std::to_string(f) + " is both free and in the tree";
      if (++free_pages > meta.page_count) return "free list has a cycle";
      char page[kPageSize];
      pager_.read(f, page);
      f = get32(page + kOffNext);
    }
    if (1 + st.seen.size() + free_pages != meta.page_count) {
      return "page leak: " + std::to_string(meta.page_count) + " pages, " + std::to_string(st.seen.size()) +
             " in tree, " + std::to_string(free_pages) + " free";
    }
    return "";
  } catch (const std::exception& e) {
    return e.what();
  }
}

}  // namespace pkv
