// Python bindings for PersistKV (pybind11).
//
//   import persistkv
//   with persistkv.DB("mydb") as db:
//       db.put(b"user:1", b"alice")
//       db.get(b"user:1")                     # b"alice", or None if missing
//       db.scan(b"user:", b"user:~")          # [(key, value), ...] in key order
//
// Keys and values are bytes (str is accepted and encoded as UTF-8); get/scan return bytes.
//
// The GIL is released around every call into the engine. pkv::DB is thread-safe, so
// Python threads really run in parallel inside it: readers share the tree lock, and
// concurrent writers are grouped into one WAL fsync (group commit).
//
// scan() collects results in C++ with the GIL released and converts them afterwards,
// so no Python code ever runs while the tree lock is held (calling back into the DB
// from a scan callback would deadlock).

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "persistkv/db.h"
#include "persistkv/write_batch.h"

namespace py = pybind11;

namespace {

class PyDB {
 public:
  PyDB(const std::string& path, bool sync_writes, size_t cache_pages, bool background_checkpoints) {
    pkv::Options o;
    o.sync_writes = sync_writes;
    o.cache_pages = cache_pages;
    o.background_checkpoints = background_checkpoints;
    py::gil_scoped_release nogil;  // opening may replay the WAL
    db_.reset(new pkv::DB(path, o));
  }

  void put(const std::string& key, const std::string& value) {
    py::gil_scoped_release nogil;
    db().put(key, value);
  }

  py::object get(const std::string& key) {
    std::string value;
    bool found;
    {
      py::gil_scoped_release nogil;
      found = db().get(key, &value);
    }
    if (!found) return py::none();
    return py::bytes(value);
  }

  bool remove(const std::string& key) {
    py::gil_scoped_release nogil;
    return db().remove(key);
  }

  bool contains(const std::string& key) {
    std::string unused;
    py::gil_scoped_release nogil;
    return db().get(key, &unused);
  }

  void write(const pkv::WriteBatch& batch) {
    py::gil_scoped_release nogil;
    db().write(batch);
  }

  // Keys in [lo, hi] in order; empty hi = no upper bound; limit 0 = no limit.
  py::list scan(const std::string& lo, const std::string& hi, size_t limit) {
    std::vector<std::pair<std::string, std::string>> rows;
    {
      py::gil_scoped_release nogil;
      db().scan(lo, hi, [&](const std::string& k, const std::string& v) {
        rows.emplace_back(k, v);
        return limit == 0 || rows.size() < limit;
      });
    }
    py::list out;
    for (const auto& r : rows) out.append(py::make_tuple(py::bytes(r.first), py::bytes(r.second)));
    return out;
  }

  // Every key starting with `prefix`, in order. Keys sharing a prefix are adjacent in a
  // B+Tree, so the scan starts at the prefix and stops at the first key without it.
  py::list scan_prefix(const std::string& prefix, size_t limit) {
    std::vector<std::pair<std::string, std::string>> rows;
    {
      py::gil_scoped_release nogil;
      db().scan(prefix, "", [&](const std::string& k, const std::string& v) {
        if (k.compare(0, prefix.size(), prefix) != 0) return false;
        rows.emplace_back(k, v);
        return limit == 0 || rows.size() < limit;
      });
    }
    py::list out;
    for (const auto& r : rows) out.append(py::make_tuple(py::bytes(r.first), py::bytes(r.second)));
    return out;
  }

  uint64_t size() {
    py::gil_scoped_release nogil;
    return db().size();
  }

  void checkpoint() {
    py::gil_scoped_release nogil;
    db().checkpoint();
  }

  std::string check_integrity() {
    py::gil_scoped_release nogil;
    return db().checkIntegrity();
  }

  py::dict stats() {
    pkv::DBStats s;
    {
      py::gil_scoped_release nogil;
      s = db().stats();
    }
    py::dict d;
    d["keys"] = s.keys;
    d["height"] = s.height;
    d["pages"] = s.pages;
    d["dirty_pages"] = s.dirty_pages;
    d["cached_pages"] = s.cached_pages;
    d["wal_bytes"] = s.wal_bytes;
    d["last_lsn"] = s.last_lsn;
    d["checkpoints"] = s.checkpoints;
    d["cache_hits"] = s.cache_hits;
    d["cache_misses"] = s.cache_misses;
    d["commit_groups"] = s.commit_groups;
    d["commit_batches"] = s.commit_batches;
    d["wal_records_replayed"] = s.wal_records_replayed;
    d["wal_bytes_truncated"] = s.wal_bytes_truncated;
    d["recovered_from_doublewrite"] = s.recovered_from_doublewrite;
    return d;
  }

  // Stops the checkpoint thread and checkpoints, so the next open needs no WAL replay.
  void close() {
    std::unique_ptr<pkv::DB> db = std::move(db_);
    py::gil_scoped_release nogil;
    db.reset();
  }

  bool closed() const { return !db_; }

 private:
  pkv::DB& db() {
    if (!db_) throw pkv::Error("database is closed");
    return *db_;
  }

  std::unique_ptr<pkv::DB> db_;
};

}  // namespace

PYBIND11_MODULE(persistkv, m) {
  m.doc() = "PersistKV: crash-safe, thread-safe key-value storage engine (B+Tree + WAL)";
  m.attr("MAX_KEY_SIZE") = pkv::kMaxKeySize;
  m.attr("MAX_VALUE_SIZE") = pkv::kMaxValueSize;

  // Translators run newest first, so the subclass is registered after its base.
  auto error = py::register_exception<pkv::Error>(m, "Error");
  py::register_exception<pkv::CorruptionError>(m, "CorruptionError", error.ptr());

  py::class_<pkv::WriteBatch>(m, "WriteBatch", "Puts and deletes committed atomically, with one fsync.")
      .def(py::init<>())
      .def("put", [](pkv::WriteBatch& b, const std::string& k, const std::string& v) { b.put(k, v); },
           py::arg("key"), py::arg("value"))
      .def("remove", [](pkv::WriteBatch& b, const std::string& k) { b.remove(k); }, py::arg("key"))
      .def("clear", &pkv::WriteBatch::clear)
      .def("__len__", &pkv::WriteBatch::count);

  py::class_<PyDB>(m, "DB")
      .def(py::init<const std::string&, bool, size_t, bool>(), py::arg("path"),
           py::arg("sync_writes") = true, py::arg("cache_pages") = 4096,
           py::arg("background_checkpoints") = true)
      .def("put", &PyDB::put, py::arg("key"), py::arg("value"))
      .def("get", &PyDB::get, py::arg("key"), "Value as bytes, or None if the key is missing.")
      .def("remove", &PyDB::remove, py::arg("key"), "Whether the key existed.")
      .def("write", &PyDB::write, py::arg("batch"))
      .def("scan", &PyDB::scan, py::arg("lo") = "", py::arg("hi") = "", py::arg("limit") = 0)
      .def("scan_prefix", &PyDB::scan_prefix, py::arg("prefix"), py::arg("limit") = 0)
      .def("checkpoint", &PyDB::checkpoint)
      .def("check_integrity", &PyDB::check_integrity, "Empty string if the tree is consistent.")
      .def("stats", &PyDB::stats)
      .def("close", &PyDB::close)
      .def_property_readonly("closed", &PyDB::closed)
      .def("__len__", &PyDB::size)
      .def("__contains__", &PyDB::contains)
      .def("__enter__", [](PyDB& self) -> PyDB& { return self; }, py::return_value_policy::reference)
      .def("__exit__", [](PyDB& self, py::args) { self.close(); });

  m.def("destroy", [](const std::string& dir) { pkv::DB::destroy(dir); }, py::arg("path"),
        "Delete the database files in `path` (the directory itself is kept).");
}
