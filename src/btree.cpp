#include "persistkv/btree.h"

#include <algorithm>
#include <unordered_set>

namespace pkv {

// ---------------------------------------------------------------------------
// Serialization
//
// Leaf page body:     repeated [klen u16][key][cell]
//   cell, inline:     [0][vlen u16][value bytes]
//   cell, overflow:   [1][vlen u32][first overflow page u32]
// Internal page body: [child u32] * (n + 1), then repeated [klen u16][key]
// Overflow page:      header (bytes used in the num_keys field, next page in
//                     the next field), then up to kOverflowPayload bytes
// ---------------------------------------------------------------------------

namespace {

constexpr uint8_t kCellInline = 0;
constexpr uint8_t kCellOverflow = 1;
constexpr size_t kOverflowCellSize = 1 + 4 + 4;

[[noreturn]] void corrupt(PageId id, const std::string& why) {
  throw CorruptionError("page " + std::to_string(id) + ": " + why);
}

// Validates the header of a serialized tree page; returns true for a leaf.
bool isLeafPage(const char* page, PageId id) {
  PageType type = static_cast<PageType>(page[kOffType]);
  if (type != PageType::Leaf && type != PageType::Internal) corrupt(id, "not a tree node");
  return type == PageType::Leaf;
}

// Bounds-checked cursor over a page, so a corrupt length can never make us
// read past the end of the buffer.
struct Reader {
  const char* page;
  PageId id;
  size_t off;

  void need(size_t n) const {
    if (off + n > kPageSize) corrupt(id, "entry overflows page");
  }
  uint8_t u8() {
    need(1);
    return static_cast<uint8_t>(page[off++]);
  }
  uint16_t u16() {
    need(2);
    uint16_t v = get16(page + off);
    off += 2;
    return v;
  }
  uint32_t u32() {
    need(4);
    uint32_t v = get32(page + off);
    off += 4;
    return v;
  }
  const char* bytes(size_t n) {
    need(n);
    const char* p = page + off;
    off += n;
    return p;
  }
  // Skips over one value cell and returns where it started.
  const char* cell(size_t* len) {
    size_t start = off;
    uint8_t tag = u8();
    if (tag == kCellInline) {
      bytes(u16());
    } else if (tag == kCellOverflow) {
      u32();
      u32();
    } else {
      corrupt(id, "unknown value cell type");
    }
    *len = off - start;
    return page + start;
  }
};

size_t keyCount(const char* page, PageId id, bool leaf) {
  uint16_t count = get16(page + kOffNumKeys);
  // Every entry takes at least 3 bytes (leaf) or 6 bytes (internal).
  if (count > (leaf ? kNodeCapacity / 3 : (kNodeCapacity - 4) / 6)) corrupt(id, "too many keys");
  return count;
}

// Read-path fast lookups that work directly on the serialized bytes, so the
// hot paths never build a Node (which costs one heap allocation per key).

// Internal page: the child to descend into for `key` (upper-bound rule).
PageId rawChildFor(const char* page, PageId id, const std::string& key, size_t* index = nullptr) {
  size_t count = keyCount(page, id, false);
  Reader r{page, id, kPageHeaderSize + 4 * (count + 1)};
  size_t i = 0;
  for (; i < count; i++) {
    uint16_t len = r.u16();
    const char* k = r.bytes(len);
    if (key.compare(0, key.size(), k, len) < 0) break;
  }
  if (index) *index = i;
  return get32(page + kPageHeaderSize + 4 * i);
}

// Leaf page: finds `key`, copying its encoded value cell out if requested.
bool rawLeafFind(const char* page, PageId id, const std::string& key, std::string* cell) {
  size_t count = keyCount(page, id, true);
  Reader r{page, id, kPageHeaderSize};
  for (size_t i = 0; i < count; i++) {
    uint16_t klen = r.u16();
    const char* k = r.bytes(klen);
    size_t clen;
    const char* c = r.cell(&clen);
    int cmp = key.compare(0, key.size(), k, klen);
    if (cmp == 0) {
      if (cell) cell->assign(c, clen);
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
  size_t count = keyCount(page, id, n.leaf);

  n.keys.resize(count);
  if (n.leaf) {
    Reader r{page, id, kPageHeaderSize};
    n.cells.resize(count);
    for (size_t i = 0; i < count; i++) {
      uint16_t klen = r.u16();
      n.keys[i].assign(r.bytes(klen), klen);
      size_t clen;
      const char* c = r.cell(&clen);
      n.cells[i].assign(c, clen);
    }
  } else {
    n.children.resize(count + 1);
    for (size_t i = 0; i <= count; i++) n.children[i] = get32(page + kPageHeaderSize + 4 * i);
    Reader r{page, id, kPageHeaderSize + 4 * (count + 1)};
    for (size_t i = 0; i < count; i++) {
      uint16_t klen = r.u16();
      n.keys[i].assign(r.bytes(klen), klen);
    }
  }
  return n;
}

// Serialized size of a node's body. This, not the entry count, decides when
// a node splits or needs rebalancing.
size_t nodeBytes(const Node& n) {
  size_t bytes = 0;
  if (n.leaf) {
    for (size_t i = 0; i < n.keys.size(); i++) bytes += 2 + n.keys[i].size() + n.cells[i].size();
  } else {
    bytes = 4 * n.children.size();
    for (const std::string& k : n.keys) bytes += 2 + k.size();
  }
  return bytes;
}

}  // namespace

Node BTree::load(PageId id) {
  char page[kPageSize];
  pager_.read(id, page);
  return decode(page, id);
}

void BTree::store(const Node& n) {
  if (nodeBytes(n) > kNodeCapacity) throw Error("internal error: node " + std::to_string(n.id) + " overflows");
  char page[kPageSize];
  std::memset(page, 0, kPageSize);
  page[kOffType] = static_cast<char>(n.leaf ? PageType::Leaf : PageType::Internal);
  put16(page + kOffNumKeys, static_cast<uint16_t>(n.keys.size()));
  put32(page + kOffNext, n.next);

  size_t off = kPageHeaderSize;
  auto writeKey = [&](const std::string& k) {
    put16(page + off, static_cast<uint16_t>(k.size()));
    std::memcpy(page + off + 2, k.data(), k.size());
    off += 2 + k.size();
  };
  if (n.leaf) {
    for (size_t i = 0; i < n.keys.size(); i++) {
      writeKey(n.keys[i]);
      std::memcpy(page + off, n.cells[i].data(), n.cells[i].size());
      off += n.cells[i].size();
    }
  } else {
    for (PageId c : n.children) {
      put32(page + off, c);
      off += 4;
    }
    for (const std::string& k : n.keys) writeKey(k);
  }
  pager_.write(n.id, page);
}

// ---------------------------------------------------------------------------
// Value cells and overflow chains
// ---------------------------------------------------------------------------

std::string BTree::makeCell(const std::string& value) {
  std::string cell;
  if (value.size() <= kMaxInlineValue) {
    cell.resize(3 + value.size());
    cell[0] = static_cast<char>(kCellInline);
    put16(&cell[1], static_cast<uint16_t>(value.size()));
    if (!value.empty()) std::memcpy(&cell[3], value.data(), value.size());
    return cell;
  }

  // Write the chain back to front so each page can record its successor.
  size_t pages = (value.size() + kOverflowPayload - 1) / kOverflowPayload;
  PageId next = kInvalidPage;
  char page[kPageSize];
  for (size_t p = pages; p-- > 0;) {
    size_t off = p * kOverflowPayload;
    size_t n = std::min<size_t>(kOverflowPayload, value.size() - off);
    std::memset(page, 0, kPageSize);
    page[kOffType] = static_cast<char>(PageType::Overflow);
    put16(page + kOffNumKeys, static_cast<uint16_t>(n));
    put32(page + kOffNext, next);
    std::memcpy(page + kPageHeaderSize, value.data() + off, n);
    PageId id = pager_.allocate();
    pager_.write(id, page);
    next = id;
  }
  cell.resize(kOverflowCellSize);
  cell[0] = static_cast<char>(kCellOverflow);
  put32(&cell[1], static_cast<uint32_t>(value.size()));
  put32(&cell[5], next);
  return cell;
}

void BTree::readCell(const std::string& cell, std::string* value) {
  if (static_cast<uint8_t>(cell[0]) == kCellInline) {
    value->assign(cell.data() + 3, get16(cell.data() + 1));
    return;
  }
  uint32_t total = get32(cell.data() + 1);
  PageId id = get32(cell.data() + 5);
  value->clear();
  value->reserve(total);
  char page[kPageSize];
  while (value->size() < total) {
    if (id == kInvalidPage) throw CorruptionError("overflow chain ends early");
    pager_.read(id, page);
    if (static_cast<PageType>(page[kOffType]) != PageType::Overflow) corrupt(id, "expected an overflow page");
    uint16_t n = get16(page + kOffNumKeys);
    if (n == 0 || n > kOverflowPayload || value->size() + n > total) corrupt(id, "bad overflow page length");
    value->append(page + kPageHeaderSize, n);
    id = get32(page + kOffNext);
  }
}

void BTree::freeCell(const std::string& cell) {
  if (static_cast<uint8_t>(cell[0]) != kCellOverflow) return;
  PageId id = get32(cell.data() + 5);
  char page[kPageSize];
  while (id != kInvalidPage) {
    pager_.read(id, page);
    PageId next = get32(page + kOffNext);
    pager_.free(id);
    id = next;
  }
}

// ---------------------------------------------------------------------------
// Lookup and range scan
// ---------------------------------------------------------------------------

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

bool BTree::get(const std::string& key, std::string* value) {
  char page[kPageSize];
  PageId id = pager_.meta().root;
  while (true) {
    pager_.read(id, page);
    if (isLeafPage(page, id)) break;
    id = rawChildFor(page, id, key);
  }
  std::string cell;
  if (!rawLeafFind(page, id, key, value ? &cell : nullptr)) return false;
  if (value) readCell(cell, value);
  return true;
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
  std::string value;
  while (true) {
    for (; i < n.keys.size(); i++) {
      if (hi && n.keys[i] > *hi) return;
      readCell(n.cells[i], &value);
      if (!fn(n.keys[i], value)) return;
    }
    if (n.next == kInvalidPage) return;
    n = load(n.next);  // follow the leaf chain instead of re-descending
    i = 0;
  }
}

// ---------------------------------------------------------------------------
// Modification
//
// A put or delete descends to the leaf and changes it in memory. On the way
// back up, each parent inspects the changed child: if it overflows the page
// it is split, if it fell below kNodeMinFill it is merged with or rebalanced
// against a sibling, otherwise it is simply stored. Because keys have
// different lengths, those fixes can in turn change the parent's size in
// either direction, so the same check repeats at every level up to the root.
// ---------------------------------------------------------------------------

bool BTree::put(const std::string& key, const std::string& value) {
  Mutation m{&key, &value};
  apply(m);
  return m.inserted;
}

bool BTree::remove(const std::string& key) {
  Mutation m{&key, nullptr};
  apply(m);
  return m.removed;
}

void BTree::apply(Mutation& m) {
  Meta& meta = pager_.meta();
  Node root;
  if (!modify(meta.root, m, &root)) return;

  if (nodeBytes(root) > kNodeCapacity) {
    // The root split: grow the tree by one level.
    Node right;
    right.id = pager_.allocate();
    std::string separator;
    splitNode(root, right, &separator);
    store(root);
    store(right);

    Node new_root;
    new_root.id = pager_.allocate();
    new_root.leaf = false;
    new_root.keys.push_back(separator);
    new_root.children.push_back(root.id);
    new_root.children.push_back(right.id);
    store(new_root);
    meta.root = new_root.id;
  } else if (!root.leaf && root.keys.empty()) {
    // Merges emptied the root: the tree shrinks by one level.
    meta.root = root.children[0];
    pager_.free(root.id);
  } else {
    store(root);  // the root alone may be underfull
  }

  if (m.inserted) meta.key_count++;
  if (m.removed) meta.key_count--;
}

// Applies the mutation inside the subtree at `id`. Returns false if nothing
// changed; otherwise *out is the modified node, not yet stored, which the
// caller must split, rebalance, or store.
bool BTree::modify(PageId id, Mutation& m, Node* out) {
  char page[kPageSize];
  pager_.read(id, page);

  if (isLeafPage(page, id)) {
    if (!m.value && !rawLeafFind(page, id, *m.key, nullptr)) return false;  // delete of a missing key
    Node n = decode(page, id);
    size_t i = std::lower_bound(n.keys.begin(), n.keys.end(), *m.key) - n.keys.begin();
    bool found = i < n.keys.size() && n.keys[i] == *m.key;

    if (!m.value) {
      freeCell(n.cells[i]);
      n.keys.erase(n.keys.begin() + i);
      n.cells.erase(n.cells.begin() + i);
      m.removed = true;
    } else if (found) {
      freeCell(n.cells[i]);
      n.cells[i] = makeCell(*m.value);
    } else {
      n.keys.insert(n.keys.begin() + i, *m.key);
      n.cells.insert(n.cells.begin() + i, makeCell(*m.value));
      m.inserted = true;
    }
    *out = std::move(n);
    return true;
  }

  // Descend without decoding; this node only changes if its child does.
  size_t idx;
  PageId child_id = rawChildFor(page, id, *m.key, &idx);
  Node child;
  if (!modify(child_id, m, &child)) return false;

  Node n = decode(page, id);
  fixChild(n, idx, child);
  *out = std::move(n);
  return true;
}

// Stores parent.children[idx] after a modification, splitting or
// rebalancing it first if needed. May change `parent` in memory.
void BTree::fixChild(Node& parent, size_t idx, Node& child) {
  size_t bytes = nodeBytes(child);
  if (bytes > kNodeCapacity) {
    Node right;
    right.id = pager_.allocate();
    std::string separator;
    splitNode(child, right, &separator);
    store(child);
    store(right);
    parent.keys.insert(parent.keys.begin() + idx, separator);
    parent.children.insert(parent.children.begin() + idx + 1, right.id);
  } else if (bytes < kNodeMinFill) {
    rebalance(parent, idx, child);
  } else {
    store(child);
  }
}

// Splits `n` into two halves of roughly equal size in bytes: `n` keeps the
// lower half and its page, `right` (whose id must be set) gets the upper
// half. *separator is the key the parent should route on.
//   Leaf:     the separator is copied up; it stays as right's first key.
//   Internal: the separator moves up and is kept in neither half.
void BTree::splitNode(Node& n, Node& right, std::string* separator) {
  right.leaf = n.leaf;
  size_t count = n.keys.size();
  std::vector<size_t> size(count);
  size_t total = 0;
  for (size_t i = 0; i < count; i++) {
    size[i] = n.leaf ? 2 + n.keys[i].size() + n.cells[i].size() : 2 + n.keys[i].size() + 4;
    total += size[i];
  }

  // Choose the cut that minimizes the larger half.
  size_t best = 0, best_cost = SIZE_MAX, prefix = 0;
  for (size_t m = 1; m < count; m++) {
    prefix += size[m - 1];
    size_t left, rest;
    if (n.leaf) {
      left = prefix;
      rest = total - prefix;
    } else {
      if (m + 1 >= count) break;  // the right half needs at least one key
      left = 4 + prefix;
      rest = 4 + total - prefix - size[m];
    }
    size_t cost = std::max(left, rest);
    if (cost < best_cost) {
      best_cost = cost;
      best = m;
    }
  }
  if (best == 0) throw Error("internal error: node " + std::to_string(n.id) + " cannot be split");

  if (n.leaf) {
    right.keys.assign(n.keys.begin() + best, n.keys.end());
    right.cells.assign(n.cells.begin() + best, n.cells.end());
    n.keys.resize(best);
    n.cells.resize(best);
    right.next = n.next;
    n.next = right.id;
    *separator = right.keys.front();
  } else {
    *separator = n.keys[best];
    right.keys.assign(n.keys.begin() + best + 1, n.keys.end());
    right.children.assign(n.children.begin() + best + 1, n.children.end());
    n.keys.resize(best);
    n.children.resize(best + 1);
  }
}

// Fixes an underfull child together with one sibling: if the two fit in one
// page they merge (and the right page is freed); otherwise their combined
// entries are redistributed evenly between the two pages.
void BTree::rebalance(Node& parent, size_t idx, Node& child) {
  size_t sep;  // index of the separator between left and right in parent
  Node left, right;
  if (idx > 0) {
    sep = idx - 1;
    left = load(parent.children[sep]);
    right = std::move(child);
  } else {
    sep = idx;
    left = std::move(child);
    right = load(parent.children[idx + 1]);
  }

  Node merged = std::move(left);
  if (merged.leaf) {
    merged.keys.insert(merged.keys.end(), right.keys.begin(), right.keys.end());
    merged.cells.insert(merged.cells.end(), right.cells.begin(), right.cells.end());
    merged.next = right.next;
  } else {
    merged.keys.push_back(parent.keys[sep]);  // pull the separator down
    merged.keys.insert(merged.keys.end(), right.keys.begin(), right.keys.end());
    merged.children.insert(merged.children.end(), right.children.begin(), right.children.end());
  }

  if (nodeBytes(merged) <= kNodeCapacity) {
    store(merged);
    parent.keys.erase(parent.keys.begin() + sep);
    parent.children.erase(parent.children.begin() + sep + 1);
    pager_.free(right.id);
  } else {
    Node new_right;
    new_right.id = right.id;
    splitNode(merged, new_right, &parent.keys[sep]);
    store(merged);
    store(new_right);
  }
}

// ---------------------------------------------------------------------------
// Integrity check
// ---------------------------------------------------------------------------

struct BTree::CheckState {
  uint32_t leaf_depth = 0;
  std::vector<PageId> leaves;
  std::unordered_set<PageId> seen;  // tree pages and overflow pages
  uint64_t keys = 0;
};

void BTree::checkCell(PageId leaf, const std::string& cell, CheckState& st) {
  if (static_cast<uint8_t>(cell[0]) != kCellOverflow) return;
  uint32_t total = get32(cell.data() + 1);
  if (total <= kMaxInlineValue) corrupt(leaf, "small value stored out of line");
  uint64_t bytes = 0;
  char page[kPageSize];
  for (PageId id = get32(cell.data() + 5); id != kInvalidPage;) {
    if (!st.seen.insert(id).second) corrupt(id, "overflow page reachable twice");
    pager_.read(id, page);
    if (static_cast<PageType>(page[kOffType]) != PageType::Overflow) corrupt(id, "expected an overflow page");
    bytes += get16(page + kOffNumKeys);
    id = get32(page + kOffNext);
  }
  if (bytes != total) corrupt(leaf, "overflow chain length does not match the value size");
}

void BTree::checkNode(PageId id, const std::string* lo, const std::string* hi, uint32_t depth, bool is_root,
                      CheckState& st) {
  if (!st.seen.insert(id).second) corrupt(id, "reachable twice (cycle or shared child)");

  Node n = load(id);
  for (size_t i = 0; i < n.keys.size(); i++) {
    if (i > 0 && !(n.keys[i - 1] < n.keys[i])) corrupt(id, "keys not strictly sorted");
    if (lo && n.keys[i] < *lo) corrupt(id, "key below parent separator");
    if (hi && !(n.keys[i] < *hi)) corrupt(id, "key at or above parent separator");
  }
  size_t bytes = nodeBytes(n);
  if (!is_root && bytes < kNodeMinFill) corrupt(id, "underfull node (" + std::to_string(bytes) + " bytes)");

  if (n.leaf) {
    if (st.leaf_depth == 0) st.leaf_depth = depth;
    if (depth != st.leaf_depth) corrupt(id, "leaves at different depths");
    st.leaves.push_back(id);
    st.keys += n.keys.size();
    for (const std::string& cell : n.cells) checkCell(id, cell, st);
    return;
  }

  if (n.keys.empty()) corrupt(id, "internal node with no keys");
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

    // Every page is the meta page, a tree or overflow page, or on the free list.
    uint64_t free_pages = 0;
    for (PageId f = meta.free_head; f != kInvalidPage;) {
      if (st.seen.count(f)) return "page " + std::to_string(f) + " is both free and in use";
      if (++free_pages > meta.page_count) return "free list has a cycle";
      char page[kPageSize];
      pager_.read(f, page);
      f = get32(page + kOffNext);
    }
    if (1 + st.seen.size() + free_pages != meta.page_count) {
      return "page leak: " + std::to_string(meta.page_count) + " pages, " + std::to_string(st.seen.size()) +
             " in use, " + std::to_string(free_pages) + " free";
    }
    return "";
  } catch (const std::exception& e) {
    return e.what();
  }
}

}  // namespace pkv
