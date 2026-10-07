// Interactive shell for a PersistKV database.

#include <iostream>
#include <sstream>
#include <string>

#include "persistkv/db.h"

using namespace pkv;

namespace {

void printHelp() {
  std::cout << "commands:\n"
               "  put <key> <value...>     insert or overwrite\n"
               "  get <key>\n"
               "  del <key>\n"
               "  scan [lo] [hi] [limit]   ordered range scan (default limit 50)\n"
               "  load <n>                 insert n generated keys\n"
               "  count | stats | check | checkpoint | help | exit\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "kvdata";
  try {
    DB db(dir);
    DBStats st = db.stats();
    std::cout << "opened '" << dir << "' (" << st.keys << " keys";
    if (st.wal_records_replayed) std::cout << ", replayed " << st.wal_records_replayed << " WAL records";
    if (st.wal_bytes_truncated) std::cout << ", dropped " << st.wal_bytes_truncated << " torn WAL bytes";
    if (st.recovered_from_doublewrite) std::cout << ", repaired from doublewrite buffer";
    std::cout << ")\ntype 'help' for commands\n";

    std::string line;
    while (std::cout << "pkv> " << std::flush, std::getline(std::cin, line)) {
      std::istringstream in(line);
      std::string cmd;
      if (!(in >> cmd)) continue;
      try {
        if (cmd == "put") {
          std::string k, v;
          in >> k;
          std::getline(in >> std::ws, v);
          db.put(k, v);
          std::cout << "OK\n";
        } else if (cmd == "get") {
          std::string k, v;
          in >> k;
          std::cout << (db.get(k, &v) ? v : "(not found)") << "\n";
        } else if (cmd == "del") {
          std::string k;
          in >> k;
          std::cout << (db.remove(k) ? "deleted" : "(not found)") << "\n";
        } else if (cmd == "scan") {
          std::string lo, hi;
          int limit = 50;
          in >> lo >> hi >> limit;
          int n = 0;
          db.scan(lo, hi, [&](const std::string& k, const std::string& v) {
            std::cout << "  " << k << " = " << v << "\n";
            return ++n < limit;
          });
          std::cout << n << " row(s)\n";
        } else if (cmd == "load") {
          int n = 0;
          in >> n;
          for (int i = 0; i < n; i++) db.put("key" + std::to_string(i), "value" + std::to_string(i));
          std::cout << "loaded " << n << " keys\n";
        } else if (cmd == "count") {
          std::cout << db.size() << "\n";
        } else if (cmd == "stats") {
          DBStats s = db.stats();
          std::cout << "keys=" << s.keys << " height=" << s.height << " pages=" << s.pages
                    << " dirty=" << s.dirty_pages << " cached=" << s.cached_pages << " wal_bytes=" << s.wal_bytes
                    << " last_lsn=" << s.last_lsn << " checkpoints=" << s.checkpoints
                    << " cache_hits=" << s.cache_hits << " cache_misses=" << s.cache_misses << "\n";
        } else if (cmd == "check") {
          std::string err = db.checkIntegrity();
          std::cout << (err.empty() ? "OK: all B+Tree invariants hold" : "CORRUPT: " + err) << "\n";
        } else if (cmd == "checkpoint") {
          db.checkpoint();
          std::cout << "OK\n";
        } else if (cmd == "help") {
          printHelp();
        } else if (cmd == "exit" || cmd == "quit") {
          break;
        } else {
          std::cout << "unknown command; type 'help'\n";
        }
      } catch (const Error& e) {
        std::cout << "error: " << e.what() << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
