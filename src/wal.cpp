#include "wal.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {

// Standard CRC-32 (IEEE 802.3), table-based. Used to detect a torn
// (partially-written) record at the tail of the WAL file after a crash.
uint32_t crc32(const std::vector<char>& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Sanity cap on a single record's serialized body size, so a corrupt
// length prefix can't make readAll() try to allocate/read a huge buffer.
// Two full page images plus header fields is the largest a real UPDATE
// record gets; leave generous headroom.
constexpr std::size_t MAX_RECORD_BODY_SIZE = 64 * 1024;

void appendUint64(std::vector<char>& buf, uint64_t v) {
    std::size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendUint32(std::vector<char>& buf, uint32_t v) {
    std::size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendInt32(std::vector<char>& buf, int32_t v) {
    std::size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendUint8(std::vector<char>& buf, uint8_t v) {
    buf.push_back(static_cast<char>(v));
}

void appendBytes(std::vector<char>& buf, const std::vector<char>& bytes) {
    appendUint32(buf, static_cast<uint32_t>(bytes.size()));
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

std::vector<char> serializeRecord(const WALRecord& r) {
    std::vector<char> buf;
    appendUint64(buf, r.lsn);
    appendUint64(buf, r.prev_lsn);
    appendUint64(buf, r.txn_id);
    appendUint8(buf, static_cast<uint8_t>(r.type));
    appendUint8(buf, static_cast<uint8_t>(r.store));
    appendInt32(buf, r.page_id);
    appendBytes(buf, r.old_data);
    appendBytes(buf, r.new_data);
    return buf;
}

// Reads a record body out of `buf`; returns false if the body is
// malformed (truncated / inconsistent length fields) rather than
// throwing, since malformed bodies are an expected outcome of scanning
// past a torn tail write.
bool deserializeRecord(const std::vector<char>& buf, WALRecord& out) {
    std::size_t offset = 0;
    auto remaining = [&]() { return buf.size() - offset; };

    auto readUint64 = [&](uint64_t& v) -> bool {
        if (remaining() < sizeof(v)) return false;
        std::memcpy(&v, buf.data() + offset, sizeof(v));
        offset += sizeof(v);
        return true;
    };
    auto readUint8 = [&](uint8_t& v) -> bool {
        if (remaining() < sizeof(v)) return false;
        v = static_cast<uint8_t>(buf[offset]);
        offset += sizeof(v);
        return true;
    };
    auto readInt32 = [&](int32_t& v) -> bool {
        if (remaining() < sizeof(v)) return false;
        std::memcpy(&v, buf.data() + offset, sizeof(v));
        offset += sizeof(v);
        return true;
    };
    auto readBytes = [&](std::vector<char>& v) -> bool {
        uint32_t len;
        if (remaining() < sizeof(len)) return false;
        std::memcpy(&len, buf.data() + offset, sizeof(len));
        offset += sizeof(len);
        if (remaining() < len) return false;
        v.assign(buf.begin() + offset, buf.begin() + offset + len);
        offset += len;
        return true;
    };

    uint8_t type_tag, store_tag;
    if (!readUint64(out.lsn)) return false;
    if (!readUint64(out.prev_lsn)) return false;
    if (!readUint64(out.txn_id)) return false;
    if (!readUint8(type_tag)) return false;
    if (!readUint8(store_tag)) return false;
    if (!readInt32(out.page_id)) return false;
    if (!readBytes(out.old_data)) return false;
    if (!readBytes(out.new_data)) return false;

    if (type_tag > static_cast<uint8_t>(WALRecordType::CLR)) return false;
    if (store_tag > static_cast<uint8_t>(WALStore::INDEX)) return false;
    out.type = static_cast<WALRecordType>(type_tag);
    out.store = static_cast<WALStore>(store_tag);
    return true;
}

}  // namespace

WALWriter::WALWriter(const std::string& wal_filename) : filename(wal_filename), next_lsn(1) {
    // Create the file if it doesn't exist yet, same pattern as
    // BufferPool/IndexManager's openFile().
    file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        file.clear();
        file.open(filename, std::ios::out | std::ios::binary);
        file.close();
        file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    }
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open WAL file: " + filename);
    }

    auto [records, valid_end_offset] = scan();
    if (!records.empty()) {
        next_lsn = records.back().lsn + 1;
    }

    // Drop any torn tail left by a crash mid-append, so future appends
    // stay contiguous with the last valid record instead of leaving
    // unreachable garbage in between.
    file.close();
    std::error_code ec;
    std::filesystem::resize_file(filename, valid_end_offset, ec);
    if (ec) {
        throw std::runtime_error("Failed to truncate WAL file: " + filename);
    }
    file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to reopen WAL file: " + filename);
    }
}

std::pair<std::vector<WALRecord>, std::size_t> WALWriter::scan() const {
    std::vector<WALRecord> records;

    std::fstream in(filename, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return {records, 0};
    }

    std::size_t valid_end_offset = 0;
    while (true) {
        std::streampos record_start = in.tellg();

        uint32_t body_len = 0;
        in.read(reinterpret_cast<char*>(&body_len), sizeof(body_len));
        if (!in || static_cast<std::size_t>(in.gcount()) < sizeof(body_len)) {
            break;  // clean EOF or torn length prefix -- stop here
        }
        if (body_len > MAX_RECORD_BODY_SIZE) {
            break;  // corrupt length prefix
        }

        std::vector<char> body(body_len);
        if (body_len > 0) {
            in.read(body.data(), body_len);
            if (!in || static_cast<std::size_t>(in.gcount()) < body_len) {
                break;  // torn body
            }
        }

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char*>(&stored_crc), sizeof(stored_crc));
        if (!in || static_cast<std::size_t>(in.gcount()) < sizeof(stored_crc)) {
            break;  // torn checksum
        }

        if (crc32(body) != stored_crc) {
            break;  // checksum mismatch -- torn or corrupt record
        }

        WALRecord record;
        if (!deserializeRecord(body, record)) {
            break;  // internally inconsistent body (shouldn't happen if CRC matched, but be safe)
        }

        records.push_back(std::move(record));
        valid_end_offset = static_cast<std::size_t>(in.tellg());
        (void)record_start;
    }

    return {records, valid_end_offset};
}

uint64_t WALWriter::append(WALRecord record) {
    std::lock_guard<std::mutex> lock(mu);

    record.lsn = next_lsn++;
    std::vector<char> body = serializeRecord(record);
    uint32_t body_len = static_cast<uint32_t>(body.size());
    uint32_t crc = crc32(body);

    file.clear();
    file.seekp(0, std::ios::end);
    file.write(reinterpret_cast<const char*>(&body_len), sizeof(body_len));
    file.write(body.data(), body.size());
    file.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    if (!file) {
        throw std::runtime_error("Failed to append WAL record to " + filename);
    }

    return record.lsn;
}

void WALWriter::flush() {
    std::lock_guard<std::mutex> lock(mu);
    file.flush();
}

void WALWriter::flushUpTo(uint64_t /*lsn*/) {
    // v1: no group commit / batching, every append is already positioned
    // at the end of the file in write order, so making everything
    // durable up to any LSN reduces to flushing the whole buffer.
    flush();
}

std::vector<WALRecord> WALWriter::readAll() const {
    std::lock_guard<std::mutex> lock(mu);
    // scan() opens its own independent handle onto the file; without a
    // flush here, any append() still sitting in `file`'s userspace
    // buffer (not yet handed to the OS) would be invisible to it. Held
    // under `mu` together with scan() itself so a concurrent append()
    // can't land in between and be read back as a torn/partial record.
    file.flush();
    return scan().first;
}

TransactionManager::TransactionManager(WALWriter& wal) : wal(wal), next_txn_id(1) {
    // txn_ids must stay unique for the WAL file's entire lifetime, not
    // just this process's: recovery has no checkpointing (a deliberate
    // simplification, see RecoveryManager) and re-scans the whole log
    // from byte 0 on every run, grouping records by txn_id. If a fresh
    // TransactionManager always restarted counting at 1, a txn_id from
    // before a restart could collide with an unrelated txn_id assigned
    // after it, and a later recovery pass would wrongly merge their
    // records into one undo/redo unit. Scanning for the highest txn_id
    // already in the log avoids that.
    for (const auto& record : wal.readAll()) {
        if (record.txn_id >= next_txn_id) {
            next_txn_id = record.txn_id + 1;
        }
    }
}

uint64_t TransactionManager::begin() {
    // Held for the whole call, including the nested wal.append(): that's
    // a different mutex (WALWriter::mu), always acquired in this same
    // order (TransactionManager::mu, then WALWriter::mu) and never the
    // reverse anywhere in the codebase, so there's no lock-order cycle.
    // Holding it throughout (rather than dropping it around wal.append())
    // also keeps "read prev_lsn, append, record new prev_lsn" atomic per
    // txn_id -- relevant if a future phase ever lets one txn_id be
    // touched from more than one thread.
    std::lock_guard<std::mutex> lock(mu);
    uint64_t txn_id = next_txn_id++;

    WALRecord record;
    record.txn_id = txn_id;
    record.type = WALRecordType::BEGIN;
    record.prev_lsn = 0;
    uint64_t lsn = wal.append(record);

    last_lsn_per_txn[txn_id] = lsn;
    return txn_id;
}

void TransactionManager::commit(uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(mu);
    uint64_t prev_lsn = last_lsn_per_txn.count(txn_id) ? last_lsn_per_txn[txn_id] : 0;

    WALRecord record;
    record.txn_id = txn_id;
    record.type = WALRecordType::COMMIT;
    record.prev_lsn = prev_lsn;
    wal.append(record);
    wal.flush();

    last_lsn_per_txn.erase(txn_id);
}

uint64_t TransactionManager::appendRecord(uint64_t txn_id, WALRecordType type, WALStore store,
                                           int32_t page_id, std::vector<char> old_data,
                                           std::vector<char> new_data) {
    std::lock_guard<std::mutex> lock(mu);
    uint64_t prev_lsn = last_lsn_per_txn.count(txn_id) ? last_lsn_per_txn[txn_id] : 0;

    WALRecord record;
    record.txn_id = txn_id;
    record.type = type;
    record.store = store;
    record.page_id = page_id;
    record.old_data = std::move(old_data);
    record.new_data = std::move(new_data);
    record.prev_lsn = prev_lsn;

    uint64_t lsn = wal.append(record);

    last_lsn_per_txn[txn_id] = lsn;
    return lsn;
}
