#include <iostream>
#include <cassert>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <set>
#include <filesystem>
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
#include "lockmanager.hpp"
#include "mvcc.hpp"
#include "database.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "executor.hpp"
#include "sqlserver.hpp"

static int passed = 0;
static int failed = 0;

#define TEST(name) \
    do { \
        std::cout << "  " << name << "... " << std::flush; \
        try {

#define END_TEST \
            std::cout << "PASS\n" << std::flush; passed++; \
        } catch (const std::exception& e) { \
            std::cout << "FAIL (" << e.what() << ")\n" << std::flush; failed++; \
        } catch (...) { \
            std::cout << "FAIL (unknown)\n" << std::flush; failed++; \
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
    TEST("MVCC header defaults to untracked/no-prev-version when unset") {
        Record r(std::vector<std::string>{"a"});
        assert(r.getCreateTxnId() == 0);
        assert(r.getDeleteTxnId() == 0);
        assert(!r.hasPrevVersion());
    } END_TEST;
    TEST("MVCC header round-trips through serialize/deserialize") {
        Record r(std::vector<std::string>{"a", "b"}, 7, RID{3, 2});
        r.setDeleteTxnId(9);
        Record restored = Record::deserialize(r.serialize());
        assert(restored.getCreateTxnId() == 7);
        assert(restored.getDeleteTxnId() == 9);
        assert(restored.hasPrevVersion());
        assert(restored.getPrevVersion() == (RID{3, 2}));
        assert(restored.getFields()[1] == "b");
    } END_TEST;
    TEST("delete_txn_id sits at a fixed offset patchable in place") {
        Record r(std::vector<std::string>{"a"}, 1);
        auto bytes = r.serialize();
        uint64_t new_delete_txn_id = 5;
        std::memcpy(bytes.data() + Record::DELETE_TXN_ID_OFFSET, &new_delete_txn_id, sizeof(new_delete_txn_id));
        Record restored = Record::deserialize(bytes);
        assert(restored.getCreateTxnId() == 1);   // untouched
        assert(restored.getDeleteTxnId() == 5);   // patched in place
        assert(restored.getFields()[0] == "a");   // body untouched
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
    TEST("double delete returns false instead of throwing") {
        // Was a bug: this used to throw std::logic_error on the second
        // call, with a dead `return false;` right after the unconditional
        // throw. A no-op false is the correct, callable-in-a-loop result.
        Page p(0);
        int slot = p.insertRecord({'x'});
        assert(p.deleteRecord(slot));
        assert(!p.deleteRecord(slot));
    } END_TEST;
    TEST("patchBytes overwrites in place without touching slot length") {
        Page p(0);
        int slot = p.insertRecord({'a', 'b', 'c', 'd'});
        p.patchBytes(slot, 1, {'X', 'Y'});
        auto r = p.readRecord(slot);
        assert(r.size() == 4);
        assert(r[0] == 'a' && r[1] == 'X' && r[2] == 'Y' && r[3] == 'd');
    } END_TEST;
    TEST("patchBytes rejects a write that would overrun the slot") {
        Page p(0);
        int slot = p.insertRecord({'a', 'b'});
        bool caught = false;
        try { p.patchBytes(slot, 1, {'X', 'Y'}); } catch (const std::out_of_range&) { caught = true; }
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
    const std::string data_file = "tbl_basic.db";
    const std::string index_file = "tbl_basic.idx";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    WALWriter wal(data_file + ".wal"); TransactionManager txns(wal);
    TEST("insert and get by key") {
        PageManager pm(data_file, wal, txns); RecordManager rm(pm);
        IndexManager im(index_file, wal, txns);
        MVCCManager mvcc(txns);
        Table t("users", Schema(std::vector<Column>{{"id","int"},{"name","string"}}), pm, rm, im, mvcc);
        t.insert({"1","Alice"}); t.insert({"2","Bob"});
        auto r = t.getByKey("1");
        assert(r.has_value() && r->getFields()[1] == "Alice");
    } END_TEST;
    TEST("get nonexistent key") {
        PageManager pm(data_file, wal, txns); RecordManager rm(pm);
        IndexManager im(index_file, wal, txns);
        MVCCManager mvcc(txns);
        Table t("x", Schema(std::vector<Column>{{"id","int"}}), pm, rm, im, mvcc);
        assert(!t.getByKey("99").has_value());
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
}

static void test_table_delete() {
    const std::string data_file = "tbl_del.db";
    const std::string index_file = "tbl_del.idx";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    WALWriter wal(data_file + ".wal"); TransactionManager txns(wal);
    TEST("delete by key") {
        PageManager pm(data_file, wal, txns); RecordManager rm(pm);
        IndexManager im(index_file, wal, txns);
        MVCCManager mvcc(txns);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm, im, mvcc);
        t.insert({"1"}); assert(t.deleteByKey("1"));
        assert(!t.getByKey("1").has_value());
    } END_TEST;
    TEST("delete nonexistent") {
        PageManager pm(data_file, wal, txns); RecordManager rm(pm);
        IndexManager im(index_file, wal, txns);
        MVCCManager mvcc(txns);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm, im, mvcc);
        assert(!t.deleteByKey("99"));
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
}

static void test_table_insert_mismatch() {
    const std::string data_file = "tbl_mismatch.db";
    const std::string index_file = "tbl_mismatch.idx";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    TEST("insert mismatched columns throws") {
        WALWriter wal(data_file + ".wal"); TransactionManager txns(wal);
        PageManager pm(data_file, wal, txns); RecordManager rm(pm);
        IndexManager im(index_file, wal, txns);
        MVCCManager mvcc(txns);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm, im, mvcc);
        bool caught = false;
        try { t.insert({"1","extra"}); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
}

static void test_table_restart() {
    const std::string data_file = "tbl_restart.db";
    const std::string index_file = "tbl_restart.idx";
    const std::string wal_file = "tbl_restart.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});
    TEST("Table's index survives a process restart (real IndexManager, not the disconnected in-memory default)") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            t.insert({"1","Alice"});
            t.insert({"2","Bob"});
        } // everything destructed -- simulates the process exiting cleanly
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto r = t.getByKey("2");
            assert(r.has_value() && r->getFields()[1] == "Bob");
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

// ─── Database (Phase 6 Session 1: multi-table storage) ───────────────────────

static void test_database_two_tables_survive_restart() {
    const std::string dir = "db_multitable_restart";
    std::filesystem::remove_all(dir);

    TEST("two tables, each with their own heap+index files, both survive a process restart") {
        {
            Database db(dir);
            db.createTable("users", Schema(std::vector<Column>{{"id","int"},{"name","string"}}));
            db.createTable("products", Schema(std::vector<Column>{{"sku","string"},{"price","int"}}));
            db.getTable("users").insert({"1", "Alice"});
            db.getTable("users").insert({"2", "Bob"});
            db.getTable("products").insert({"a1", "100"});
        }  // Database destructed -- simulates a clean process exit
        {
            Database db(dir);
            assert(db.hasTable("users") && db.hasTable("products"));

            auto alice = db.getTable("users").getByKey("1");
            assert(alice.has_value() && alice->getFields()[1] == "Alice");
            auto bob = db.getTable("users").getByKey("2");
            assert(bob.has_value() && bob->getFields()[1] == "Bob");

            auto sku = db.getTable("products").getByKey("a1");
            assert(sku.has_value() && sku->getFields()[1] == "100");
        }
    } END_TEST;

    std::filesystem::remove_all(dir);
}

static void test_database_crash_spanning_two_tables_undone_atomically() {
    const std::string dir = "db_multitable_crash";
    std::filesystem::remove_all(dir);

    TEST("a crash mid-transaction that touched two different tables loses both tables' writes atomically") {
        {
            Database db(dir);
            db.createTable("a", Schema(std::vector<Column>{{"id", "int"}}));
            db.createTable("b", Schema(std::vector<Column>{{"id", "int"}}));
        }
        {
            Database db(dir);
            uint64_t txn = db.beginTxn();
            db.getTable("a").insert({"1"}, txn);
            db.getTable("b").insert({"1"}, txn);
            // No commitTxn -- simulates a crash with one transaction's
            // writes in flight across two different tables' heaps and
            // indexes, all sharing this one Database's WAL.
        }
        {
            // Database's constructor runs RecoveryManager itself before
            // this scope's own reads -- this is the actual test.
            Database db(dir);
            assert(!db.getTable("a").getByKey("1").has_value());
            assert(!db.getTable("b").getByKey("1").has_value());

            // Prove the WAL/txn_id space really was shared (not two
            // coincidentally-successful independent recoveries): commit a
            // transaction touching both tables afterward and confirm it
            // takes effect normally.
            uint64_t txn2 = db.beginTxn();
            db.getTable("a").insert({"1"}, txn2);
            db.getTable("b").insert({"1"}, txn2);
            db.commitTxn(txn2);
            assert(db.getTable("a").getByKey("1").has_value());
            assert(db.getTable("b").getByKey("1").has_value());
        }
    } END_TEST;

    std::filesystem::remove_all(dir);
}

static void test_recovery_multitable_routing() {
    const std::string wal_file = "recovery_multitable.wal";
    const std::string a_heap = "recovery_multitable_a.heap", a_idx = "recovery_multitable_a.idx";
    const std::string b_heap = "recovery_multitable_b.heap", b_idx = "recovery_multitable_b.idx";
    for (const std::string& f : {wal_file, a_heap, a_idx, b_heap, b_idx}) std::remove(f.c_str());
    Schema schema(std::vector<Column>{{"id", "int"}});

    TEST("RecoveryManager's undo pass on table A's WAL records never touches table B's files") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm_a(a_heap, wal, txns, /*table_id=*/0); RecordManager rm_a(pm_a);
            IndexManager im_a(a_idx, wal, txns, /*table_id=*/0);
            PageManager pm_b(b_heap, wal, txns, /*table_id=*/1); RecordManager rm_b(pm_b);
            IndexManager im_b(b_idx, wal, txns, /*table_id=*/1);
            MVCCManager mvcc(txns);
            Table ta("a", schema, pm_a, rm_a, im_a, mvcc);
            Table tb("b", schema, pm_b, rm_b, im_b, mvcc);

            tb.insert({"1"});  // auto-commit -- should survive recovery untouched
        }
        {
            // Table A gets an uncommitted write left dangling -- table B
            // is never even opened in this scope.
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm_a(a_heap, wal, txns, /*table_id=*/0); RecordManager rm_a(pm_a);
            IndexManager im_a(a_idx, wal, txns, /*table_id=*/0);
            MVCCManager mvcc(txns);
            Table ta("a", schema, pm_a, rm_a, im_a, mvcc);
            uint64_t txn = ta.beginTxn();
            ta.insert({"1"}, txn);
            // no commit
        }
        {
            WALWriter wal(wal_file);
            std::unordered_map<uint32_t, RecoveryManager::TableFiles> tables{
                {0u, RecoveryManager::TableFiles{a_heap, a_idx}},
                {1u, RecoveryManager::TableFiles{b_heap, b_idx}},
            };
            RecoveryManager recovery(wal, tables);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm_a(a_heap, wal, txns, /*table_id=*/0); RecordManager rm_a(pm_a);
            IndexManager im_a(a_idx, wal, txns, /*table_id=*/0);
            PageManager pm_b(b_heap, wal, txns, /*table_id=*/1); RecordManager rm_b(pm_b);
            IndexManager im_b(b_idx, wal, txns, /*table_id=*/1);
            MVCCManager mvcc(txns);
            Table ta("a", schema, pm_a, rm_a, im_a, mvcc);
            Table tb("b", schema, pm_b, rm_b, im_b, mvcc);

            assert(!ta.getByKey("1").has_value());  // table A's uncommitted insert was undone
            auto b1 = tb.getByKey("1");              // table B's committed row is untouched
            assert(b1.has_value() && b1->getFields()[0] == "1");
        }
    } END_TEST;

    for (const std::string& f : {wal_file, a_heap, a_idx, b_heap, b_idx}) std::remove(f.c_str());
}

static void test_database() {
    test_database_two_tables_survive_restart();
    test_database_crash_spanning_two_tables_undone_atomically();
    test_recovery_multitable_routing();
}

// ─── SQL Lexer + Parser (Phase 6 Session 2) ───────────────────────────────────

static std::vector<Token> lex(const std::string& sql) {
    Lexer lexer(sql);
    return lexer.tokenize();
}

static void test_sql_lexer() {
    TEST("keywords, identifiers, punctuation tokenize correctly and case-insensitively") {
        auto tokens = lex("select * From users;");
        assert(tokens.size() == 6);  // 5 tokens + END_OF_INPUT
        assert(tokens[0].type == SqlTokenType::KEYWORD_SELECT);
        assert(tokens[1].type == SqlTokenType::STAR);
        assert(tokens[2].type == SqlTokenType::KEYWORD_FROM);
        assert(tokens[3].type == SqlTokenType::IDENTIFIER && tokens[3].text == "users");
        assert(tokens[4].type == SqlTokenType::SEMICOLON);
        assert(tokens[5].type == SqlTokenType::END_OF_INPUT);
    } END_TEST;

    TEST("identifier text preserves original case, unlike keywords") {
        auto tokens = lex("SELECT UserName FROM t;");
        assert(tokens[1].type == SqlTokenType::IDENTIFIER && tokens[1].text == "UserName");
    } END_TEST;

    TEST("string literal, including an escaped '' quote") {
        auto tokens = lex("'it''s here'");
        assert(tokens[0].type == SqlTokenType::STRING_LITERAL);
        assert(tokens[0].text == "it's here");
    } END_TEST;

    TEST("number literal, including a negative one") {
        auto tokens = lex("42 -7");
        assert(tokens[0].type == SqlTokenType::NUMBER_LITERAL && tokens[0].text == "42");
        assert(tokens[1].type == SqlTokenType::NUMBER_LITERAL && tokens[1].text == "-7");
    } END_TEST;

    TEST("all comparison operators, including <> as an alias for !=") {
        auto tokens = lex("= != < <= > >= <>");
        std::vector<SqlTokenType> expected = {
            SqlTokenType::OP_EQ, SqlTokenType::OP_NEQ, SqlTokenType::OP_LT, SqlTokenType::OP_LTE,
            SqlTokenType::OP_GT, SqlTokenType::OP_GTE, SqlTokenType::OP_NEQ,
        };
        for (std::size_t i = 0; i < expected.size(); i++) assert(tokens[i].type == expected[i]);
    } END_TEST;

    TEST("a `-- ...` line comment is skipped entirely") {
        auto tokens = lex("SELECT 1; -- trailing comment\n");
        assert(tokens.back().type == SqlTokenType::END_OF_INPUT);
        // no stray tokens from the comment text itself
        for (auto& t : tokens) assert(t.text.find("trailing") == std::string::npos);
    } END_TEST;

    TEST("unterminated string literal throws LexError, not a crash") {
        bool caught = false;
        try { lex("'never closed"); } catch (const LexError&) { caught = true; }
        assert(caught);
    } END_TEST;

    TEST("a lone '!' not followed by '=' throws LexError") {
        bool caught = false;
        try { lex("!"); } catch (const LexError&) { caught = true; }
        assert(caught);
    } END_TEST;

    TEST("an unrecognized character throws LexError") {
        bool caught = false;
        try { lex("@"); } catch (const LexError&) { caught = true; }
        assert(caught);
    } END_TEST;
}

static Stmt parseOne(const std::string& sql) {
    Parser parser(lex(sql));
    return parser.parseStatement();
}

static void test_sql_parser() {
    TEST("CREATE TABLE with an inert PRIMARY KEY marker on the first column") {
        auto stmt = parseOne("CREATE TABLE users (id int PRIMARY KEY, name string);");
        auto& create = std::get<CreateTableStmt>(stmt);
        assert(create.table_name == "users");
        assert(create.columns.size() == 2);
        assert(create.columns[0].name == "id" && create.columns[0].type == "int");
        assert(create.columns[0].primary_key);
        assert(create.columns[1].name == "name" && create.columns[1].type == "string");
        assert(!create.columns[1].primary_key);
    } END_TEST;

    TEST("PRIMARY KEY on a column other than the first is a parse error") {
        bool caught = false;
        try { parseOne("CREATE TABLE t (a int, b int PRIMARY KEY);"); }
        catch (const ParseError&) { caught = true; }
        assert(caught);
    } END_TEST;

    TEST("INSERT INTO ... VALUES parses positional literals in order") {
        auto stmt = parseOne("INSERT INTO users VALUES ('1', 'Alice');");
        auto& insert = std::get<InsertStmt>(stmt);
        assert(insert.table_name == "users");
        assert(insert.values == std::vector<std::string>({"1", "Alice"}));
    } END_TEST;

    TEST("SELECT * FROM parses to an empty (meaning 'all columns') projection list") {
        auto stmt = parseOne("SELECT * FROM users;");
        auto& select = std::get<SelectStmt>(stmt);
        assert(select.table_name == "users");
        assert(select.columns.empty());
        assert(!select.where.has_value());
    } END_TEST;

    TEST("SELECT with an explicit column list and a typed WHERE condition") {
        auto stmt = parseOne("SELECT id, name FROM users WHERE id = 5;");
        auto& select = std::get<SelectStmt>(stmt);
        assert(select.columns == std::vector<std::string>({"id", "name"}));
        assert(select.where.has_value());
        assert(select.where->column == "id");
        assert(select.where->op == ComparisonOp::EQ);
        assert(select.where->literal == "5");
    } END_TEST;

    TEST("DELETE FROM ... WHERE with a string literal and a non-equality operator") {
        auto stmt = parseOne("DELETE FROM users WHERE name != 'Bob';");
        auto& del = std::get<DeleteStmt>(stmt);
        assert(del.table_name == "users");
        assert(del.where.has_value());
        assert(del.where->op == ComparisonOp::NEQ);
        assert(del.where->literal == "Bob");
    } END_TEST;

    TEST("UPDATE ... SET with multiple assignments and a WHERE clause") {
        auto stmt = parseOne("UPDATE users SET name = 'Alice2', age = 31 WHERE id = '1';");
        auto& update = std::get<UpdateStmt>(stmt);
        assert(update.table_name == "users");
        assert(update.assignments.size() == 2);
        assert(update.assignments[0].column == "name" && update.assignments[0].value == "Alice2");
        assert(update.assignments[1].column == "age" && update.assignments[1].value == "31");
        assert(update.where.has_value() && update.where->column == "id");
    } END_TEST;

    TEST("BEGIN / COMMIT / ROLLBACK parse to their own statement types") {
        assert(std::holds_alternative<BeginStmt>(parseOne("BEGIN;")));
        assert(std::holds_alternative<CommitStmt>(parseOne("COMMIT;")));
        assert(std::holds_alternative<RollbackStmt>(parseOne("ROLLBACK;")));
    } END_TEST;

    TEST("a statement missing its trailing ';' is a clean parse error, not a crash") {
        bool caught = false;
        try { parseOne("SELECT * FROM users"); } catch (const ParseError&) { caught = true; }
        assert(caught);
    } END_TEST;

    TEST("unbalanced parentheses in INSERT VALUES is a clean parse error") {
        bool caught = false;
        try { parseOne("INSERT INTO t VALUES ('1', '2';"); } catch (const ParseError&) { caught = true; }
        assert(caught);
    } END_TEST;

    TEST("a missing FROM in SELECT is a clean parse error") {
        bool caught = false;
        try { parseOne("SELECT * users;"); } catch (const ParseError&) { caught = true; }
        assert(caught);
    } END_TEST;
}

// ─── SQL Executor (Phase 6 Session 3) ─────────────────────────────────────────

// Lexes, parses, and executes one statement in one call -- the tests
// below only care about the resulting QueryResult, not the intermediate
// token stream or AST (those are covered separately above).
static QueryResult run(Database& db, uint64_t& txn, const std::string& sql) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    Stmt stmt = parser.parseStatement();
    return execute(stmt, db, txn);
}

static void test_sql_executor_crud() {
    const std::string dir = "sql_executor_crud";
    std::filesystem::remove_all(dir);

    TEST("CREATE TABLE, INSERT, SELECT (both '*' and a projection, both PK and scan WHERE), UPDATE, DELETE all work end to end") {
        Database db(dir);
        uint64_t txn = 0;

        auto created = run(db, txn, "CREATE TABLE users (id string PRIMARY KEY, name string, age int);");
        assert(created.ok);

        assert(run(db, txn, "INSERT INTO users VALUES ('1', 'Alice', 30);").ok);
        assert(run(db, txn, "INSERT INTO users VALUES ('2', 'Bob', 25);").ok);
        assert(run(db, txn, "INSERT INTO users VALUES ('3', 'Carol', 40);").ok);

        auto all = run(db, txn, "SELECT * FROM users;");
        assert(all.ok);
        assert(all.columns == std::vector<std::string>({"id", "name", "age"}));
        assert(all.rows.size() == 3);

        auto by_pk = run(db, txn, "SELECT name FROM users WHERE id = '2';");
        assert(by_pk.ok && by_pk.columns == std::vector<std::string>({"name"}));
        assert(by_pk.rows.size() == 1 && by_pk.rows[0][0] == "Bob");

        auto by_scan = run(db, txn, "SELECT id FROM users WHERE name = 'Carol';");
        assert(by_scan.ok && by_scan.rows.size() == 1 && by_scan.rows[0][0] == "3");

        auto upd = run(db, txn, "UPDATE users SET age = 26 WHERE id = '2';");
        assert(upd.ok && upd.affected_rows == 1);
        auto after_upd = run(db, txn, "SELECT age FROM users WHERE id = '2';");
        assert(after_upd.rows[0][0] == "26");

        auto del = run(db, txn, "DELETE FROM users WHERE id = '1';");
        assert(del.ok && del.affected_rows == 1);
        auto after_del = run(db, txn, "SELECT * FROM users;");
        assert(after_del.rows.size() == 2);
    } END_TEST;

    std::filesystem::remove_all(dir);
}

static void test_sql_executor_typed_where() {
    const std::string dir = "sql_executor_typed_where";
    std::filesystem::remove_all(dir);

    TEST("a scanned WHERE on an int column compares numerically, not lexicographically") {
        Database db(dir);
        uint64_t txn = 0;
        run(db, txn, "CREATE TABLE t (id string PRIMARY KEY, age int);");
        run(db, txn, "INSERT INTO t VALUES ('a', 9);");
        run(db, txn, "INSERT INTO t VALUES ('b', 10);");

        // Lexicographically, "10" < "9" (since '1' < '9'), so a naive
        // string compare of `age > 9` would wrongly return nothing.
        // Numerically, only age=10 qualifies.
        auto r = run(db, txn, "SELECT id FROM t WHERE age > 9;");
        assert(r.ok && r.rows.size() == 1 && r.rows[0][0] == "b");
    } END_TEST;

    std::filesystem::remove_all(dir);
}

static void test_sql_executor_transactions() {
    const std::string dir = "sql_executor_txn";
    std::filesystem::remove_all(dir);

    TEST("BEGIN groups multiple statements; ROLLBACK undoes all of them, COMMIT keeps all of them") {
        Database db(dir);
        uint64_t txn = 0;
        run(db, txn, "CREATE TABLE t (id string PRIMARY KEY);");

        assert(run(db, txn, "BEGIN;").ok);
        assert(txn != 0);
        run(db, txn, "INSERT INTO t VALUES ('1');");
        run(db, txn, "INSERT INTO t VALUES ('2');");
        assert(run(db, txn, "ROLLBACK;").ok);
        assert(txn == 0);
        assert(run(db, txn, "SELECT * FROM t;").rows.empty());

        assert(run(db, txn, "BEGIN;").ok);
        run(db, txn, "INSERT INTO t VALUES ('1');");
        run(db, txn, "INSERT INTO t VALUES ('2');");
        assert(run(db, txn, "COMMIT;").ok);
        assert(txn == 0);
        assert(run(db, txn, "SELECT * FROM t;").rows.size() == 2);
    } END_TEST;

    TEST("COMMIT/ROLLBACK with nothing open, or a nested BEGIN, are clean protocol errors") {
        Database db(dir + "_protocol");
        uint64_t txn = 0;
        assert(!run(db, txn, "COMMIT;").ok);
        assert(!run(db, txn, "ROLLBACK;").ok);

        assert(run(db, txn, "BEGIN;").ok);
        assert(!run(db, txn, "BEGIN;").ok);  // already one open
        assert(run(db, txn, "ROLLBACK;").ok);  // cleanup
    } END_TEST;

    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir + "_protocol");
}

// ─── SQL Server (Phase 6 Session 4) ───────────────────────────────────────────

static SOCKET sqlConnect(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(rc == 0);
    return s;
}

// Accumulates whatever the server sends back until a short idle window
// passes with nothing more arriving -- the wire protocol has no
// length-prefix framing, so this is how a test client (which, unlike the
// server, knows exactly how many statements it just sent) tells "the
// full response landed" apart from "still arriving". Waits indefinitely
// (up to a long safety ceiling) for the *first* byte -- a statement
// blocked on a row lock can legitimately take a while before any
// response starts arriving at all, which is exactly what the write-write
// conflict test below deliberately exercises -- then switches to a short
// idle timeout once bytes are flowing, to detect the response's end.
static std::string sqlRecvResponse(SOCKET s) {
    std::string result;
    char buf[4096];
    bool got_any = false;
    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        timeval tv = got_any ? timeval{0, 50000} : timeval{30, 0};  // 50ms once flowing, else 30s ceiling
        int rc = select(0, &readfds, nullptr, nullptr, &tv);
        if (rc <= 0) break;
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        result.append(buf, n);
        got_any = true;
    }
    return result;
}

static std::string sqlSendAndRecv(SOCKET s, const std::string& sql) {
    send(s, sql.data(), static_cast<int>(sql.size()), 0);
    return sqlRecvResponse(s);
}

static void test_sql_server_basic() {
    const std::string dir = "sql_server_basic";
    std::filesystem::remove_all(dir);
    const uint16_t port = 15501;
    // Database and SqlServer are scoped to this block so both are fully
    // destructed (closing every open file handle) before the trailing
    // remove_all() below runs -- removing the directory while Database's
    // PageManagers still have files open fails on Windows.
    {
    Database db(dir);
    SqlServer server(db, port);
    server.start();

    TEST("CREATE/INSERT/SELECT/UPDATE/DELETE over a real TCP connection get well-formed responses") {
        SOCKET client = sqlConnect(port);

        assert(sqlSendAndRecv(client, "CREATE TABLE users (id string PRIMARY KEY, name string);") ==
               "OK\n");
        assert(sqlSendAndRecv(client, "INSERT INTO users VALUES ('1', 'Alice');") ==
               "OK 1 rows affected\n");
        assert(sqlSendAndRecv(client, "INSERT INTO users VALUES ('2', 'Bob');") ==
               "OK 1 rows affected\n");

        assert(sqlSendAndRecv(client, "SELECT * FROM users;") == "id|name\n1|Alice\n2|Bob\nOK 2 rows\n");

        assert(sqlSendAndRecv(client, "UPDATE users SET name = 'Alice2' WHERE id = '1';") ==
               "OK 1 rows affected\n");
        assert(sqlSendAndRecv(client, "DELETE FROM users WHERE id = '2';") ==
               "OK 1 rows affected\n");
        assert(sqlSendAndRecv(client, "SELECT name FROM users WHERE id = '1';") ==
               "name\nAlice2\nOK 1 rows\n");

        std::string err = sqlSendAndRecv(client, "SELECT * FROM nonexistent;");
        assert(err.rfind("ERROR:", 0) == 0);

        closesocket(client);
    } END_TEST;

    TEST("multiple ';'-terminated statements sent in one packet each get their own response, in order") {
        SOCKET client = sqlConnect(port);
        std::string batch =
            "CREATE TABLE t (id string PRIMARY KEY);"
            "INSERT INTO t VALUES ('x');"
            "SELECT * FROM t;";
        assert(sqlSendAndRecv(client, batch) == "OK\nOK 1 rows affected\nid\nx\nOK 1 rows\n");
        closesocket(client);
    } END_TEST;

    TEST("BEGIN/COMMIT over a real connection groups statements atomically, same as the Table API") {
        SOCKET client = sqlConnect(port);
        sqlSendAndRecv(client, "CREATE TABLE t2 (id string PRIMARY KEY);");

        assert(sqlSendAndRecv(client, "BEGIN;") == "OK\n");
        sqlSendAndRecv(client, "INSERT INTO t2 VALUES ('a');");
        assert(sqlSendAndRecv(client, "COMMIT;") == "OK\n");
        assert(sqlSendAndRecv(client, "SELECT * FROM t2;") == "id\na\nOK 1 rows\n");

        closesocket(client);
    } END_TEST;

    server.stop();
    }  // db, server destructed here -- file handles closed
    std::filesystem::remove_all(dir);
}

// ─── SQL Server integration + crash recovery (Phase 6 Session 5) ─────────────

static void test_sql_server_write_write_conflict() {
    const std::string dir = "sql_server_conflict";
    std::filesystem::remove_all(dir);
    const uint16_t port = 15502;
    {
    Database db(dir);
    SqlServer server(db, port);
    server.start();

    TEST("a write-write conflict between two real SQL connections blocks the second until the first commits, then it chains onto the committed result") {
        SOCKET a = sqlConnect(port);
        SOCKET b = sqlConnect(port);

        assert(sqlSendAndRecv(a, "CREATE TABLE t (id string PRIMARY KEY, val string);") == "OK\n");
        assert(sqlSendAndRecv(a, "INSERT INTO t VALUES ('1', 'orig');") == "OK 1 rows affected\n");

        assert(sqlSendAndRecv(a, "BEGIN;") == "OK\n");
        assert(sqlSendAndRecv(a, "UPDATE t SET val = 'FromA' WHERE id = '1';") ==
               "OK 1 rows affected\n");
        // Connection A now holds the row lock, uncommitted.

        std::atomic<bool> b_done{false};
        std::string b_response;
        std::thread writer_b([&] {
            b_response = sqlSendAndRecv(b, "UPDATE t SET val = 'FromB' WHERE id = '1';");
            b_done = true;
        });

        // Give B's request time to actually be received by its
        // connection thread and block on the row lock, not just "not yet
        // sent" -- same discipline test_mvcc_lock_integration uses at the
        // Table API level, just with sockets in between now.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(!b_done);  // genuinely blocked on A, not racing a stale read

        assert(sqlSendAndRecv(a, "COMMIT;") == "OK\n");
        writer_b.join();
        assert(b_done);
        assert(b_response == "OK 1 rows affected\n");

        assert(sqlSendAndRecv(a, "SELECT val FROM t WHERE id = '1';") == "val\nFromB\nOK 1 rows\n");

        closesocket(a);
        closesocket(b);
    } END_TEST;

    server.stop();
    }
    std::filesystem::remove_all(dir);
}

static void test_sql_server_repeatable_read() {
    const std::string dir = "sql_server_repeatable_read";
    std::filesystem::remove_all(dir);
    const uint16_t port = 15503;
    {
    Database db(dir);
    SqlServer server(db, port);
    server.start();

    TEST("a BEGIN-held connection's snapshot sees no phantoms from a concurrent connection's committed insert, until it commits and re-queries") {
        SOCKET a = sqlConnect(port);
        SOCKET b = sqlConnect(port);

        assert(sqlSendAndRecv(a, "CREATE TABLE t (id string PRIMARY KEY, val string);") == "OK\n");
        assert(sqlSendAndRecv(a, "INSERT INTO t VALUES ('1', 'x');") == "OK 1 rows affected\n");

        assert(sqlSendAndRecv(a, "BEGIN;") == "OK\n");
        assert(sqlSendAndRecv(a, "SELECT * FROM t;") == "id|val\n1|x\nOK 1 rows\n");  // takes A's snapshot

        // A separate, auto-commit connection inserts and commits a new
        // row entirely outside A's still-open transaction.
        assert(sqlSendAndRecv(b, "INSERT INTO t VALUES ('2', 'y');") == "OK 1 rows affected\n");

        // Still within A's original transaction: repeatable read means
        // the new row must stay invisible even though it's since
        // committed -- A's snapshot never moves.
        assert(sqlSendAndRecv(a, "SELECT * FROM t;") == "id|val\n1|x\nOK 1 rows\n");

        assert(sqlSendAndRecv(a, "COMMIT;") == "OK\n");
        // A fresh (auto-commit) statement on the same connection takes a
        // brand-new snapshot and does see it.
        assert(sqlSendAndRecv(a, "SELECT * FROM t;") == "id|val\n1|x\n2|y\nOK 2 rows\n");

        closesocket(a);
        closesocket(b);
    } END_TEST;

    server.stop();
    }
    std::filesystem::remove_all(dir);
}

static void test_sql_crash_recovery() {
    const std::string dir = "sql_crash_recovery";
    std::filesystem::remove_all(dir);

    TEST("CREATE TABLE + data on two tables, all issued via SQL, survives a process restart") {
        {
            Database db(dir);
            uint64_t txn = 0;
            assert(run(db, txn, "CREATE TABLE users (id string PRIMARY KEY, name string);").ok);
            assert(run(db, txn, "CREATE TABLE products (sku string PRIMARY KEY, price int);").ok);
            assert(run(db, txn, "INSERT INTO users VALUES ('1', 'Alice');").ok);
            assert(run(db, txn, "INSERT INTO products VALUES ('a1', 100);").ok);
        }  // Database destructed -- simulates a clean process exit
        {
            Database db(dir);  // recovery runs here, in the constructor
            uint64_t txn = 0;
            auto users = run(db, txn, "SELECT * FROM users;");
            assert(users.ok && users.rows.size() == 1 && users.rows[0][1] == "Alice");
            auto products = run(db, txn, "SELECT * FROM products;");
            assert(products.ok && products.rows.size() == 1 && products.rows[0][1] == "100");
        }
    } END_TEST;

    std::filesystem::remove_all(dir);
}

static void test_sql() {
    test_sql_lexer();
    test_sql_parser();
    test_sql_executor_crud();
    test_sql_executor_typed_where();
    test_sql_executor_transactions();
    test_sql_server_basic();
    test_sql_server_write_write_conflict();
    test_sql_server_repeatable_read();
    test_sql_crash_recovery();
}

// ─── MVCC (Phase 5) ────────────────────────────────────────────────────────

static void test_mvcc_txn_grouping() {
    const std::string data_file = "mvcc_grouping.db";
    const std::string index_file = "mvcc_grouping.idx";
    const std::string wal_file = "mvcc_grouping.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});
    TEST("a multi-statement transaction's heap+index writes are undone as one atomic unit on crash") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);

            // Two Table::insert calls (each touching both the heap page
            // and the index) sharing ONE caller-owned txn_id -- before
            // Session 2's txn_id threading, these would have been two
            // independent auto-commit transactions with no way to undo
            // them together.
            uint64_t txn = t.beginTxn();
            t.insert({"1", "Alice"}, txn);
            t.insert({"2", "Bob"}, txn);
            // Deliberately no t.commitTxn(txn) -- simulates a crash before
            // commit. Scope exit still flushes dirty pages to disk (the
            // WAL's "steal" policy allows that; it's what makes the undo
            // pass necessary in the first place), so without recovery's
            // undo pass this data would incorrectly survive.
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            assert(!t.getByKey("1").has_value());
            assert(!t.getByKey("2").has_value());
        }
    } END_TEST;
    TEST("a committed multi-statement transaction's writes all survive") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);

            uint64_t txn = t.beginTxn();
            t.insert({"3", "Carol"}, txn);
            t.insert({"4", "Dave"}, txn);
            t.commitTxn(txn);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto a = t.getByKey("3");
            auto b = t.getByKey("4");
            assert(a.has_value() && a->getFields()[1] == "Carol");
            assert(b.has_value() && b->getFields()[1] == "Dave");
        }
    } END_TEST;
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_mvcc_visibility() {
    const std::string data_file = "mvcc_vis.db";
    const std::string index_file = "mvcc_vis.idx";
    const std::string wal_file = "mvcc_vis.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});
    WALWriter wal(wal_file); TransactionManager txns(wal);
    PageManager pm(data_file, wal, txns); RecordManager rm(pm);
    IndexManager im(index_file, wal, txns);
    MVCCManager mvcc(txns);
    Table t("users", schema, pm, rm, im, mvcc);

    TEST("a transaction sees its own uncommitted write, but no one else does yet") {
        uint64_t txn = t.beginTxn();
        t.insert({"1", "Alice"}, txn);

        auto self_read = t.getByKey("1", txn);
        assert(self_read.has_value() && self_read->getFields()[1] == "Alice");
        assert(!t.getByKey("1").has_value());  // ad hoc read: not committed yet

        t.commitTxn(txn);
        assert(t.getByKey("1").has_value());  // committed now
    } END_TEST;

    TEST("a snapshot taken before a commit never sees it, even after that commit") {
        uint64_t reader = t.beginTxn();

        uint64_t writer = t.beginTxn();
        t.insert({"2", "Bob"}, writer);
        t.commitTxn(writer);

        assert(!t.getByKey("2", reader).has_value());
        t.commitTxn(reader);
    } END_TEST;

    TEST("a snapshot started after a commit does see it") {
        uint64_t late_reader = t.beginTxn();
        auto r = t.getByKey("2", late_reader);
        assert(r.has_value() && r->getFields()[1] == "Bob");
        t.commitTxn(late_reader);
    } END_TEST;

    TEST("an aborted transaction's writes are invisible to everyone, including snapshots started after the abort") {
        uint64_t doomed = t.beginTxn();
        t.insert({"3", "Ghost"}, doomed);
        assert(t.getByKey("3", doomed).has_value());  // visible to itself pre-abort
        t.abortTxn(doomed);

        assert(!t.getByKey("3").has_value());

        uint64_t later = t.beginTxn();
        assert(!t.getByKey("3", later).has_value());  // still invisible to a fresh snapshot
        t.commitTxn(later);
    } END_TEST;

    TEST("an aborted delete un-hides the row for later readers") {
        t.insert({"4", "Persistent"});  // auto-commit

        uint64_t doomed = t.beginTxn();
        assert(t.deleteByKey("4", doomed));
        assert(!t.getByKey("4", doomed).has_value());  // deleted from its own point of view
        t.abortTxn(doomed);

        assert(t.getByKey("4").has_value());  // delete rolled back
    } END_TEST;

    TEST("updateByKey chains a new version and repoints the index; an old snapshot still sees the pre-update value") {
        t.insert({"5", "Original"});  // auto-commit
        uint64_t reader = t.beginTxn();  // snapshot before the update

        assert(t.updateByKey("5", {"5", "Updated"}));  // auto-commit update

        auto fresh = t.getByKey("5");
        assert(fresh.has_value() && fresh->getFields()[1] == "Updated");
        auto old = t.getByKey("5", reader);
        assert(old.has_value() && old->getFields()[1] == "Original");
        t.commitTxn(reader);
    } END_TEST;

    TEST("updateByKey on a nonexistent key returns false") {
        assert(!t.updateByKey("no-such-key", {"x", "y"}));
    } END_TEST;

    TEST("updateByKey on an already-deleted key returns false") {
        t.insert({"7", "Temp"});
        assert(t.deleteByKey("7"));
        assert(!t.updateByKey("7", {"7", "Resurrected"}));
    } END_TEST;

    TEST("deleting then re-inserting the same key repoints the index instead of duplicating it") {
        t.insert({"8", "First"});
        assert(t.deleteByKey("8"));
        assert(!t.getByKey("8").has_value());

        t.insert({"8", "Second"});
        auto r = t.getByKey("8");
        assert(r.has_value() && r->getFields()[1] == "Second");
    } END_TEST;

    TEST("a snapshot predating a delete+reinsert still walks past the new version to the old one") {
        t.insert({"9", "Before"});  // auto-commit
        uint64_t reader = t.beginTxn();  // snapshot before the delete+reinsert

        assert(t.deleteByKey("9"));       // auto-commit
        t.insert({"9", "After"});         // auto-commit -- repoints the index, chained

        auto seen_by_reader = t.getByKey("9", reader);
        assert(seen_by_reader.has_value() && seen_by_reader->getFields()[1] == "Before");
        auto seen_now = t.getByKey("9");
        assert(seen_now.has_value() && seen_now->getFields()[1] == "After");
        t.commitTxn(reader);
    } END_TEST;

    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_mvcc_crash_recovery_versioning() {
    const std::string data_file = "mvcc_crash.db";
    const std::string index_file = "mvcc_crash.idx";
    const std::string wal_file = "mvcc_crash.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});

    TEST("an uncommitted update's new version is undone by recovery; the old version survives, live") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            t.insert({"1", "Original"});  // auto-commit, survives on its own
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            uint64_t txn = t.beginTxn();
            assert(t.updateByKey("1", {"1", "Uncommitted"}, txn));
            // No commitTxn -- simulates a crash mid-transaction. Both the
            // old version's delete_txn_id patch and the new version's
            // insert are WAL-logged under the same txn_id, so recovery's
            // undo pass must revert them together.
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto r = t.getByKey("1");
            assert(r.has_value() && r->getFields()[1] == "Original");
        }
    } END_TEST;

    TEST("a committed update's new version survives recovery; the old version stays correctly superseded") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            uint64_t txn = t.beginTxn();
            assert(t.updateByKey("1", {"1", "Committed"}, txn));
            t.commitTxn(txn);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto r = t.getByKey("1");
            assert(r.has_value() && r->getFields()[1] == "Committed");
        }
    } END_TEST;

    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_lockmanager_basic() {
    TEST("uncontended acquire succeeds immediately") {
        LockManager lm;
        RID r{1, 0};
        lm.acquireExclusive(r, 100);
        lm.releaseAll(100);
    } END_TEST;

    TEST("re-entrant acquire by the same transaction succeeds immediately") {
        LockManager lm;
        RID r{1, 0};
        lm.acquireExclusive(r, 100);
        lm.acquireExclusive(r, 100);  // same txn, same rid -- must not block on itself
        lm.releaseAll(100);
    } END_TEST;

    TEST("locks on different RIDs never contend") {
        LockManager lm;
        RID r1{1, 0}, r2{2, 0};
        lm.acquireExclusive(r1, 100);
        lm.acquireExclusive(r2, 200);  // different txn, different rid -- must not block
        lm.releaseAll(100);
        lm.releaseAll(200);
    } END_TEST;

    TEST("a second transaction blocks until the first releases, then acquires") {
        LockManager lm;
        RID r{1, 0};
        lm.acquireExclusive(r, 100);

        std::atomic<bool> acquired{false};
        std::thread waiter([&] {
            lm.acquireExclusive(r, 200, std::chrono::milliseconds(3000));
            acquired = true;
            lm.releaseAll(200);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(!acquired);  // still blocked -- txn 100 hasn't released yet

        lm.releaseAll(100);
        waiter.join();
        assert(acquired);
    } END_TEST;

    TEST("a wait that outlives its timeout throws LockTimeoutError instead of hanging forever") {
        LockManager lm;
        RID r{1, 0};
        lm.acquireExclusive(r, 100);  // never released within this test

        bool timed_out = false;
        try {
            lm.acquireExclusive(r, 200, std::chrono::milliseconds(150));
        } catch (const LockManager::LockTimeoutError&) {
            timed_out = true;
        }
        assert(timed_out);
        lm.releaseAll(100);
    } END_TEST;
}

static void test_lockmanager_deadlock() {
    TEST("an A-wants-B/B-wants-A cycle is broken by aborting the younger transaction, not left to hang") {
        // Repeated (not just run-once): CONCURRENCY_BUGS.md's own history
        // in this codebase is that races can be as rare as 1-in-900, so a
        // single clean pass proves little. This particular scenario is
        // actually deterministic by construction (every possible
        // interleaving of which thread detects the cycle first still
        // victimizes the same, numerically-younger txn_id -- see
        // lockmanager.cpp), but running it repeatedly is cheap and is
        // exactly what would catch it if that reasoning were wrong.
        for (int iter = 0; iter < 25; iter++) {
            LockManager lm;
            RID r1{1, 0}, r2{2, 0};
            std::atomic<bool> txn1_holds_r1{false};
            std::atomic<bool> txn2_holds_r2{false};
            std::string txn1_result, txn2_result;

            std::thread t1([&] {
                try {
                    lm.acquireExclusive(r1, 1);
                    txn1_holds_r1 = true;
                    while (!txn2_holds_r2) std::this_thread::yield();
                    lm.acquireExclusive(r2, 1, std::chrono::milliseconds(3000));
                    lm.releaseAll(1);
                    txn1_result = "ok";
                } catch (const LockManager::DeadlockError&) {
                    lm.releaseAll(1);
                    txn1_result = "deadlock";
                } catch (const std::exception& e) {
                    lm.releaseAll(1);
                    txn1_result = std::string("unexpected: ") + e.what();
                }
            });
            std::thread t2([&] {
                try {
                    lm.acquireExclusive(r2, 2);
                    txn2_holds_r2 = true;
                    while (!txn1_holds_r1) std::this_thread::yield();
                    lm.acquireExclusive(r1, 2, std::chrono::milliseconds(3000));
                    lm.releaseAll(2);
                    txn2_result = "ok";
                } catch (const LockManager::DeadlockError&) {
                    lm.releaseAll(2);
                    txn2_result = "deadlock";
                } catch (const std::exception& e) {
                    lm.releaseAll(2);
                    txn2_result = std::string("unexpected: ") + e.what();
                }
            });
            t1.join();
            t2.join();

            // The numerically younger transaction (2) is always the one
            // aborted, regardless of which thread's acquireExclusive call
            // happens to detect the cycle first.
            if (txn1_result != "ok" || txn2_result != "deadlock") {
                throw std::runtime_error("iteration " + std::to_string(iter) +
                    ": txn1=" + txn1_result + " txn2=" + txn2_result);
            }
        }
    } END_TEST;
}

static void test_mvcc_lock_integration() {
    const std::string data_file = "mvcc_lock.db";
    const std::string index_file = "mvcc_lock.idx";
    const std::string wal_file = "mvcc_lock.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});
    WALWriter wal(wal_file); TransactionManager txns(wal);
    PageManager pm(data_file, wal, txns); RecordManager rm(pm);
    IndexManager im(index_file, wal, txns);
    MVCCManager mvcc(txns);
    Table t("users", schema, pm, rm, im, mvcc);

    TEST("a concurrent writer genuinely blocks on Table's own lock, then correctly builds on the committed result") {
        t.insert({"1", "Original"});  // auto-commit

        uint64_t txn_a = t.beginTxn();
        // A now holds locks on both the row it read (the original insert)
        // and the new version this update just created -- see the
        // acquireExclusive calls in Table::insert/updateByKey. The second
        // one is what B below actually blocks on: by the time B looks
        // this key up, the index already points past A to that new
        // version.
        assert(t.updateByKey("1", {"1", "FromA"}, txn_a));

        std::atomic<bool> b_done{false};
        std::string b_outcome;
        std::thread writer_b([&] {
            uint64_t txn_b = t.beginTxn();
            bool ok = t.updateByKey("1", {"1", "FromB"}, txn_b);  // blocks until A commits/aborts
            b_outcome = ok ? "ok" : "stale";
            if (ok) t.commitTxn(txn_b); else t.abortTxn(txn_b);
            b_done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(!b_done);  // still blocked behind A's lock

        t.commitTxn(txn_a);
        writer_b.join();
        assert(b_done);
        // B was genuinely blocked ON A (not racing a stale read) and
        // wakes to find A's version still live and now committed, so it
        // correctly chains onto it and succeeds -- the write-write
        // conflict serialized instead of corrupting or silently losing
        // either write.
        assert(b_outcome == "ok");

        auto final = t.getByKey("1");
        assert(final.has_value() && final->getFields()[1] == "FromB");
    } END_TEST;

    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_mvcc_crash_recovery_mixed_ops() {
    const std::string data_file = "mvcc_crash_mixed.db";
    const std::string index_file = "mvcc_crash_mixed.idx";
    const std::string wal_file = "mvcc_crash_mixed.wal";
    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
    Schema schema(std::vector<Column>{{"id","int"},{"name","string"}});

    TEST("a crash mid-transaction mixing insert+update+delete loses all of them atomically; a committed mix survives") {
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            t.insert({"1", "Alice"});
            t.insert({"2", "Bob"});
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            uint64_t txn = t.beginTxn();
            t.insert({"3", "Carol"}, txn);
            assert(t.updateByKey("1", {"1", "AliceV2"}, txn));
            assert(t.deleteByKey("2", txn));
            // No commitTxn -- simulates a crash with all three kinds of
            // write in flight under one transaction.
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto a = t.getByKey("1");
            assert(a.has_value() && a->getFields()[1] == "Alice");  // update undone
            assert(t.getByKey("2").has_value());                    // delete undone
            assert(!t.getByKey("3").has_value());                   // insert undone

            // Same mix again, this time committed.
            uint64_t txn2 = t.beginTxn();
            t.insert({"3", "Carol"}, txn2);
            assert(t.updateByKey("1", {"1", "AliceV2"}, txn2));
            assert(t.deleteByKey("2", txn2));
            t.commitTxn(txn2);
        }
        {
            WALWriter wal(wal_file);
            RecoveryManager recovery(wal, data_file, index_file);
            recovery.run();
        }
        {
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("users", schema, pm, rm, im, mvcc);
            auto a = t.getByKey("1");
            assert(a.has_value() && a->getFields()[1] == "AliceV2");
            assert(!t.getByKey("2").has_value());
            auto c = t.getByKey("3");
            assert(c.has_value() && c->getFields()[1] == "Carol");
        }
    } END_TEST;

    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_mvcc_stress_mixed_concurrent() {
    const std::string data_file = "mvcc_stress.db";
    const std::string index_file = "mvcc_stress.idx";
    const std::string wal_file = "mvcc_stress.wal";

    // Repeated-run discipline (CONCURRENCY_BUGS.md's own precedent), kept
    // modest for local hardware -- heavier iteration counts belong in CI
    // via a plain branch push, same as Phase 3/4's races.
    constexpr int NUM_RUNS = 3;
    constexpr int NUM_WRITERS = 4;
    constexpr int KEYS_PER_WRITER = 15;
    constexpr int NUM_INITIAL = NUM_WRITERS * KEYS_PER_WRITER;  // 60
    constexpr int DELETES_PER_WRITER = 5;
    constexpr int NEW_INSERTS_PER_WRITER = 10;

    TEST("mixed concurrent insert/update/delete/read against a shared Table: no corruption, a stable snapshot sees no phantoms") {
        for (int run = 0; run < NUM_RUNS; run++) {
            std::remove(data_file.c_str());
            std::remove(index_file.c_str());
            std::remove(wal_file.c_str());
            Schema schema(std::vector<Column>{{"id","string"},{"value","string"}});
            WALWriter wal(wal_file); TransactionManager txns(wal);
            PageManager pm(data_file, wal, txns); RecordManager rm(pm);
            IndexManager im(index_file, wal, txns);
            MVCCManager mvcc(txns);
            Table t("stress", schema, pm, rm, im, mvcc);

            auto ownedKey = [](int i) { return "k" + std::to_string(i); };
            auto newKey = [](int writer, int i) {
                return "new_" + std::to_string(writer) + "_" + std::to_string(i);
            };

            for (int i = 0; i < NUM_INITIAL; i++) {
                t.insert({ownedKey(i), "v0"});
            }

            // Reader: takes a snapshot before any writer starts, then
            // repeatedly re-reads the same fixed set of keys while
            // writers run concurrently. A repeatable-read snapshot must
            // see exactly what it saw on its first pass, every time --
            // no phantoms from the concurrent updates/deletes/inserts
            // below, no matter how the threads interleave.
            std::atomic<bool> writers_done{false};
            std::atomic<bool> reader_failed{false};
            std::string reader_failure;
            std::thread reader([&] {
                uint64_t reader_txn = t.beginTxn();
                std::vector<std::optional<std::string>> baseline(NUM_INITIAL);
                for (int i = 0; i < NUM_INITIAL; i++) {
                    auto r = t.getByKey(ownedKey(i), reader_txn);
                    baseline[i] = r.has_value() ? std::optional<std::string>(r->getFields()[1]) : std::nullopt;
                }
                while (!writers_done.load()) {
                    for (int i = 0; i < NUM_INITIAL; i++) {
                        auto r = t.getByKey(ownedKey(i), reader_txn);
                        std::optional<std::string> now =
                            r.has_value() ? std::optional<std::string>(r->getFields()[1]) : std::nullopt;
                        if (now != baseline[i]) {
                            reader_failed = true;
                            reader_failure = "key " + ownedKey(i) + " changed under a stable snapshot";
                        }
                    }
                    std::this_thread::yield();
                }
                t.commitTxn(reader_txn);
            });

            std::vector<std::thread> writers;
            for (int w = 0; w < NUM_WRITERS; w++) {
                writers.emplace_back([&, w] {
                    int base = w * KEYS_PER_WRITER;
                    for (int i = 0; i < KEYS_PER_WRITER; i++) {
                        std::string key = ownedKey(base + i);
                        t.updateByKey(key, {key, "v1"});
                        t.updateByKey(key, {key, "v2"});
                    }
                    for (int i = 0; i < DELETES_PER_WRITER; i++) {
                        t.deleteByKey(ownedKey(base + i));
                    }
                    for (int i = 0; i < NEW_INSERTS_PER_WRITER; i++) {
                        t.insert({newKey(w, i), "fresh"});
                    }
                });
            }
            for (auto& th : writers) th.join();
            writers_done = true;
            reader.join();

            if (reader_failed.load()) {
                throw std::runtime_error("run " + std::to_string(run) + ": " + reader_failure);
            }

            // Disjoint key ranges per writer make the end state fully
            // deterministic despite the concurrency above.
            for (int w = 0; w < NUM_WRITERS; w++) {
                int base = w * KEYS_PER_WRITER;
                for (int i = 0; i < DELETES_PER_WRITER; i++) {
                    if (t.getByKey(ownedKey(base + i)).has_value()) {
                        throw std::runtime_error("run " + std::to_string(run) + ": " +
                            ownedKey(base + i) + " should be deleted");
                    }
                }
                for (int i = DELETES_PER_WRITER; i < KEYS_PER_WRITER; i++) {
                    auto r = t.getByKey(ownedKey(base + i));
                    if (!r.has_value() || r->getFields()[1] != "v2") {
                        throw std::runtime_error("run " + std::to_string(run) + ": " +
                            ownedKey(base + i) + " should read v2");
                    }
                }
                for (int i = 0; i < NEW_INSERTS_PER_WRITER; i++) {
                    auto r = t.getByKey(newKey(w, i));
                    if (!r.has_value() || r->getFields()[1] != "fresh") {
                        throw std::runtime_error("run " + std::to_string(run) + ": " +
                            newKey(w, i) + " should exist");
                    }
                }
            }
        }
    } END_TEST;

    std::remove(data_file.c_str());
    std::remove(index_file.c_str());
    std::remove(wal_file.c_str());
}

static void test_mvcc() {
    test_mvcc_txn_grouping();
    test_mvcc_visibility();
    test_mvcc_crash_recovery_versioning();
    test_lockmanager_basic();
    test_lockmanager_deadlock();
    test_mvcc_lock_integration();
    test_mvcc_crash_recovery_mixed_ops();
    test_mvcc_stress_mixed_concurrent();
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
    test_table_basic(); test_table_delete(); test_table_insert_mismatch(); test_table_restart();

    std::cout << "=== Database (Phase 6 Session 1: multi-table storage) ===\n";
    test_database();

    std::cout << "=== SQL Frontend (Phase 6) ===\n";
    test_sql();

    std::cout << "=== MVCC (Phase 5) ===\n";
    test_mvcc();

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
