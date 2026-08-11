#include "recoverymanager.hpp"

#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "constants.hpp"

namespace {
// Mirrors Page's header layout (include/page.hpp, src/page.cpp): 4 bytes
// page_id + 2 bytes num_slots + 2 bytes free_space_offset, then the LSN.
constexpr std::size_t HEAP_LSN_OFFSET = 8;
// Mirrors BPlusTreeNode's header layout (include/node.hpp, src/node.cpp):
// node_id(4) + is_leaf(4, padded) + num_keys(4), then the LSN.
constexpr std::size_t INDEX_LSN_OFFSET = 12;
}  // namespace

RecoveryManager::RecoveryManager(WALWriter& wal, const std::string& heap_filename,
                                  const std::string& index_filename)
    : wal(wal), heap_filename(heap_filename), index_filename(index_filename) {}

std::size_t RecoveryManager::heapOffset(int page_id) const {
    return static_cast<std::size_t>(page_id) * PAGE_SIZE;
}

std::size_t RecoveryManager::indexOffset(int node_id) const {
    // Must match IndexManager::nodeOffset(): a PAGE_SIZE header slot
    // (root_node_id) precedes the node slots.
    return PAGE_SIZE + static_cast<std::size_t>(node_id) * PAGE_SIZE;
}

std::size_t RecoveryManager::lsnFieldOffset(WALStore store) const {
    return store == WALStore::HEAP ? HEAP_LSN_OFFSET : INDEX_LSN_OFFSET;
}

void RecoveryManager::ensureOpen(std::fstream& file, const std::string& filename) const {
    file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        file.clear();
        file.open(filename, std::ios::out | std::ios::binary);
        file.close();
        file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    }
    if (!file.is_open()) {
        throw std::runtime_error("RecoveryManager: failed to open " + filename);
    }
}

uint64_t RecoveryManager::readStoredLSN(std::fstream& file, std::size_t page_offset,
                                         std::size_t lsn_offset) const {
    file.clear();
    file.seekg(0, std::ios::end);
    std::size_t file_size = static_cast<std::size_t>(file.tellg());
    if (file_size < page_offset + lsn_offset + sizeof(uint64_t)) {
        // Page doesn't exist on disk yet -- treat as "older than any
        // real record", so redo always applies.
        return 0;
    }

    uint64_t lsn = 0;
    file.seekg(page_offset + lsn_offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(&lsn), sizeof(lsn));
    if (!file) {
        throw std::runtime_error("RecoveryManager: failed to read stored LSN");
    }
    return lsn;
}

void RecoveryManager::writePageBytes(std::fstream& file, std::size_t page_offset,
                                      const std::vector<char>& data) const {
    file.clear();
    file.seekp(0, std::ios::end);
    std::size_t current_size = static_cast<std::size_t>(file.tellp());
    std::size_t needed = page_offset + data.size();
    if (current_size < needed) {
        std::vector<char> zeros(needed - current_size, 0);
        file.write(zeros.data(), zeros.size());
    }

    file.clear();
    file.seekp(page_offset, std::ios::beg);
    file.write(data.data(), data.size());
    if (!file) {
        throw std::runtime_error("RecoveryManager: failed to write recovered page");
    }
    file.flush();
}

void RecoveryManager::run() {
    auto records = wal.readAll();
    if (records.empty()) return;

    std::fstream heap_file, index_file;
    ensureOpen(heap_file, heap_filename);
    ensureOpen(index_file, index_filename);

    auto fileFor = [&](WALStore store) -> std::fstream& {
        return store == WALStore::HEAP ? heap_file : index_file;
    };
    auto offsetFor = [&](WALStore store, int page_id) -> std::size_t {
        return store == WALStore::HEAP ? heapOffset(page_id) : indexOffset(page_id);
    };

    // --- Redo: replay everything whose LSN is newer than what's on disk ---
    for (auto& r : records) {
        if (r.type != WALRecordType::UPDATE && r.type != WALRecordType::CLR) continue;

        std::fstream& file = fileFor(r.store);
        std::size_t offset = offsetFor(r.store, r.page_id);
        std::size_t lsn_offset = lsnFieldOffset(r.store);

        uint64_t stored_lsn = readStoredLSN(file, offset, lsn_offset);
        if (stored_lsn < r.lsn) {
            const std::vector<char>& data = (r.type == WALRecordType::UPDATE) ? r.new_data : r.old_data;
            writePageBytes(file, offset, data);
        }
    }

    // --- Identify losers: txns with no COMMIT record ---
    std::unordered_set<uint64_t> committed;
    std::unordered_map<uint64_t, std::vector<const WALRecord*>> updates_by_txn;
    for (auto& r : records) {
        if (r.type == WALRecordType::COMMIT) {
            committed.insert(r.txn_id);
        } else if (r.type == WALRecordType::UPDATE) {
            updates_by_txn[r.txn_id].push_back(&r);
        }
    }

    // --- Undo: revert each loser's UPDATEs in reverse order ---
    for (auto& [txn_id, updates] : updates_by_txn) {
        if (committed.count(txn_id)) continue;

        for (auto it = updates.rbegin(); it != updates.rend(); ++it) {
            const WALRecord* r = *it;

            WALRecord clr;
            clr.txn_id = txn_id;
            clr.type = WALRecordType::CLR;
            clr.store = r->store;
            clr.page_id = r->page_id;
            clr.old_data = r->old_data;
            clr.prev_lsn = r->prev_lsn;
            uint64_t clr_lsn = wal.append(clr);

            // Patch the reapplied image's own LSN field to the CLR's LSN
            // (not whatever stale LSN old_data's bytes happened to carry
            // from when it was originally captured as a before-image) --
            // otherwise a page's on-disk LSN could regress, which the
            // idempotent-redo comparison above relies on never happening.
            std::vector<char> patched = r->old_data;
            std::size_t lsn_offset = lsnFieldOffset(r->store);
            if (patched.size() >= lsn_offset + sizeof(uint64_t)) {
                std::memcpy(patched.data() + lsn_offset, &clr_lsn, sizeof(clr_lsn));
            }

            writePageBytes(fileFor(r->store), offsetFor(r->store, r->page_id), patched);
        }
    }

    wal.flush();
    heap_file.flush();
    index_file.flush();
}
