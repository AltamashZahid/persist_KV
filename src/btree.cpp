#include "persistkv/btree.h"

#include <algorithm>
#include <unordered_set>

namespace pkv {

// ---------------------------------------------------------------------------
// Serialization
//
// Pages are "slotted": an array of 2-byte entry offsets precedes the
// entries, so a lookup can binary-search a page instead of scanning it.
//
// Leaf page body:     [slot u16] * n, then n entries [klen u16][key][cell]
//   cell, inline:     [0][vlen u16][value bytes]
//   cell, overflow:   [1][vlen u32][first overflow page u32]
// Internal page body: [child u32] * (n + 1), [slot u16] * n, then n keys [klen u16][key]
// (slot i is the byte offset, from the page start, of entry / key i)
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
  // A leaf entry takes at least 8 bytes (slot, length, key, cell) and an
  // internal key at least 9 (child pointer, slot, length, key).
  if (count > (leaf ? kNodeCapacity / 8 : (kNodeCapacity - 4) / 9)) corrupt(id, "too many keys");
  return count;
}

// Offset of the slot array.
size_t slotBase(bool leaf, size_t count) { return kPageHeaderSize + (leaf ? 0 : 4 * (count + 1)); }

// Offset of entry i, from its slot.
size_t slotAt(const char* page, PageId id, size_t base, size_t i) {
  size_t off = get16(page + base + 2 * i);
  if (off < kPageHeaderSize || off + 2 > kPageSize) corrupt(id, "bad slot offset");
  return off;
}

// Key i of a page (its slot points at [klen][key...]).
const char* keyAt(const char* page, PageId id, size_t base, size_t i, uint16_t* len) {
  size_t off = slotAt(page, id, base, i);
  *len = get16(page + off);
  if (off + 2 + *len > kPageSize) corrupt(id, "entry overflows page");
  return page + off + 2;
}

// Read-path fast lookups that work directly on the serialized bytes, so the
// hot paths never build a Node (which costs one heap allocation per key).

// Internal page: the child to descend into for `key` (upper-bound rule),
// found by binary search over the slots.
PageId rawChildFor(const char* page, PageId id, const std::string& key, size_t* index = nullptr) {
  size_t count = keyCount(page, id, false);
  size_t base = slotBase(false, count);
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    uint16_t len;
    const char* k = keyAt(page, id, base, mid, &len);
    if (key.compare(0, key.size(), k, len) < 0) hi = mid;
    else lo = mid + 1;
  }
  if (index) *index = lo;
  return get32(page + kPageHeaderSize + 4 * lo);
}

// Leaf page: index of the first entry with key >= `key` (binary search).
size_t leafLowerBound(const char* page, PageId id, size_t count, const std::string& key) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    uint16_t len;
    const char* k = keyAt(page, id, kPageHeaderSize, mid, &len);
    if (key.compare(0, key.size(), k, len) <= 0) hi = mid;
    else lo = mid + 1;
  }
  return lo;
}

// Leaf page: finds `key`, copying its encoded value cell out if requested.
bool rawLeafFind(const char* page, PageId id, const std::string& key, std::string* cell) {
  size_t count = keyCount(page, id, true);
  size_t i = leafLowerBound(page, id, count, key);
  if (i == count) return false;
  Reader r{page, id, slotAt(page, id, kPageHeaderSize, i)};
  uint16_t klen = r.u16();
  const char* k = r.bytes(klen);
  if (key.compare(0, key.size(), k, klen) != 0) return false;
  size_t clen;
  const char* c = r.cell(&clen);
  if (cell) cell->assign(c, clen);
  return true;
}

Node decode(const char* page, PageId id) {
  Node n;
  n.id = id;
  n.leaf = isLeafPage(page, id);
  n.next = get32(page + kOffNext);
  size_t count = keyCount(page, id, n.leaf);

  n.keys.resize(count);
  size_t base = slotBase(n.leaf, count);
  if (base + 2 * count > kPageSize) corrupt(id, "slot array overflows page");
  if (n.leaf) {
    n.cells.resize(count);
    for (size_t i = 0; i < count; i++) {
      Reader r{page, id, slotAt(page, id, base, i)};
      uint16_t klen = r.u16();
      n.keys[i].assign(r.bytes(klen), klen);
      size_t clen;
      const char* c = r.cell(&clen);
      n.cells[i].assign(c, clen);
    }
  } else {
    n.children.resize(count + 1);
    for (size_t i = 0; i <= count; i++) n.children[i] = get32(page + kPageHeaderSize + 4 * i);
    for (size_t i = 0; i < count; i++) {
      uint16_t klen;
      const char* k = keyAt(page, id, base, i, &klen);
      n.keys[i].assign(k, klen);
    }
  }
  return n;
}

// Serialized size of a node's body. This, not the entry count, decides when
// a node splits or needs rebalancing.
size_t nodeBytes(const Node& n) {
  size_t bytes = 2 * n.keys.size();  // slots
  if (n.leaf) {
    for (size_t i = 0; i < n.keys.size(); i++) bytes += 2 + n.keys[i].size() + n.cells[i].size();
  } else {
    bytes += 4 * n.children.size();
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

  size_t count = n.keys.size();
  size_t base = slotBase(n.leaf, count);
  size_t off = base + 2 * count;  // entries start after the slot array
  auto writeKey = [&](size_t i, const std::string& k) {
    put16(page + base + 2 * i, static_cast<uint16_t>(off));
    put16(page + off, static_cast<uint16_t>(k.size()));
    std::memcpy(page + off + 2, k.data(), k.size());
    off += 2 + k.size();
  };
  if (n.leaf) {
    for (size_t i = 0; i < count; i++) {
      writeKey(i, n.keys[i]);
      std::memcpy(page + off, n.cells[i].data(), n.cells[i].size());
      off += n.cells[i].size();
    }
  } else {
    for (size_t i = 0; i < n.children.size(); i++) put32(page + kPageHeaderSize + 4 * i, n.children[i]);
    for (size_t i = 0; i < count; i++) writeKey(i, n.keys[i]);
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
  // Each level is searched in place in the page cache: no 4 KB page copy
  // per level, only the matching cell is copied out.
  PageId id = pager_.meta().root;
  std::string cell;
  bool leaf = false, found = false;
  while (!leaf) {
    PageId child = kInvalidPage;
    pager_.visit(id, [&](const char* page) {
      leaf = isLeafPage(page, id);
      if (leaf) found = rawLeafFind(page, id, key, value ? &cell : nullptr);
      else child = rawChildFor(page, id, key);
    });
    if (!leaf) id = child;
  }
  if (!found) return false;
  if (value) readCell(cell, value);
  return true;
}

bool BTree::getRaw(const std::string& key, const RawValueFn& fn) {
  PageId id = pager_.meta().root;
  std::string overflow_cell;  // a value stored out of line is assembled after the search
  bool leaf = false, found = false;
  while (!leaf) {
    PageId child = kInvalidPage;
    pager_.visit(id, [&](const char* page) {
      leaf = isLeafPage(page, id);
      if (!leaf) {
        child = rawChildFor(page, id, key);
        return;
      }
      size_t count = keyCount(page, id, true);
      size_t i = leafLowerBound(page, id, count, key);
      if (i == count) return;
      Reader r{page, id, slotAt(page, id, kPageHeaderSize, i)};
      uint16_t klen = r.u16();
      const char* k = r.bytes(klen);
      if (key.compare(0, key.size(), k, klen) != 0) return;
      found = true;
      size_t clen;
      const char* c = r.cell(&clen);
      if (static_cast<uint8_t>(c[0]) == kCellInline) fn(c + 3, get16(c + 1));  // straight from the cached page
      else overflow_cell.assign(c, clen);
    });
    if (!leaf) id = child;
  }
  if (found && !overflow_cell.empty()) {
    std::string value;
    readCell(overflow_cell, &value);
    fn(value.data(), value.size());
  }
  return found;
}

void BTree::multiGetRaw(const std::vector<std::string>& keys, const MultiValueFn& fn) {
  PageId leaf = kInvalidPage;  // the leaf that held (or would hold) the previous key
  std::string overflow_cell;
  for (size_t ki = 0; ki < keys.size(); ki++) {
    const std::string& key = keys[ki];
    // Searches `leaf` for key. With `reuse`, a key past the leaf's last key is
    // left unresolved: it may belong to a later leaf. Keys are ascending, so
    // a key no greater than the leaf's last key is inside the leaf's range.
    auto search = [&](bool reuse) {
      bool resolved = false;
      pager_.visit(leaf, [&](const char* page) {
        size_t count = keyCount(page, leaf, true);
        size_t i = leafLowerBound(page, leaf, count, key);
        if (i == count) {
          resolved = !reuse;
          return;
        }
        resolved = true;
        Reader r{page, leaf, slotAt(page, leaf, kPageHeaderSize, i)};
        uint16_t klen = r.u16();
        const char* k = r.bytes(klen);
        if (key.compare(0, key.size(), k, klen) != 0) return;
        size_t clen;
        const char* c = r.cell(&clen);
        if (static_cast<uint8_t>(c[0]) == kCellInline) fn(ki, c + 3, get16(c + 1));
        else overflow_cell.assign(c, clen);
      });
      return resolved;
    };
    if (leaf == kInvalidPage || !search(true)) {
      PageId id = pager_.meta().root;
      while (true) {
        bool is_leaf = false;
        PageId child = kInvalidPage;
        pager_.visit(id, [&](const char* page) {
          is_leaf = isLeafPage(page, id);
          if (!is_leaf) child = rawChildFor(page, id, key);
        });
        if (is_leaf) break;
        id = child;
      }
      leaf = id;
      search(false);
    }
    if (!overflow_cell.empty()) {
      std::string value;
      readCell(overflow_cell, &value);
      fn(ki, value.data(), value.size());
      overflow_cell.clear();
    }
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

void BTree::scanRaw(const std::string& lo, const std::string* hi, const RawScanFn& fn) {
  char page[kPageSize];
  PageId id = pager_.meta().root;
  while (true) {
    pager_.read(id, page);
    if (isLeafPage(page, id)) break;
    id = rawChildFor(page, id, lo);
  }

  std::string overflow;  // assembled value for cells stored out of line
  bool first_leaf = true;
  while (true) {
    size_t count = keyCount(page, id, true);
    // In the first leaf, binary-search to the start key.
    size_t start = first_leaf ? leafLowerBound(page, id, count, lo) : 0;
    for (size_t i = start; i < count; i++) {
      Reader r{page, id, slotAt(page, id, kPageHeaderSize, i)};
      uint16_t klen = r.u16();
      const char* k = r.bytes(klen);
      size_t clen;
      const char* c = r.cell(&clen);
      if (hi && hi->compare(0, hi->size(), k, klen) < 0) return;  // past the end key
      bool more;
      if (static_cast<uint8_t>(c[0]) == kCellInline) {
        more = fn(k, klen, c + 3, get16(c + 1));
      } else {
        readCell(std::string(c, clen), &overflow);
        more = fn(k, klen, overflow.data(), overflow.size());
      }
      if (!more) return;
    }
    PageId next = get32(page + kOffNext);
    if (next == kInvalidPage) return;
    id = next;
    pager_.read(id, page);  // follow the leaf chain instead of re-descending
    first_leaf = false;
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

// The leaf for `key`, found without decoding. *fence is set to the smallest
// separator above the leaf (every key in the leaf is below it), if any.
PageId BTree::leafFor(const std::string& key, std::string* fence, bool* has_fence) {
  PageId id = pager_.meta().root;
  if (has_fence) *has_fence = false;
  bool leaf = false;
  while (!leaf) {
    PageId child = kInvalidPage;
    pager_.visit(id, [&](const char* page) {
      leaf = isLeafPage(page, id);
      if (leaf) return;
      size_t idx;
      child = rawChildFor(page, id, key, &idx);
      size_t count = keyCount(page, id, false);
      if (fence && idx < count) {  // deeper separators are tighter bounds
        uint16_t len;
        const char* k = keyAt(page, id, slotBase(false, count), idx, &len);
        fence->assign(k, len);
        *has_fence = true;
      }
    });
    if (!leaf) id = child;
  }
  return id;
}

// Fast path for puts: merges the longest prefix of items (sorted, unique,
// inline values, all belonging to leaf `id`) that fits into the leaf, by
// rebuilding the page from byte ranges of the cached copy (no Node, no
// per-key allocations). No other page changes. Returns how many items were
// merged; 0 means the general path is needed: a split, an overflow value to
// replace, an underfull leaf, or a page not laid out contiguously.
size_t BTree::mergeIntoLeaf(PageId id, const PutItem* items, size_t n, size_t* inserted) {
  char out[kPageSize];
  size_t taken = 0;
  *inserted = 0;
  pager_.visit(id, [&](const char* page) {
    size_t count = keyCount(page, id, true);
    size_t first = kPageHeaderSize + 2 * count;  // where the entries start
    size_t end = first;                          // where they end
    if (count > 0) {
      if (slotAt(page, id, kPageHeaderSize, 0) != first) return;  // not contiguous
      Reader last{page, id, slotAt(page, id, kPageHeaderSize, count - 1)};
      last.bytes(last.u16());
      size_t clen;
      last.cell(&clen);
      end = last.off;
    }
    auto entryEnd = [&](size_t i) { return i + 1 < count ? slotAt(page, id, kPageHeaderSize, i + 1) : end; };
    auto oldKey = [&](size_t i, uint16_t* len) { return keyAt(page, id, kPageHeaderSize, i, len); };

    // Pass 1: how many items fit, and which old entries they replace.
    size_t body = end - kPageHeaderSize;
    size_t oi = 0, replaced = 0;
    for (; taken < n; taken++) {
      const std::string& key = *items[taken].key;
      while (oi < count) {
        uint16_t len;
        const char* k = oldKey(oi, &len);
        if (key.compare(0, key.size(), k, len) <= 0) break;
        oi++;
      }
      bool found = false;
      size_t old_size = 0;
      if (oi < count) {
        uint16_t len;
        const char* k = oldKey(oi, &len);
        if (key.compare(0, key.size(), k, len) == 0) {
          Reader r{page, id, slotAt(page, id, kPageHeaderSize, oi) + 2 + len};
          size_t clen;
          if (static_cast<uint8_t>(r.cell(&clen)[0]) != kCellInline) break;  // its overflow chain must be freed
          found = true;
          old_size = entryEnd(oi) - slotAt(page, id, kPageHeaderSize, oi);
        }
      }
      size_t grown = body - old_size + (2 + key.size() + 3 + items[taken].value->size()) + (found ? 0 : 2);
      if (grown > kNodeCapacity) break;  // needs a split
      body = grown;
      if (found) {
        replaced++;
        oi++;
      }
    }
    if (taken == 0) return;
    if (body < kNodeMinFill && id != pager_.meta().root) {  // smaller values left it underfull: rebalance
      taken = 0;
      return;
    }

    // Pass 2: write the merged page.
    size_t new_count = count + taken - replaced;
    std::memcpy(out, page, kPageHeaderSize);
    put16(out + kOffNumKeys, static_cast<uint16_t>(new_count));
    size_t off = kPageHeaderSize + 2 * new_count, slot = 0;
    auto copyOld = [&](size_t i) {
      size_t from = slotAt(page, id, kPageHeaderSize, i), len = entryEnd(i) - from;
      put16(out + kPageHeaderSize + 2 * slot++, static_cast<uint16_t>(off));
      std::memcpy(out + off, page + from, len);
      off += len;
    };
    oi = 0;
    for (size_t t = 0; t < taken; t++) {
      const std::string& key = *items[t].key;
      const std::string& value = *items[t].value;
      bool found = false;
      while (oi < count) {
        uint16_t len;
        const char* k = oldKey(oi, &len);
        int c = key.compare(0, key.size(), k, len);
        if (c < 0) break;
        if (c == 0) {
          found = true;
          oi++;  // replaced
          break;
        }
        copyOld(oi++);
      }
      put16(out + kPageHeaderSize + 2 * slot++, static_cast<uint16_t>(off));
      put16(out + off, static_cast<uint16_t>(key.size()));
      std::memcpy(out + off + 2, key.data(), key.size());
      out[off + 2 + key.size()] = static_cast<char>(kCellInline);
      put16(out + off + 3 + key.size(), static_cast<uint16_t>(value.size()));
      if (!value.empty()) std::memcpy(out + off + 5 + key.size(), value.data(), value.size());
      off += 2 + key.size() + 3 + value.size();
      if (!found) ++*inserted;
    }
    while (oi < count) copyOld(oi++);
    std::memset(out + off, 0, kPageSize - off);
  });
  if (taken == 0) return 0;
  pager_.write(id, out);
  pager_.meta().key_count += *inserted;
  return taken;
}

void BTree::putMany(const std::vector<PutItem>& items) {
  std::string fence;
  size_t j = 0;
  while (j < items.size()) {
    if (items[j].value->size() > kMaxInlineValue) {
      put(*items[j].key, *items[j].value);
      j++;
      continue;
    }
    bool has_fence;
    PageId leaf = leafFor(*items[j].key, &fence, &has_fence);
    size_t k = j + 1;  // the run of items that belong to this leaf
    while (k < items.size() && items[k].value->size() <= kMaxInlineValue && (!has_fence || *items[k].key < fence)) k++;
    size_t inserted;
    size_t taken = mergeIntoLeaf(leaf, &items[j], k - j, &inserted);
    if (taken > 0) {
      j += taken;
    } else {
      put(*items[j].key, *items[j].value);  // the general path: split or rebalance
      j++;
    }
  }
}

bool BTree::put(const std::string& key, const std::string& value) {
  if (value.size() <= kMaxInlineValue) {
    PutItem item{&key, &value};
    size_t inserted;
    if (mergeIntoLeaf(leafFor(key, nullptr, nullptr), &item, 1, &inserted) == 1) return inserted == 1;
  }
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
  if (!modify(meta.root, m, &root, true)) return;

  if (nodeBytes(root) > kNodeCapacity) {
    // The root split: grow the tree by one level.
    Node right;
    right.id = pager_.allocate();
    std::string separator;
    splitNode(root, right, &separator, m.append);
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
bool BTree::modify(PageId id, Mutation& m, Node* out, bool rightmost) {
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
      m.append = rightmost && i + 1 == n.keys.size();  // past every key in the tree
    }
    *out = std::move(n);
    return true;
  }

  // Descend without decoding; this node only changes if its child does.
  size_t idx;
  PageId child_id = rawChildFor(page, id, *m.key, &idx);
  bool last_child = idx == get16(page + kOffNumKeys);
  Node child;
  if (!modify(child_id, m, &child, rightmost && last_child)) return false;

  Node n = decode(page, id);
  fixChild(n, idx, child, m.append);
  *out = std::move(n);
  return true;
}

// Stores parent.children[idx] after a modification, splitting or
// rebalancing it first if needed. May change `parent` in memory.
void BTree::fixChild(Node& parent, size_t idx, Node& child, bool append) {
  size_t bytes = nodeBytes(child);
  if (bytes > kNodeCapacity) {
    Node right;
    right.id = pager_.allocate();
    std::string separator;
    splitNode(child, right, &separator, append);
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
// With `append` (keys arriving in ascending order) the right page gets only
// the minimum fill instead: an even split would leave every left page half
// empty forever, because no later key will land in it.
void BTree::splitNode(Node& n, Node& right, std::string* separator, bool append) {
  right.leaf = n.leaf;
  size_t count = n.keys.size();
  std::vector<size_t> size(count);
  size_t total = 0;
  for (size_t i = 0; i < count; i++) {
    // Each entry also costs a 2-byte slot.
    size[i] = n.leaf ? 2 + 2 + n.keys[i].size() + n.cells[i].size() : 2 + 2 + n.keys[i].size() + 4;
    total += size[i];
  }

  // Choose the cut that minimizes the larger half (or, for appends, the
  // latest cut that still leaves the right page at its minimum fill).
  size_t best = 0, best_cost = SIZE_MAX, prefix = 0, append_cut = 0;
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
    if (append && left <= kNodeCapacity && rest >= kNodeMinFill) append_cut = m;  // keep the latest such cut
  }
  if (append && append_cut > 0) best = append_cut;
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
