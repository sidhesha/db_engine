#include <iostream>
#include <cassert>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include "key.hpp"
#include "schema.hpp"
#include "record.hpp"
#include "page.hpp"
#include "node.hpp"
#include "bplustree.hpp"
#include "indexmanager.hpp"
#include "catalogmanager.hpp"
#include "pagemanager.hpp"
#include "bufferpool.hpp"
#include "recordmanager.hpp"
#include "table.hpp"
#include "wal.hpp"
#include "recoverymanager.hpp"

static int passed = 0;
static int failed = 0;

#define TEST(name) \
    do { \
        std::cout << "  " << name << "... "; \
        try {

#define END_TEST \
            std::cout << "PASS\n"; passed++; \
        } catch (const std::exception& e) { \
            std::cout << "FAIL (" << e.what() << ")\n"; failed++; \
        } catch (...) { \
            std::cout << "FAIL (unknown)\n"; failed++; \
        } \
    } while(0)

// ─── Key ────────────────────────────────────────────────────────────────────

static void test_key() {
    TEST("int")         { Key k(42);    assert(k.getType() == KeyTypeTag::INTEGER); assert(k.toString() == "42"); } END_TEST;
    TEST("float")       { Key k(3.14f); assert(k.getType() == KeyTypeTag::FLOAT); } END_TEST;
    TEST("string")      { Key k("hi");  assert(k.getType() == KeyTypeTag::STRING); assert(k.toString() == "hi"); } END_TEST;
    TEST("int <")       { assert(Key(1) <  Key(2)); } END_TEST;
    TEST("int >")       { assert(Key(5) >  Key(3)); } END_TEST;
    TEST("int ==")      { assert(Key(4) == Key(4)); } END_TEST;
    TEST("int !=")      { assert(Key(1) != Key(2)); } END_TEST;
    TEST("int <=")      { assert(Key(1) <= Key(2)); assert(Key(2) <= Key(2)); } END_TEST;
    TEST("int >=")      { assert(Key(3) >= Key(1)); assert(Key(2) >= Key(2)); } END_TEST;
    TEST("string cmp")  { assert(Key("aa") < Key("bb")); } END_TEST;
    TEST("float cmp")   { assert(Key(1.5f) < Key(2.5f)); } END_TEST;
    TEST("type mismatch") {
        bool ok = false;
        try { Key(1) < Key("a"); } catch (const std::runtime_error&) { ok = true; }
        assert(ok);
    } END_TEST;
    TEST("empty string key") { Key k(""); assert(k.toString() == ""); } END_TEST;
    TEST("int min/max") { assert(Key(-2147483647) < Key(2147483647)); } END_TEST;
}

// ─── Schema ──────────────────────────────────────────────────────────────────

static void test_schema() {
    TEST("create with columns") {
        Schema s(std::vector<Column>{{"id", "int"}, {"name", "string"}});
        assert(s.getColumns().size() == 2);
        assert(s.getColumns()[0].name == "id");
    } END_TEST;
    TEST("empty schema") { Schema s; assert(s.getColumns().empty()); } END_TEST;
    TEST("serialize round-trip") {
        Schema a(std::vector<Column>{{"a", "int"}, {"b", "string"}});
        Schema b = Schema::deserialize(a.serialize());
        assert(b.getColumns().size() == 2);
    } END_TEST;
    TEST("single column") {
        Schema a(std::vector<Column>{{"pk", "int"}});
        Schema b = Schema::deserialize(a.serialize());
        assert(b.getColumns().size() == 1);
    } END_TEST;
    TEST("empty column name") {
        Schema s(std::vector<Column>{{"", "int"}});
        assert(s.getColumns()[0].name == "");
    } END_TEST;
}

// ─── Record ──────────────────────────────────────────────────────────────────

static void test_record() {
    TEST("create with fields") {
        Record r(std::vector<std::string>{"a", "b"});
        assert(r.getFields().size() == 2);
    } END_TEST;
    TEST("empty record") {
        Record r(std::vector<std::string>{});
        assert(r.getFields().empty());
    } END_TEST;
    TEST("serialize round-trip") {
        Record a(std::vector<std::string>{"hello", "world"});
        Record b = Record::deserialize(a.serialize());
        assert(b.getFields()[1] == "world");
    } END_TEST;
    TEST("empty fields round-trip") {
        Record a(std::vector<std::string>{});
        Record b = Record::deserialize(a.serialize());
        assert(b.getFields().empty());
    } END_TEST;
    TEST("empty string field") {
        Record r(std::vector<std::string>{"a", "", "c"});
        auto data = r.serialize();
        Record restored = Record::deserialize(data);
        assert(restored.getFields().size() == 3);
        assert(restored.getFields()[1] == "");
    } END_TEST;
    TEST("large field") {
        std::string big(1000, 'x');
        Record r(std::vector<std::string>{big});
        Record restored = Record::deserialize(r.serialize());
        assert(restored.getFields()[0].size() == 1000);
    } END_TEST;
}

// ─── Page ────────────────────────────────────────────────────────────────────

static void test_page() {
    TEST("create with id") {
        Page p(5); assert(p.getPageId() == 5u); assert(p.getNumSlots() == 0);
    } END_TEST;
    TEST("insert and read") {
        Page p(0);
        int slot = p.insertRecord({'a', 'b', 'c'});
        assert(slot == 0);
        auto r = p.readRecord(slot);
        assert(r.size() == 3);
    } END_TEST;
    TEST("multiple records") {
        Page p(0);
        assert(p.insertRecord({'x'}) == 0);
        assert(p.insertRecord({'y', 'y'}) == 1);
    } END_TEST;
    TEST("delete record") {
        Page p(0);
        int slot = p.insertRecord({'d'});
        assert(p.deleteRecord(slot));
        bool caught = false;
        try { p.readRecord(slot); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    TEST("double delete throws") {
        Page p(0);
        int slot = p.insertRecord({'x'});
        p.deleteRecord(slot);
        bool caught = false;
        try { p.deleteRecord(slot); } catch (const std::logic_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    TEST("invalid slot throws") {
        Page p(0);
        bool caught = false;
        try { p.readRecord(99); } catch (const std::out_of_range&) { caught = true; }
        assert(caught);
    } END_TEST;
    TEST("serialize round-trip") {
        Page a(7);
        a.insertRecord({'a', 'b'});
        a.insertRecord({'c', 'd', 'e'});
        Page b = Page::deserialize(a.serialize());
        assert(b.getPageId() == 7u);
        assert(b.getNumSlots() == 2);
    } END_TEST;
    TEST("full page returns -1") {
        Page p(0);
        assert(p.insertRecord(std::vector<char>(PAGE_SIZE - 100, 'x')) != -1);
        assert(p.insertRecord(std::vector<char>(PAGE_SIZE, 'x')) == -1);
    } END_TEST;
    TEST("near-exact fit") {
        Page p(0);
        int available = p.getFreeSpace();
        // Record consumes record data + slot entry; getFreeSpace doesn't account for slot entry
        assert(p.insertRecord(std::vector<char>(available - SLOT_ENTRY_SIZE, 'x')) != -1);
    } END_TEST;
}

// ─── BPlusTreeNode ───────────────────────────────────────────────────────────

static void test_node() {
    TEST("create leaf") { BPlusTreeNode n(true); assert(n.is_leaf); } END_TEST;
    TEST("create internal") { BPlusTreeNode n(false); assert(!n.is_leaf); } END_TEST;
    TEST("insert and find") {
        BPlusTreeNode n(true);
        n.insertInLeaf(Key("b"), 0, 1); n.insertInLeaf(Key("a"), 0, 2);
        assert(n.keys[0] == Key("a"));
        auto f = n.findInLeaf(Key("b"));
        assert(f.has_value() && f->slot_id == 1);
    } END_TEST;
    TEST("find nonexistent") {
        BPlusTreeNode n(true); n.insertInLeaf(Key("a"),0,0);
        assert(!n.findInLeaf(Key("z")).has_value());
    } END_TEST;
    TEST("update existing") {
        BPlusTreeNode n(true); n.insertInLeaf(Key("k"),1,1);
        assert(n.updateInLeaf(Key("k"),9,9));
        assert(n.findInLeaf(Key("k"))->page_id == 9);
    } END_TEST;
    TEST("update nonexistent") {
        BPlusTreeNode n(true);
        assert(!n.updateInLeaf(Key("x"),0,0));
    } END_TEST;
    TEST("isFull after max keys") {
        BPlusTreeNode n(true);
        for (int i = 0; i < MAX_KEYS; i++)
            n.insertInLeaf(Key(std::to_string(i)),0,i);
        assert(!n.isFull());
        n.insertInLeaf(Key("overflow"),0,99);
        assert(n.isFull());
    } END_TEST;
    TEST("split leaf") {
        BPlusTreeNode n(true);
        for (int i = 0; i < MAX_KEYS+1; i++)
            n.insertInLeaf(Key(std::to_string(i)),0,i);
        auto [sk, nn] = n.splitLeafNode();
        assert(n.keys.size() <= (size_t)MAX_KEYS);
        assert(nn->keys.size() > 0);
    } END_TEST;
    TEST("serialize round-trip") {
        BPlusTreeNode a(true); a.node_id = 1;
        a.insertInLeaf(Key("hello"),2,3); a.insertInLeaf(Key("world"),4,5);
        BPlusTreeNode b = BPlusTreeNode::deserialize(a.serialize());
        assert(b.node_id == 1 && b.keys.size() == 2);
    } END_TEST;
    TEST("deserialize throws cleanly on corrupt/unknown key type tag") {
        // Hand-build a page: node_id=1, is_leaf=true, num_keys=1, then an
        // invalid type_tag at the first key slot (offset 32). Must be
        // rejected instead of reading garbage past the buffer.
        std::vector<char> buf(PAGE_SIZE, 0);
        int node_id = 1;
        std::memcpy(buf.data() + 0, &node_id, sizeof(int));
        char is_leaf = 1;
        std::memcpy(buf.data() + 4, &is_leaf, 1);
        int num_keys = 1;
        std::memcpy(buf.data() + 8, &num_keys, sizeof(int));
        int bogus_type_tag = 99;
        std::memcpy(buf.data() + 32, &bogus_type_tag, sizeof(int));

        bool caught = false;
        try { BPlusTreeNode::deserialize(buf); }
        catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    TEST("deserialize throws cleanly on truncated string key length") {
        // Same as above but type_tag=2 (STRING) with a length that claims
        // more bytes than the page actually has.
        std::vector<char> buf(PAGE_SIZE, 0);
        int node_id = 1;
        std::memcpy(buf.data() + 0, &node_id, sizeof(int));
        char is_leaf = 1;
        std::memcpy(buf.data() + 4, &is_leaf, 1);
        int num_keys = 1;
        std::memcpy(buf.data() + 8, &num_keys, sizeof(int));
        int type_tag = 2; // STRING
        std::memcpy(buf.data() + 32, &type_tag, sizeof(int));
        int huge_len = PAGE_SIZE * 10;
        std::memcpy(buf.data() + 36, &huge_len, sizeof(int));

        bool caught = false;
        try { BPlusTreeNode::deserialize(buf); }
        catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    TEST("serialize throws cleanly when a key can't fit in a page") {
        // A string key long enough that header + type_tag + len + data
        // exceeds PAGE_SIZE (4096) must be rejected with a clean error,
        // not silently overrun the fixed-size serialize buffer.
        BPlusTreeNode n(true);
        std::string huge(PAGE_SIZE, 'x');
        n.insertInLeaf(Key(huge), 0, 0);
        bool caught = false;
        try { n.serialize(); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
}

// ─── BPlusTree ───────────────────────────────────────────────────────────────

static void test_bplustree() {
    TEST("insert and search") {
        BPlusTree t; t.insert(Key("a"),0,1);
        auto r = t.search(Key("a"));
        assert(r.has_value() && r->slot_id == 1);
    } END_TEST;
    TEST("search nonexistent") {
        BPlusTree t; t.insert(Key("x"),0,0);
        assert(!t.search(Key("y")).has_value());
    } END_TEST;
    TEST("search empty tree") {
        BPlusTree t;
        assert(!t.search(Key("a")).has_value());
    } END_TEST;
    TEST("update empty tree") {
        BPlusTree t;
        assert(!t.update(Key("a"),0,0));
    } END_TEST;
    TEST("remove empty tree") {
        BPlusTree t;
        assert(!t.remove(Key("a")));
    } END_TEST;
    TEST("range scan empty tree") {
        BPlusTree t;
        assert(t.rangeScan(Key("a"),Key("z")).empty());
    } END_TEST;
    TEST("getAll empty tree") {
        BPlusTree t;
        assert(t.getAllKeyRIDPairs().empty());
    } END_TEST;
    TEST("many inserts") {
        BPlusTree t;
        for (int i = 0; i < 20; i++) t.insert(Key(std::to_string(i)),i,i*10);
        for (int i = 0; i < 20; i++) {
            auto r = t.search(Key(std::to_string(i)));
            assert(r.has_value() && r->page_id == i);
        }
    } END_TEST;
    TEST("update") {
        BPlusTree t; t.insert(Key("k"),1,1);
        assert(t.update(Key("k"),9,9));
        assert(t.search(Key("k"))->page_id == 9);
    } END_TEST;
    TEST("update nonexistent") { BPlusTree t; assert(!t.update(Key("n"),0,0)); } END_TEST;
    TEST("remove") {
        BPlusTree t; t.insert(Key("a"),0,0); t.insert(Key("b"),1,1);
        assert(t.remove(Key("a")));
        assert(!t.search(Key("a")).has_value());
        assert(t.search(Key("b")).has_value());
    } END_TEST;
    TEST("remove nonexistent") { BPlusTree t; assert(!t.remove(Key("n"))); } END_TEST;
    TEST("remove all keys from single node") {
        BPlusTree t;
        t.insert(Key("a"),0,0); t.insert(Key("b"),1,1);
        assert(t.remove(Key("a")));
        assert(t.remove(Key("b")));
        assert(!t.search(Key("a")).has_value());
        assert(!t.search(Key("b")).has_value());
    } END_TEST;
    TEST("remove all then re-insert") {
        BPlusTree t;
        t.insert(Key("x"),0,0); t.remove(Key("x"));
        t.insert(Key("y"),1,1);
        assert(t.search(Key("y")).has_value());
    } END_TEST;
    TEST("range scan") {
        BPlusTree t;
        for (int i = 0; i < 10; i++) t.insert(Key(std::to_string(i)),i,i);
        auto r = t.rangeScan(Key("2"),Key("5"));
        assert(r.size() == 4);
        assert(r[0].first == Key("2"));
    } END_TEST;
    TEST("range scan no results") {
        BPlusTree t; t.insert(Key("a"),0,0);
        assert(t.rangeScan(Key("z"),Key("zz")).empty());
    } END_TEST;
    TEST("range scan single key") {
        BPlusTree t; t.insert(Key("m"),0,0);
        auto r = t.rangeScan(Key("m"),Key("m"));
        assert(r.size() == 1);
    } END_TEST;
    TEST("getAllKeyRIDPairs") {
        BPlusTree t; t.insert(Key("a"),0,0); t.insert(Key("b"),1,1);
        assert(t.getAllKeyRIDPairs().size() == 2);
    } END_TEST;
    TEST("duplicate key insert") {
        BPlusTree t;
        t.insert(Key("dup"),0,0);
        t.insert(Key("dup"),1,1);
        auto all = t.getAllKeyRIDPairs();
        assert(all.size() == 2);
    } END_TEST;
    TEST("remove causing leaf underflow merge") {
        // ORDER=4 so max 3 keys, min 1. Insert enough to force splits then remove.
        BPlusTree t;
        for (int i = 0; i < 6; i++) t.insert(Key(std::to_string(i)),i,i);
        // Remove until underflow forces merge
        assert(t.remove(Key("0")));
        assert(t.remove(Key("1")));
        auto all = t.getAllKeyRIDPairs();
        assert(all.size() == 4);
    } END_TEST;
    TEST("sequential ascending insert") {
        BPlusTree t;
        for (int i = 0; i < 15; i++) t.insert(Key(std::to_string(i)),i,i);
        for (int i = 0; i < 15; i++) assert(t.search(Key(std::to_string(i))).has_value());
    } END_TEST;
    TEST("descending insert") {
        BPlusTree t;
        for (int i = 14; i >= 0; i--) t.insert(Key(std::to_string(i)),i,i);
        for (int i = 0; i < 15; i++) assert(t.search(Key(std::to_string(i))).has_value());
    } END_TEST;
}

// ─── CatalogManager ──────────────────────────────────────────────────────────

static void test_catalog_basic() {
    const std::string file = "cat_basic.txt";
    std::remove(file.c_str());
    TEST("create and check table") {
        CatalogManager cm(file);
        cm.createTable("users", Schema(std::vector<Column>{{"id","int"},{"name","string"}}));
        assert(cm.hasTable("users"));
        assert(cm.getSchema("users").getColumns().size() == 2);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_catalog_persist() {
    const std::string file = "cat_persist.txt";
    std::remove(file.c_str());
    {
        CatalogManager cm(file);
        cm.createTable("t1", Schema(std::vector<Column>{{"a","int"}}));
        cm.createTable("t2", Schema(std::vector<Column>{{"b","string"}}));
    }
    {
        CatalogManager cm(file);
        assert(cm.hasTable("t1") && cm.hasTable("t2"));
        assert(cm.getSchema("t1").getColumns()[0].name == "a");
    }
    std::remove(file.c_str());
}

static void test_catalog_dup() {
    const std::string file = "cat_dup.txt";
    std::remove(file.c_str());
    TEST("duplicate table throws") {
        CatalogManager cm(file);
        auto s = Schema(std::vector<Column>{{"id","int"}});
        cm.createTable("x", s);
        bool caught = false;
        try { cm.createTable("x", s); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_catalog_missing() {
    const std::string file = "cat_missing.txt";
    std::remove(file.c_str());
    TEST("get missing table throws") {
        CatalogManager cm(file);
        bool caught = false;
        try { cm.getSchema("nope"); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(file.c_str());
}

// ─── PageManager ─────────────────────────────────────────────────────────────

static void test_pm_alloc() {
    const std::string file = "pm_alloc.db";
    std::remove(file.c_str());
    TEST("allocate page") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns);
        assert(pm.allocatePage() == 0);
        assert(pm.getNextPageId() == 1);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_pm_readback() {
    const std::string file = "pm_read.db";
    std::remove(file.c_str());
    TEST("write and read back") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns);
        int id = pm.allocatePage();
        assert(pm.readPage(id).getPageId() == (uint32_t)id);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_pm_multi() {
    const std::string file = "pm_multi.db";
    std::remove(file.c_str());
    {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns);
        for (int i = 0; i < 5; i++) assert(pm.allocatePage() == i);
        assert(pm.getNextPageId() == 5);
    }
    {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns);
        assert(pm.getNextPageId() == 5);
    }
    std::remove(file.c_str());
}

// ─── RecordManager ───────────────────────────────────────────────────────────

static void test_rm_basic() {
    const std::string file = "rm_basic.db";
    std::remove(file.c_str());
    TEST("insert and read") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        RID rid = rm.insertRecord(Record(std::vector<std::string>{"hello","world"}));
        Record r = rm.readRecord(rid);
        assert(r.getFields()[0] == "hello");
    } END_TEST;
    TEST("delete record") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        RID rid = rm.insertRecord(Record(std::vector<std::string>{"del"}));
        assert(rm.deleteRecord(rid));
    } END_TEST;
    std::remove(file.c_str());
}

static void test_rm_multi() {
    const std::string file = "rm_multi.db";
    std::remove(file.c_str());
    TEST("multiple records") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        RID ids[10];
        for (int i = 0; i < 10; i++)
            ids[i] = rm.insertRecord(Record(std::vector<std::string>{std::to_string(i)}));
        for (int i = 0; i < 10; i++)
            assert(rm.readRecord(ids[i]).getFields()[0] == std::to_string(i));
    } END_TEST;
    std::remove(file.c_str());
}

// ─── WAL ─────────────────────────────────────────────────────────────────────

static void test_wal_append_readall() {
    const std::string file = "wal_append.log";
    std::remove(file.c_str());
    TEST("append and readAll round-trip") {
        WALWriter wal(file);
        WALRecord begin;
        begin.txn_id = 1;
        begin.type = WALRecordType::BEGIN;
        uint64_t begin_lsn = wal.append(begin);

        WALRecord upd;
        upd.txn_id = 1;
        upd.type = WALRecordType::UPDATE;
        upd.store = WALStore::HEAP;
        upd.page_id = 7;
        upd.prev_lsn = begin_lsn;
        upd.old_data = std::vector<char>(10, 'a');
        upd.new_data = std::vector<char>(10, 'b');
        uint64_t upd_lsn = wal.append(upd);

        WALRecord commit;
        commit.txn_id = 1;
        commit.type = WALRecordType::COMMIT;
        commit.prev_lsn = upd_lsn;
        wal.append(commit);
        wal.flush();

        auto records = wal.readAll();
        assert(records.size() == 3);
        assert(records[0].type == WALRecordType::BEGIN);
        assert(records[1].type == WALRecordType::UPDATE);
        assert(records[1].page_id == 7);
        assert(records[1].old_data == std::vector<char>(10, 'a'));
        assert(records[1].new_data == std::vector<char>(10, 'b'));
        assert(records[1].prev_lsn == begin_lsn);
        assert(records[2].type == WALRecordType::COMMIT);
        assert(records[0].lsn < records[1].lsn);
        assert(records[1].lsn < records[2].lsn);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_wal_lsn_continues_across_reopen() {
    const std::string file = "wal_reopen.log";
    std::remove(file.c_str());
    TEST("LSN counter survives reopen") {
        uint64_t last_lsn;
        {
            WALWriter wal(file);
            WALRecord r1; r1.type = WALRecordType::BEGIN; r1.txn_id = 1;
            wal.append(r1);
            WALRecord r2; r2.type = WALRecordType::COMMIT; r2.txn_id = 1;
            last_lsn = wal.append(r2);
            wal.flush();
        }
        {
            WALWriter wal(file);
            assert(wal.currentLSN() == last_lsn);
            WALRecord r3; r3.type = WALRecordType::BEGIN; r3.txn_id = 2;
            uint64_t new_lsn = wal.append(r3);
            assert(new_lsn == last_lsn + 1);
        }
        WALWriter wal(file);
        auto records = wal.readAll();
        assert(records.size() == 3);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_wal_torn_tail_is_discarded() {
    const std::string file = "wal_torn.log";
    std::remove(file.c_str());
    TEST("corrupt/torn tail record is discarded, later appends stay contiguous") {
        {
            WALWriter wal(file);
            WALRecord r1; r1.type = WALRecordType::BEGIN; r1.txn_id = 1;
            wal.append(r1);
            wal.flush();
        }
        // Simulate a crash mid-write: append a bogus, too-short "record"
        // (a length prefix claiming more bytes than actually follow).
        {
            std::fstream raw(file, std::ios::in | std::ios::out | std::ios::binary);
            raw.seekp(0, std::ios::end);
            uint32_t bogus_len = 999;
            raw.write(reinterpret_cast<const char*>(&bogus_len), sizeof(bogus_len));
            raw.write("short", 5);
        }
        WALWriter wal(file);
        auto records = wal.readAll();
        assert(records.size() == 1);
        assert(records[0].type == WALRecordType::BEGIN);

        // A fresh append must land right after the one valid record, not
        // after the discarded torn bytes.
        WALRecord r2; r2.type = WALRecordType::COMMIT; r2.txn_id = 1;
        wal.append(r2);
        wal.flush();

        WALWriter wal2(file);
        auto records2 = wal2.readAll();
        assert(records2.size() == 2);
        assert(records2[1].type == WALRecordType::COMMIT);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_wal_transaction_manager() {
    const std::string file = "wal_txn.log";
    std::remove(file.c_str());
    TEST("TransactionManager chains prev_lsn per txn and commits") {
        WALWriter wal(file);
        TransactionManager txns(wal);

        uint64_t t1 = txns.begin();
        uint64_t lsn_a = txns.appendRecord(t1, WALRecordType::UPDATE, WALStore::INDEX, 3, {}, {});
        uint64_t lsn_b = txns.appendRecord(t1, WALRecordType::UPDATE, WALStore::INDEX, 4, {}, {});
        txns.commit(t1);

        auto records = wal.readAll();
        assert(records.size() == 4);  // BEGIN, UPDATE, UPDATE, COMMIT
        assert(records[1].lsn == lsn_a);
        assert(records[2].prev_lsn == lsn_a);
        assert(records[2].lsn == lsn_b);
        assert(records[3].type == WALRecordType::COMMIT);
        assert(records[3].prev_lsn == lsn_b);

        // A second, independent transaction gets its own prev_lsn chain
        // starting from its own BEGIN, not the first txn's.
        uint64_t t2 = txns.begin();
        uint64_t lsn_c = txns.appendRecord(t2, WALRecordType::UPDATE, WALStore::HEAP, 5, {}, {});
        auto records2 = wal.readAll();
        assert(records2.back().lsn == lsn_c);
        assert(records2.back().prev_lsn == records2[4].lsn);  // t2's BEGIN
    } END_TEST;
    std::remove(file.c_str());
}

static void test_wal() {
    test_wal_append_readall();
    test_wal_lsn_continues_across_reopen();
    test_wal_torn_tail_is_discarded();
    test_wal_transaction_manager();
}

// ─── Recovery ────────────────────────────────────────────────────────────────
// These craft the exact WAL/on-disk states a real crash would leave
// behind (committed record never write-backed; uncommitted record that
// did reach disk under steal) rather than trying to force a live crash
// via killed processes, then verify RecoveryManager restores the correct
// state before any BufferPool/IndexManager is trusted to serve reads.

static void test_recovery_heap_redo() {
    const std::string data_file = "rec_heap_redo.db";
    const std::string index_file = "rec_heap_redo.idx";
    const std::string wal_file = "rec_heap_redo.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("redo replays a committed heap UPDATE that never reached the data file") {
        Page old_page(0);
        std::vector<char> old_data = old_page.serialize();
        Page new_page(0);
        new_page.insertRecord({'h', 'i'});
        std::vector<char> new_data = new_page.serialize();

        {
            // Commit an UPDATE to the WAL directly -- simulates a
            // BufferPool write whose WAL record made it to disk but whose
            // page write-back (deferred under steal/no-force) never did.
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            uint64_t txn = txns.begin();
            txns.appendRecord(txn, WALRecordType::UPDATE, WALStore::HEAP, 0, old_data, new_data);
            txns.commit(txn);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns);
            Page recovered = pm.readPage(0);
            auto rec = recovered.readRecord(0);
            assert(rec.size() == 2 && rec[0] == 'h' && rec[1] == 'i');
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_recovery_heap_undo() {
    const std::string data_file = "rec_heap_undo.db";
    const std::string index_file = "rec_heap_undo.idx";
    const std::string wal_file = "rec_heap_undo.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("undo reverts an uncommitted heap UPDATE, even if it reached disk") {
        Page original(0);
        original.insertRecord({'o', 'k'});
        std::vector<char> original_data = original.serialize();

        Page corrupted(0);
        corrupted.insertRecord({'o', 'k'});
        corrupted.insertRecord({'b', 'a', 'd'});
        std::vector<char> corrupted_data = corrupted.serialize();

        uint64_t crashed_txn;
        {
            // Clean, durable prior state: a committed write, then a
            // second UPDATE that never gets a COMMIT -- simulates a
            // crash mid-transaction. Steal means the dirty page *can*
            // have reached disk before the crash, so write it there too:
            // undo must revert it regardless of what's on disk.
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            uint64_t setup_txn = txns.begin();
            txns.appendRecord(setup_txn, WALRecordType::UPDATE, WALStore::HEAP, 0,
                               Page(0).serialize(), original_data);
            txns.commit(setup_txn);

            crashed_txn = txns.begin();
            txns.appendRecord(crashed_txn, WALRecordType::UPDATE, WALStore::HEAP, 0,
                               original_data, corrupted_data);
            // No commit() -- this is the "crash before commit" state.

            std::fstream raw(data_file, std::ios::in | std::ios::out | std::ios::binary);
            if (!raw.is_open()) { raw.clear(); raw.open(data_file, std::ios::out | std::ios::binary); raw.close();
                                   raw.open(data_file, std::ios::in | std::ios::out | std::ios::binary); }
            raw.seekp(0, std::ios::beg);
            raw.write(corrupted_data.data(), corrupted_data.size());
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();

            // The undo pass must have logged a CLR for the reverted change.
            bool found_clr = false;
            for (auto& r : wal.readAll()) {
                if (r.type == WALRecordType::CLR && r.txn_id == crashed_txn && r.store == WALStore::HEAP) {
                    found_clr = true;
                }
            }
            assert(found_clr);
        }
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns);
            Page recovered = pm.readPage(0);
            assert(recovered.getNumSlots() == 1);
            auto rec = recovered.readRecord(0);
            assert(rec.size() == 2 && rec[0] == 'o' && rec[1] == 'k');
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_recovery_index_redo_and_undo() {
    const std::string data_file = "rec_idx.db";
    const std::string index_file = "rec_idx.idx";
    const std::string wal_file = "rec_idx.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("redo and undo both work against index node pages, not just heap pages") {
        auto emptyNode = std::make_shared<BPlusTreeNode>(true);
        emptyNode->node_id = 0;
        std::vector<char> empty_bytes = emptyNode->serialize();

        auto committedNode = std::make_shared<BPlusTreeNode>(true);
        committedNode->node_id = 0;
        committedNode->insertInLeaf(Key(1), 1, 1);
        std::vector<char> committed_bytes = committedNode->serialize();

        auto uncommittedNode = std::make_shared<BPlusTreeNode>(true);
        uncommittedNode->node_id = 0;
        uncommittedNode->insertInLeaf(Key(1), 1, 1);
        uncommittedNode->insertInLeaf(Key(2), 2, 2);
        std::vector<char> uncommitted_bytes = uncommittedNode->serialize();

        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            // Committed: redo must apply it (never reached the index file).
            uint64_t t1 = txns.begin();
            txns.appendRecord(t1, WALRecordType::UPDATE, WALStore::INDEX, 0, empty_bytes, committed_bytes);
            txns.commit(t1);
            // Uncommitted: undo must revert it back to committed_bytes.
            uint64_t t2 = txns.begin();
            txns.appendRecord(t2, WALRecordType::UPDATE, WALStore::INDEX, 0, committed_bytes, uncommitted_bytes);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            IndexManager im(index_file, wal, txns);
            auto node = im.loadNode(0);
            assert(node->keys.size() == 1);
            assert(node->keys[0] == Key(1));
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_recovery_combined_heap_and_index() {
    const std::string data_file = "rec_combined.db";
    const std::string index_file = "rec_combined.idx";
    const std::string wal_file = "rec_combined.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("one shared WAL correctly recovers interleaved heap and index writes") {
        Page heap_new(0);
        heap_new.insertRecord({'x'});
        std::vector<char> heap_new_data = heap_new.serialize();

        auto node = std::make_shared<BPlusTreeNode>(true);
        node->node_id = 0;
        node->insertInLeaf(Key(5), 0, 0);
        std::vector<char> node_bytes = node->serialize();

        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            // Interleave: heap write, index write, both committed --
            // both are "crash before write-back", both need redo.
            uint64_t t1 = txns.begin();
            txns.appendRecord(t1, WALRecordType::UPDATE, WALStore::HEAP, 0, Page(0).serialize(), heap_new_data);
            txns.commit(t1);

            uint64_t t2 = txns.begin();
            txns.appendRecord(t2, WALRecordType::UPDATE, WALStore::INDEX, 0,
                               BPlusTreeNode(true).serialize(), node_bytes);
            txns.commit(t2);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns);
            assert(pm.readPage(0).readRecord(0).size() == 1);

            IndexManager im(index_file, wal, txns);
            auto reloaded = im.loadNode(0);
            assert(reloaded->keys.size() == 1 && reloaded->keys[0] == Key(5));
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_recovery_empty_wal_is_a_noop() {
    const std::string data_file = "rec_empty.db";
    const std::string index_file = "rec_empty.idx";
    const std::string wal_file = "rec_empty.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("running recovery against a fresh, empty WAL does nothing and doesn't throw") {
        WALWriter wal(wal_file);
        RecoveryManager recovery(wal, data_file, index_file);
        recovery.run();
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_recovery_end_to_end_via_real_bufferpool() {
    const std::string data_file = "rec_e2e.db";
    const std::string index_file = "rec_e2e.idx";
    const std::string wal_file = "rec_e2e.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    TEST("real BufferPool writes that never got flushed are recovered via WAL redo") {
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            // Heap-allocated and deliberately never deleted: standard way
            // to simulate "the process died" without actually crashing
            // this test binary. Its destructor (which would flush all
            // dirty pages to the data file) never runs, so this write
            // exists only in the WAL, not yet in rec_e2e.db -- exactly
            // the state a real crash under steal/no-force leaves behind.
            BufferPool* bp = new BufferPool(data_file, wal, txns);
            int id = bp->allocatePage();
            Page& p = bp->fetchPage(id);
            p.insertRecord({'c', 'r', 'a', 's', 'h'});
            bp->unpinPage(id, true);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file);
            TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns);
            auto rec = pm.readPage(0).readRecord(0);
            assert(rec.size() == 5 && rec[0] == 'c');
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

// ─── Table ───────────────────────────────────────────────────────────────────

static void test_table_basic() {
    const std::string file = "tbl_basic.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    TEST("insert and get by key") {
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        Table t("users", Schema(std::vector<Column>{{"id","int"},{"name","string"}}), pm, rm);
        t.insert({"1","Alice"}); t.insert({"2","Bob"});
        auto r = t.getByKey("1");
        assert(r.has_value() && r->getFields()[1] == "Alice");
    } END_TEST;
    TEST("get nonexistent key") {
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        Table t("x", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        assert(!t.getByKey("99").has_value());
    } END_TEST;
    std::remove(file.c_str());
}

static void test_table_delete() {
    const std::string file = "tbl_del.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    TEST("delete by key") {
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        t.insert({"1"}); assert(t.deleteByKey("1"));
        assert(!t.getByKey("1").has_value());
    } END_TEST;
    TEST("delete nonexistent") {
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        assert(!t.deleteByKey("99"));
    } END_TEST;
    std::remove(file.c_str());
}

static void test_table_insert_mismatch() {
    const std::string file = "tbl_mismatch.db";
    std::remove(file.c_str());
    TEST("insert mismatched columns throws") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        PageManager pm(file, wal, txns); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        bool caught = false;
        try { t.insert({"1","extra"}); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(file.c_str());
}

// ─── BPlusTree Persistence ───────────────────────────────────────────────────

static void test_bpt_persist_20() {
    const std::string file = "bt_20.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    {
        TEST("insert 20 keys and persist") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            for (int i = 0; i < 20; i++) {
                t.insert(Key(std::to_string(i)), i, i * 10);
            }
        } END_TEST;
    }
    {
        TEST("reload and verify all 20 keys survive") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            for (int i = 0; i < 20; i++) {
                auto r = t.search(Key(std::to_string(i)));
                assert(r.has_value());
                assert(r->page_id == i);
                assert(r->slot_id == i * 10);
            }
        } END_TEST;
    }
    {
        TEST("reload and range scan") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            auto results = t.rangeScan(Key("5"), Key("9"));
            assert(results.size() == 5);
        } END_TEST;
    }
    {
        TEST("reload and getAll") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            assert(t.getAllKeyRIDPairs().size() == 20);
        } END_TEST;
    }
    std::remove(file.c_str());
}

static void test_bpt_persist_update() {
    const std::string file = "bt_update.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    {
        TEST("insert, update, persist") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            t.insert(Key("x"), 1, 1);
            t.insert(Key("y"), 2, 2);
            assert(t.update(Key("x"), 99, 99));
        } END_TEST;
    }
    {
        TEST("reload and verify update survived") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            auto r = t.search(Key("x"));
            assert(r.has_value());
            assert(r->page_id == 99);
            assert(r->slot_id == 99);
            assert(t.search(Key("y"))->page_id == 2);
        } END_TEST;
    }
    std::remove(file.c_str());
}

static void test_bpt_persist_remove() {
    const std::string file = "bt_remove.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    {
        TEST("insert, remove, persist") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            t.insert(Key("keep"), 0, 0);
            t.insert(Key("gone"), 1, 1);
            assert(t.remove(Key("gone")));
        } END_TEST;
    }
    {
        TEST("reload and verify remove survived") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            assert(t.search(Key("keep")).has_value());
            assert(!t.search(Key("gone")).has_value());
        } END_TEST;
    }
    std::remove(file.c_str());
}

static void test_bpt_persist_empty() {
    const std::string file = "bt_empty.db";
    std::remove(file.c_str());
    WALWriter wal(file + ".wal"); TransactionManager txns(wal);
    {
        TEST("save empty tree") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            t.insert(Key("a"), 0, 0);
            t.remove(Key("a"));
        } END_TEST;
    }
    {
        TEST("reload empty tree") {
            IndexManager im(file, wal, txns);
            BPlusTree t(im);
            assert(!t.search(Key("a")).has_value());
            assert(t.getAllKeyRIDPairs().empty());
        } END_TEST;
    }
    std::remove(file.c_str());
}

// ─── IndexManager ────────────────────────────────────────────────────────────

static void test_indexmanager_load_unsaved_node() {
    const std::string file = "im_unsaved.db";
    std::remove(file.c_str());
    TEST("loadNode throws on a node_id that was never saved") {
        // A fresh IndexManager has never called saveNode/allocateNodeID, so
        // node_id 5 does not exist on disk. loadNode() must fail loudly
        // instead of silently zero-extending the file and returning a
        // bogus-but-valid-looking empty node (masks real corruption/bugs).
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        IndexManager im(file, wal, txns);
        bool caught = false;
        try { im.loadNode(5); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_indexmanager_incremental_writes() {
    const std::string file = "im_incremental.db";
    const std::string wal_file = file + ".wal";
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
    TEST("a plain leaf insert (no split) logs exactly one INDEX UPDATE record, not the whole tree") {
        WALWriter wal(wal_file); TransactionManager txns(wal);
        IndexManager im(file, wal, txns);
        BPlusTree t(im);
        // Build a multi-level tree (ORDER=4, so this forces several splits).
        for (int i = 0; i < 20; i++) t.insert(Key(i), i, i * 10);

        auto countIndexUpdates = [&]() {
            int n = 0;
            for (auto& r : wal.readAll()) {
                if (r.type == WALRecordType::UPDATE && r.store == WALStore::INDEX) n++;
            }
            return n;
        };
        int before = countIndexUpdates();

        // Insert one more key into a leaf that still has room (no split
        // expected): should touch exactly one node, not the ~7+ nodes a
        // 20-key order-4 tree has by now.
        t.insert(Key(1000), 1000, 1000);
        int after = countIndexUpdates();

        assert(after - before >= 1);
        assert(after - before <= 2);  // the touched leaf, plus at most one ancestor if a separator changed
    } END_TEST;
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
}

static void test_indexmanager_saves_new_nodes_on_split() {
    const std::string file = "im_split.db";
    const std::string wal_file = file + ".wal";
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
    WALWriter wal(wal_file); TransactionManager txns(wal);
    TEST("build a multi-level tree and remove a key via incremental writes") {
        IndexManager im(file, wal, txns);
        BPlusTree t(im);
        for (int i = 0; i < 30; i++) t.insert(Key(i), i, i * 10);
        assert(t.remove(Key(15)));
    } END_TEST;
    TEST("reload after incremental writes still reconstructs the full tree") {
        IndexManager im(file, wal, txns);
        BPlusTree t(im);
        for (int i = 0; i < 30; i++) {
            if (i == 15) { assert(!t.search(Key(i)).has_value()); continue; }
            auto r = t.search(Key(i));
            assert(r.has_value());
            assert(r->page_id == i);
        }
    } END_TEST;
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
}

// ─── BufferPool ──────────────────────────────────────────────────────────────

static void test_bp_fetch_unpin() {
    const std::string file = "bp_fetch.db";
    std::remove(file.c_str());
    TEST("fetch and unpin cycle") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        int id = bp.allocatePage();
        Page& p = bp.fetchPage(id);
        assert(p.getPageId() == (uint32_t)id);
        bp.unpinPage(id, false);
        bp.flush();
    } END_TEST;
    std::remove(file.c_str());
}

static void test_bp_write_readback() {
    const std::string file = "bp_rw.db";
    std::remove(file.c_str());
    TEST("write data and read back") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        int id = bp.allocatePage();
        {
            Page& p = bp.fetchPage(id);
            p.insertRecord({'h', 'i'});
            bp.unpinPage(id, true);
        }
        bp.flush();

        BufferPool bp2(file, wal, txns);
        Page& p2 = bp2.fetchPage(id);
        auto rec = p2.readRecord(0);
        assert(rec.size() == 2 && rec[0] == 'h');
        bp2.unpinPage(id, false);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_bp_eviction() {
    const std::string file = "bp_evict.db";
    std::remove(file.c_str());
    TEST("eviction reuses frames") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        // Allocate more pages than frames to force eviction
        int ids[100];
        for (int i = 0; i < 100; i++) {
            ids[i] = bp.allocatePage();
            Page& p = bp.fetchPage(ids[i]);
            bp.unpinPage(ids[i], false);
        }
        // All pages should still be readable (eviction writes them back)
        for (int i = 0; i < 100; i++) {
            Page& p = bp.fetchPage(ids[i]);
            assert(p.getPageId() == (uint32_t)ids[i]);
            bp.unpinPage(ids[i], false);
        }
    } END_TEST;
    std::remove(file.c_str());
}

static void test_bp_dirty_flush() {
    const std::string file = "bp_dirty.db";
    std::remove(file.c_str());
    TEST("dirty pages survive flush and reopen") {
        int id;
        {
            WALWriter wal(file + ".wal"); TransactionManager txns(wal);
            BufferPool bp(file, wal, txns);
            id = bp.allocatePage();
            Page& p = bp.fetchPage(id);
            p.insertRecord({'x', 'y', 'z'});
            bp.unpinPage(id, true);
            bp.flush();
        }
        {
            WALWriter wal(file + ".wal"); TransactionManager txns(wal);
            BufferPool bp(file, wal, txns);
            Page& p = bp.fetchPage(id);
            auto rec = p.readRecord(0);
            assert(rec.size() == 3);
            bp.unpinPage(id, false);
        }
    } END_TEST;
    std::remove(file.c_str());
}

static void test_bp_sequential_ids() {
    const std::string file = "bp_seq.db";
    std::remove(file.c_str());
    TEST("sequential page IDs") {
        WALWriter wal(file + ".wal"); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        assert(bp.allocatePage() == 0);
        assert(bp.allocatePage() == 1);
        assert(bp.allocatePage() == 2);
        assert(bp.getNextPageId() == 3);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_bp_wal_logs_mutations() {
    const std::string file = "bp_wal.db";
    const std::string wal_file = file + ".wal";
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
    TEST("mutating a page through fetch/unpin(dirty) logs one UPDATE record") {
        WALWriter wal(wal_file); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        int id = bp.allocatePage();
        {
            Page& p = bp.fetchPage(id);
            p.insertRecord({'w', 'a', 'l'});
            bp.unpinPage(id, true);
        }

        auto records = wal.readAll();
        int update_count = 0;
        const WALRecord* upd = nullptr;
        for (const auto& r : records) {
            if (r.type == WALRecordType::UPDATE && r.store == WALStore::HEAP && r.page_id == id) {
                update_count++;
                upd = &r;
            }
        }
        assert(update_count == 1);
        assert(upd->old_data != upd->new_data);
        assert(upd->new_data.size() == PAGE_SIZE);

        // A pure read (unpin dirty=false) must not produce another record.
        Page& p2 = bp.fetchPage(id);
        (void)p2;
        bp.unpinPage(id, false);
        assert(wal.readAll().size() == records.size());
    } END_TEST;
    TEST("a second, separate mutation on the same page logs a second UPDATE") {
        WALWriter wal(wal_file); TransactionManager txns(wal);
        BufferPool bp(file, wal, txns);
        int id = bp.allocatePage();
        { Page& p = bp.fetchPage(id); p.insertRecord({'a'}); bp.unpinPage(id, true); }
        { Page& p = bp.fetchPage(id); p.insertRecord({'b'}); bp.unpinPage(id, true); }

        auto records = wal.readAll();
        int update_count = 0;
        for (const auto& r : records) {
            if (r.type == WALRecordType::UPDATE && r.store == WALStore::HEAP && r.page_id == id) update_count++;
        }
        assert(update_count == 2);
    } END_TEST;
    std::remove(file.c_str());
    std::remove(wal_file.c_str());
}

// ─── Concurrent BPlusTree (Phase 3: latch crabbing + B-link) ─────────────────
// No ThreadSanitizer on this MinGW toolchain (-fsanitize=thread fails to
// link), so these lean on volume + varied access patterns across many
// threads/iterations rather than a sanitizer to surface races.

static void test_concurrent_disjoint_inserts() {
    TEST("concurrent: disjoint inserts from many threads all land") {
        constexpr int NUM_THREADS = 8;
        constexpr int KEYS_PER_THREAD = 200;
        BPlusTree t;

        std::vector<std::thread> threads;
        for (int ti = 0; ti < NUM_THREADS; ++ti) {
            threads.emplace_back([&t, ti]() {
                for (int i = 0; i < KEYS_PER_THREAD; ++i) {
                    int k = ti * KEYS_PER_THREAD + i;
                    t.insert(Key(k), k, k * 10);
                }
            });
        }
        for (auto& th : threads) th.join();

        for (int k = 0; k < NUM_THREADS * KEYS_PER_THREAD; ++k) {
            auto r = t.search(Key(k));
            if (!r.has_value() || r->page_id != k || r->slot_id != k * 10) {
                std::cerr << "MISSING/WRONG key " << k << "\n";
                assert(false);
            }
        }
        auto all = t.getAllKeyRIDPairs();
        assert(all.size() == static_cast<size_t>(NUM_THREADS * KEYS_PER_THREAD));
        for (size_t i = 1; i < all.size(); ++i) {
            assert(all[i - 1].first < all[i].first); // still sorted, no duplicates
        }
    } END_TEST;
}

static void test_concurrent_mixed_insert_search() {
    TEST("concurrent: mixed insert + search, no crash, no wrong reads") {
        constexpr int NUM_KEYS = 320; // divisible by 2*NUM_WRITER_THREADS so every key actually gets inserted
        constexpr int NUM_WRITER_THREADS = 4;
        constexpr int NUM_READER_THREADS = 4;
        constexpr int SEARCHES_PER_READER = 500;
        BPlusTree t;

        // Seed some keys before the race so readers have something to find.
        for (int i = 0; i < NUM_KEYS / 2; ++i) t.insert(Key(i), i, i);

        std::atomic<bool> stop{false};
        std::atomic<int> wrong_reads{0};
        std::vector<std::thread> threads;

        for (int ti = 0; ti < NUM_WRITER_THREADS; ++ti) {
            threads.emplace_back([&t, ti]() {
                for (int i = 0; i < NUM_KEYS / (2 * NUM_WRITER_THREADS); ++i) {
                    int k = NUM_KEYS / 2 + ti * (NUM_KEYS / (2 * NUM_WRITER_THREADS)) + i;
                    t.insert(Key(k), k, k);
                }
            });
        }
        for (int ti = 0; ti < NUM_READER_THREADS; ++ti) {
            threads.emplace_back([&t, &wrong_reads]() {
                for (int i = 0; i < SEARCHES_PER_READER; ++i) {
                    int k = i % (NUM_KEYS / 2); // always an already-seeded key
                    auto r = t.search(Key(k));
                    if (!r.has_value() || r->page_id != k) {
                        wrong_reads.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();

        assert(wrong_reads.load() == 0);
        for (int k = 0; k < NUM_KEYS; ++k) {
            assert(t.search(Key(k)).has_value());
        }
    } END_TEST;
}

static void test_concurrent_rangescan_during_inserts() {
    TEST("concurrent: rangeScan during inserts sees a consistent, sorted view") {
        constexpr int NUM_KEYS = 400;
        BPlusTree t;
        for (int i = 0; i < NUM_KEYS; i += 2) t.insert(Key(i), i, i); // seed evens

        std::atomic<bool> stop{false};
        std::atomic<int> bad_scans{0};

        std::thread writer([&t, &stop]() {
            for (int i = 1; i < NUM_KEYS; i += 2) { // fill in odds
                t.insert(Key(i), i, i);
            }
            stop.store(true);
        });

        std::thread reader([&t, &stop, &bad_scans]() {
            while (!stop.load()) {
                auto r = t.rangeScan(Key(0), Key(NUM_KEYS));
                for (size_t i = 1; i < r.size(); ++i) {
                    if (!(r[i - 1].first < r[i].first)) {
                        bad_scans.fetch_add(1);
                        break;
                    }
                }
            }
        });

        writer.join();
        reader.join();

        assert(bad_scans.load() == 0);
        auto final_scan = t.rangeScan(Key(0), Key(NUM_KEYS));
        assert(final_scan.size() == static_cast<size_t>(NUM_KEYS));
    } END_TEST;
}

static void test_concurrent_insert_remove() {
    TEST("concurrent: inserts and removes on disjoint keys stay consistent") {
        constexpr int NUM_INITIAL = 300;
        constexpr int NUM_THREADS = 6; // 3 removers, 3 inserters
        BPlusTree t;
        for (int i = 0; i < NUM_INITIAL; ++i) t.insert(Key(i), i, i);

        std::vector<std::thread> threads;
        // Removers: each thread removes a disjoint slice of the initial keys.
        int per_remover = NUM_INITIAL / 3;
        for (int ti = 0; ti < 3; ++ti) {
            threads.emplace_back([&t, ti, per_remover]() {
                for (int i = 0; i < per_remover; ++i) {
                    int k = ti * per_remover + i;
                    t.remove(Key(k));
                }
            });
        }
        // Inserters: each thread inserts a disjoint slice of brand-new keys.
        int per_inserter = 100;
        for (int ti = 0; ti < 3; ++ti) {
            threads.emplace_back([&t, ti, per_inserter]() {
                for (int i = 0; i < per_inserter; ++i) {
                    int k = NUM_INITIAL + 1000 + ti * per_inserter + i;
                    t.insert(Key(k), k, k);
                }
            });
        }
        for (auto& th : threads) th.join();

        for (int i = 0; i < 3 * per_remover; ++i) {
            assert(!t.search(Key(i)).has_value());
        }
        for (int i = 3 * per_remover; i < NUM_INITIAL; ++i) {
            assert(t.search(Key(i)).has_value());
        }
        for (int i = 0; i < 3 * per_inserter; ++i) {
            int ti = i / per_inserter;
            int local = i % per_inserter;
            int k = NUM_INITIAL + 1000 + ti * per_inserter + local;
            assert(t.search(Key(k)).has_value());
        }

        auto all = t.getAllKeyRIDPairs();
        for (size_t i = 1; i < all.size(); ++i) {
            assert(all[i - 1].first < all[i].first);
        }
    } END_TEST;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Key ===\n";         test_key();
    std::cout << "=== Schema ===\n";      test_schema();
    std::cout << "=== Record ===\n";      test_record();
    std::cout << "=== Page ===\n";        test_page();
    std::cout << "=== BPlusTreeNode ===\n"; test_node();
    std::cout << "=== BPlusTree ===\n";    test_bplustree();
    std::cout << "=== CatalogManager ===\n";
    test_catalog_basic(); test_catalog_persist(); test_catalog_dup(); test_catalog_missing();
    std::cout << "=== BufferPool ===\n";
    test_bp_fetch_unpin(); test_bp_write_readback(); test_bp_eviction(); test_bp_dirty_flush(); test_bp_sequential_ids();
    test_bp_wal_logs_mutations();

    std::cout << "=== PageManager ===\n";
    test_pm_alloc(); test_pm_readback(); test_pm_multi();
    std::cout << "=== RecordManager ===\n";
    test_rm_basic(); test_rm_multi();
    std::cout << "=== BPlusTree Persistence ===\n";
    test_bpt_persist_20(); test_bpt_persist_update(); test_bpt_persist_remove(); test_bpt_persist_empty();
    std::cout << "=== IndexManager ===\n";
    test_indexmanager_load_unsaved_node();
    test_indexmanager_incremental_writes();
    test_indexmanager_saves_new_nodes_on_split();

    std::cout << "=== WAL ===\n";
    test_wal();

    std::cout << "=== Recovery ===\n";
    test_recovery_heap_redo();
    test_recovery_heap_undo();
    test_recovery_index_redo_and_undo();
    test_recovery_combined_heap_and_index();
    test_recovery_empty_wal_is_a_noop();
    test_recovery_end_to_end_via_real_bufferpool();

    std::cout << "=== Table ===\n";
    test_table_basic(); test_table_delete(); test_table_insert_mismatch();

    std::cout << "=== Concurrent BPlusTree (Phase 3) ===\n";
    test_concurrent_disjoint_inserts();
    test_concurrent_mixed_insert_search();
    test_concurrent_rangescan_during_inserts();
    test_concurrent_insert_remove();

    int total = passed + failed;
    std::cout << "\n=== " << passed << "/" << total << " passed";
    if (failed) std::cout << " (" << failed << " FAILED)";
    std::cout << " ===\n";
    return failed ? 1 : 0;
}
