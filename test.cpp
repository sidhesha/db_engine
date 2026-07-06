#include <iostream>
#include <cassert>
#include "key.hpp"
#include "schema.hpp"
#include "record.hpp"
#include "page.hpp"
#include "node.hpp"
#include "bplustree.hpp"
#include "catalogmanager.hpp"
#include "pagemanager.hpp"
#include "recordmanager.hpp"
#include "table.hpp"

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
        PageManager pm(file);
        assert(pm.allocatePage() == 0);
        assert(pm.getNextPageId() == 1);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_pm_readback() {
    const std::string file = "pm_read.db";
    std::remove(file.c_str());
    TEST("write and read back") {
        PageManager pm(file);
        int id = pm.allocatePage();
        assert(pm.readPage(id).getPageId() == (uint32_t)id);
    } END_TEST;
    std::remove(file.c_str());
}

static void test_pm_multi() {
    const std::string file = "pm_multi.db";
    std::remove(file.c_str());
    {
        PageManager pm(file);
        for (int i = 0; i < 5; i++) assert(pm.allocatePage() == i);
        assert(pm.getNextPageId() == 5);
    }
    {
        PageManager pm(file);
        assert(pm.getNextPageId() == 5);
    }
    std::remove(file.c_str());
}

// ─── RecordManager ───────────────────────────────────────────────────────────

static void test_rm_basic() {
    const std::string file = "rm_basic.db";
    std::remove(file.c_str());
    TEST("insert and read") {
        PageManager pm(file); RecordManager rm(pm);
        RID rid = rm.insertRecord(Record(std::vector<std::string>{"hello","world"}));
        Record r = rm.readRecord(rid);
        assert(r.getFields()[0] == "hello");
    } END_TEST;
    TEST("delete record") {
        PageManager pm(file); RecordManager rm(pm);
        RID rid = rm.insertRecord(Record(std::vector<std::string>{"del"}));
        assert(rm.deleteRecord(rid));
    } END_TEST;
    std::remove(file.c_str());
}

static void test_rm_multi() {
    const std::string file = "rm_multi.db";
    std::remove(file.c_str());
    TEST("multiple records") {
        PageManager pm(file); RecordManager rm(pm);
        RID ids[10];
        for (int i = 0; i < 10; i++)
            ids[i] = rm.insertRecord(Record(std::vector<std::string>{std::to_string(i)}));
        for (int i = 0; i < 10; i++)
            assert(rm.readRecord(ids[i]).getFields()[0] == std::to_string(i));
    } END_TEST;
    std::remove(file.c_str());
}

// ─── Table ───────────────────────────────────────────────────────────────────

static void test_table_basic() {
    const std::string file = "tbl_basic.db";
    std::remove(file.c_str());
    TEST("insert and get by key") {
        PageManager pm(file); RecordManager rm(pm);
        Table t("users", Schema(std::vector<Column>{{"id","int"},{"name","string"}}), pm, rm);
        t.insert({"1","Alice"}); t.insert({"2","Bob"});
        auto r = t.getByKey("1");
        assert(r.has_value() && r->getFields()[1] == "Alice");
    } END_TEST;
    TEST("get nonexistent key") {
        PageManager pm(file); RecordManager rm(pm);
        Table t("x", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        assert(!t.getByKey("99").has_value());
    } END_TEST;
    std::remove(file.c_str());
}

static void test_table_delete() {
    const std::string file = "tbl_del.db";
    std::remove(file.c_str());
    TEST("delete by key") {
        PageManager pm(file); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        t.insert({"1"}); assert(t.deleteByKey("1"));
        assert(!t.getByKey("1").has_value());
    } END_TEST;
    TEST("delete nonexistent") {
        PageManager pm(file); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        assert(!t.deleteByKey("99"));
    } END_TEST;
    std::remove(file.c_str());
}

static void test_table_insert_mismatch() {
    const std::string file = "tbl_mismatch.db";
    std::remove(file.c_str());
    TEST("insert mismatched columns throws") {
        PageManager pm(file); RecordManager rm(pm);
        Table t("t", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        bool caught = false;
        try { t.insert({"1","extra"}); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
    } END_TEST;
    std::remove(file.c_str());
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
    std::cout << "=== PageManager ===\n";
    test_pm_alloc(); test_pm_readback(); test_pm_multi();
    std::cout << "=== RecordManager ===\n";
    test_rm_basic(); test_rm_multi();
    std::cout << "=== Table ===\n";
    test_table_basic(); test_table_delete(); test_table_insert_mismatch();

    int total = passed + failed;
    std::cout << "\n=== " << passed << "/" << total << " passed";
    if (failed) std::cout << " (" << failed << " FAILED)";
    std::cout << " ===\n";
    return failed ? 1 : 0;
}
