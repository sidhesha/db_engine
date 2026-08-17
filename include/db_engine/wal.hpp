#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Which physical file a WALRecord's page_id refers to. The heap file and
// the index file each have their own page_id namespace, so a record has
// to say which one it belongs to before recovery can locate the page.
enum class WALStore : uint8_t { HEAP = 0, INDEX = 1 };

enum class WALRecordType : uint8_t { BEGIN = 0, UPDATE = 1, COMMIT = 2, ABORT = 3, CLR = 4 };

// A single ARIES-style log record. BEGIN/COMMIT/ABORT carry no page data
// (page_id == -1, old_data/new_data empty). UPDATE carries both images.
// CLR (compensation log record, written while undoing an UPDATE) carries
// old_data only -- old_data is what gets reapplied to make the undo
// itself redoable if recovery crashes mid-undo.
struct WALRecord {
    uint64_t lsn = 0;
    uint64_t prev_lsn = 0;   // this txn's previous record's LSN; 0 = none
    uint64_t txn_id = 0;
    WALRecordType type = WALRecordType::BEGIN;
    WALStore store = WALStore::HEAP;
    // Which table's heap/index file `store`+`page_id` refers to (Phase 6:
    // multiple tables now share one WAL/LSN/txn_id space, so `store` alone
    // no longer identifies a physical file). 0 for BEGIN/COMMIT/ABORT,
    // which are table-agnostic, and for every pre-Phase-6 single-table
    // caller, which never set it and doesn't need to.
    uint32_t table_id = 0;
    int32_t page_id = -1;
    std::vector<char> old_data;
    std::vector<char> new_data;
};

// Append-only WAL file. Records are length-prefixed and CRC-checked so a
// torn write at the tail (the one a real crash leaves behind) is detected
// and cleanly discarded rather than misparsed as the next record.
class WALWriter {
public:
    explicit WALWriter(const std::string& wal_filename);

    // Assigns the next LSN, appends the record, returns the assigned LSN.
    // Does not itself force durability -- see flush()/flushUpTo().
    uint64_t append(WALRecord record);

    void flush();                     // force everything written so far to disk
    void flushUpTo(uint64_t lsn);     // v1: equivalent to flush() (no group commit yet)

    // Sequential scan of the whole log from the start. Stops (without
    // error) at the first record that fails its length/CRC check --
    // that's the torn tail of an in-progress write at crash time. Flushes
    // this writer's own pending buffer first, so records appended earlier
    // in the same process are visible even without an explicit flush().
    std::vector<WALRecord> readAll() const;

    uint64_t currentLSN() const { return next_lsn - 1; }

private:
    std::string filename;
    mutable std::fstream file;
    uint64_t next_lsn;
    // Protects next_lsn and `file`: this WALWriter is shared between
    // BufferPool and IndexManager, which can be driven from different
    // threads (that's the whole point of one shared LSN/txn_id space
    // across heap and index writes), so append()/flush()/readAll() all
    // need to serialize against each other.
    mutable std::mutex mu;

    // Scans the file from the start; returns the parsed records and the
    // byte offset just past the last valid record (i.e. the start of any
    // torn tail, which the constructor truncates away so future appends
    // don't leave garbage sitting between valid records). Caller must
    // hold `mu`.
    std::pair<std::vector<WALRecord>, std::size_t> scan() const;
};

// Every top-level mutating call wraps itself as its own auto-commit
// transaction (txn_id == 0 passed down to it) purely so WAL records
// have a txn_id to be tagged with and something for recovery's undo
// pass to key off of, UNLESS the caller supplies a real, caller-owned
// txn_id (Phase 5/MVCC's multi-statement transactions), in which case
// that layer defers begin/commit/abort to the caller entirely.
class TransactionManager {
public:
    explicit TransactionManager(WALWriter& wal);

    uint64_t begin();
    void commit(uint64_t txn_id);
    // Explicit ROLLBACK. Writes an ABORT record (the WALRecordType value
    // existed since Phase 4 but had no producer until this). Does NOT
    // itself undo any page bytes -- RecoveryManager's crash-undo pass
    // already reverts anything from a txn with no COMMIT record (which
    // this now correctly is), and Phase 5's MVCC visibility rules make
    // an aborted transaction's row versions invisible to everyone
    // without needing a live undo replay (see the version-chain design).
    void abort(uint64_t txn_id);

    // Appends an UPDATE/CLR-style record for this txn, threading
    // prev_lsn automatically from the txn's last record. table_id
    // defaults to 0 -- every pre-Phase-6 caller has exactly one table
    // (implicitly table_id 0), so this stays source-compatible with
    // every existing call site.
    uint64_t appendRecord(uint64_t txn_id, WALRecordType type, WALStore store,
                           int32_t page_id, std::vector<char> old_data,
                           std::vector<char> new_data, uint32_t table_id = 0);

    // The txn_id that WOULD be assigned to the next begin() call, without
    // actually allocating one. MVCCManager (Phase 5) uses this as a
    // snapshot's xmax: any txn_id >= this value necessarily started after
    // the snapshot was taken.
    uint64_t peekNextTxnId() const;

private:
    WALWriter& wal;
    uint64_t next_txn_id;
    std::unordered_map<uint64_t, uint64_t> last_lsn_per_txn;
    // Protects next_txn_id and last_lsn_per_txn: one TransactionManager
    // is shared across BufferPool and IndexManager (see WALWriter::mu).
    mutable std::mutex mu;
};
