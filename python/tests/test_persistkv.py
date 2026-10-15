"""Tests for the Python bindings. Run after building:

    pip install . && python -m pytest python/tests -q
"""
import os
import threading

import pytest

import persistkv


@pytest.fixture
def path(tmp_path):
    return str(tmp_path / "db")


def test_put_get_remove(path):
    with persistkv.DB(path) as db:
        db.put(b"user:1", b"alice")
        assert db.get(b"user:1") == b"alice"
        assert db.get(b"user:2") is None
        assert b"user:1" in db and b"user:2" not in db
        assert db.remove(b"user:1") is True
        assert db.remove(b"user:1") is False
        assert len(db) == 0


def test_str_is_accepted_and_bytes_returned(path):
    with persistkv.DB(path) as db:
        db.put("naam", "Altamash ✓")
        assert db.get("naam") == "Altamash ✓".encode()


def test_binary_values_and_overflow_pages(path):
    blob = os.urandom(300_000)  # > 512 B, so it lives in an overflow page chain
    with persistkv.DB(path) as db:
        db.put(b"img", blob)
        db.put(b"zero\x00byte", b"a\x00b")
        assert db.get(b"img") == blob
        assert db.get(b"zero\x00byte") == b"a\x00b"


def test_data_survives_reopen(path):
    with persistkv.DB(path) as db:
        for i in range(1000):
            db.put(f"k{i:04d}", f"v{i}")
    with persistkv.DB(path) as db:
        assert len(db) == 1000
        assert db.get(b"k0999") == b"v999"
        assert db.check_integrity() == ""


def test_scan_is_ordered_and_bounded(path):
    with persistkv.DB(path) as db:
        for k in [b"b", b"a", b"d", b"c"]:
            db.put(k, k.upper())
        assert db.scan() == [(b"a", b"A"), (b"b", b"B"), (b"c", b"C"), (b"d", b"D")]
        assert [k for k, _ in db.scan(b"b", b"c")] == [b"b", b"c"]
        assert len(db.scan(limit=3)) == 3


def test_scan_prefix(path):
    with persistkv.DB(path) as db:
        db.put(b"story:rahul:2026-10-09", b"got a dog")
        db.put(b"story:rahul:2026-10-01", b"failed physics")
        db.put(b"story:rahulx:2026-10-02", b"other person")
        db.put(b"story:zoya:2026-10-03", b"moved to Pune")
        rows = db.scan_prefix(b"story:rahul:")
        assert [v for _, v in rows] == [b"failed physics", b"got a dog"]


def test_write_batch_is_atomic(path):
    with persistkv.DB(path) as db:
        db.put(b"gone", b"1")
        b = persistkv.WriteBatch()
        b.put(b"acct:a", b"90")
        b.put(b"acct:b", b"110")
        b.remove(b"gone")
        assert len(b) == 3
        db.write(b)
        assert db.get(b"acct:a") == b"90" and db.get(b"gone") is None


def test_bad_batch_changes_nothing(path):
    with persistkv.DB(path) as db:
        b = persistkv.WriteBatch()
        b.put(b"ok", b"1")
        b.put(b"x" * (persistkv.MAX_KEY_SIZE + 1), b"too long")
        with pytest.raises(persistkv.Error):
            db.write(b)
        assert db.get(b"ok") is None


def test_errors_become_python_exceptions(path):
    db = persistkv.DB(path)
    with pytest.raises(persistkv.Error, match="key"):
        db.put(b"", b"empty key")
    with pytest.raises(persistkv.Error):
        db.put(b"k" * (persistkv.MAX_KEY_SIZE + 1), b"v")
    db.close()
    assert db.closed
    with pytest.raises(persistkv.Error, match="closed"):
        db.get(b"k")
    assert issubclass(persistkv.CorruptionError, persistkv.Error)


def test_threads_share_one_db(path):
    """GIL released inside the engine: concurrent writers get grouped into fewer fsyncs."""
    with persistkv.DB(path) as db:
        def writer(t):
            for i in range(200):
                db.put(f"t{t}:{i:03d}", "x")
        threads = [threading.Thread(target=writer, args=(t,)) for t in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert len(db) == 1600
        s = db.stats()
        assert s["commit_batches"] == 1600
        assert s["commit_groups"] <= s["commit_batches"]


def test_destroy(path):
    with persistkv.DB(path) as db:
        db.put(b"k", b"v")
    persistkv.destroy(path)
    with persistkv.DB(path) as db:
        assert len(db) == 0
