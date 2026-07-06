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

static void test_key() {
    TEST("int") { Key k(42); assert(k.getType() == KeyTypeTag::INTEGER); } END_TEST;
    TEST("float") { Key k(3.14f); assert(k.getType() == KeyTypeTag::FLOAT); } END_TEST;
    TEST("string") { Key k("h"); assert(k.getType() == KeyTypeTag::STRING); } END_TEST;
    TEST("cmp") { assert(Key(1) < Key(2)); } END_TEST;
    TEST("eq") { assert(Key(4) == Key(4)); } END_TEST;
}

static void test_schema() {
    TEST("create") {
        Schema s(std::vector<Column>{{"id", "int"}});
        assert(s.getColumns().size() == 1);
    } END_TEST;
    TEST("serialize") {
        Schema a(std::vector<Column>{{"a","int"},{"b","string"}});
        Schema b = Schema::deserialize(a.serialize());
        assert(b.getColumns().size() == 2);
    } END_TEST;
}

static void test_record() {
    TEST("create") {
        Record r(std::vector<std::string>{"a"});
        assert(r.getFields().size() == 1);
    } END_TEST;
    TEST("serialize") {
        Record a(std::vector<std::string>{"hello","world"});
        Record b = Record::deserialize(a.serialize());
        assert(b.getFields()[1] == "world");
    } END_TEST;
}

static void test_page() {
    TEST("insert") {
        Page p(0); p.insertRecord({'x'}); assert(p.getNumSlots() == 1);
    } END_TEST;
    TEST("serialize") {
        Page a(7); a.insertRecord({'a'});
        Page b = Page::deserialize(a.serialize());
        assert(b.getPageId() == 7u);
    } END_TEST;
    TEST("full") {
        Page p(0);
        assert(p.insertRecord(std::vector<char>(PAGE_SIZE-100,'x')) != -1);
        assert(p.insertRecord(std::vector<char>(PAGE_SIZE,'x')) == -1);
    } END_TEST;
}

static void test_node() {
    TEST("insert") {
        BPlusTreeNode n(true); n.insertInLeaf(Key("a"), 0, 0); assert(n.keys.size() == 1);
    } END_TEST;
    TEST("split") {
        BPlusTreeNode n(true);
        for (int i = 0; i < MAX_KEYS+1; i++) n.insertInLeaf(Key(std::to_string(i)),0,i);
        auto [sk,nn] = n.splitLeafNode();
        assert(nn->keys.size() > 0);
    } END_TEST;
}

static void test_tree() {
    TEST("insert/search") {
        BPlusTree t; t.insert(Key("a"), 0, 0);
        assert(t.search(Key("a")).has_value());
    } END_TEST;
    TEST("many") {
        BPlusTree t;
        for (int i = 0; i < 20; i++) t.insert(Key(std::to_string(i)), i, i);
        for (int i = 0; i < 20; i++) assert(t.search(Key(std::to_string(i))).has_value());
    } END_TEST;
    TEST("remove") {
        BPlusTree t; t.insert(Key("a"),0,0); t.insert(Key("b"),1,1);
        assert(t.remove(Key("a")));
        assert(!t.search(Key("a")).has_value());
    } END_TEST;
}

static void test_cat() {
    const std::string f = "tcat.txt";
    std::remove(f.c_str());
    TEST("create") {
        CatalogManager cm(f);
        cm.createTable("x", Schema(std::vector<Column>{{"id", "int"}}));
        assert(cm.hasTable("x"));
    } END_TEST;
    std::remove(f.c_str());
}

static void test_pm() {
    const std::string f = "tpm.db";
    std::remove(f.c_str());
    TEST("alloc") {
        PageManager pm(f); assert(pm.allocatePage() == 0);
    } END_TEST;
    std::remove(f.c_str());
}

static void test_rm() {
    const std::string f = "trm.db";
    std::remove(f.c_str());
    TEST("insert") {
        PageManager pm(f); RecordManager rm(pm);
        RID rid = rm.insertRecord(Record(std::vector<std::string>{"hello"}));
        assert(rm.readRecord(rid).getFields()[0] == "hello");
    } END_TEST;
    std::remove(f.c_str());
}

static void test_tbl() {
    const std::string f = "ttbl.db";
    std::remove(f.c_str());
    TEST("insert") {
        PageManager pm(f); RecordManager rm(pm);
        Table t("x", Schema(std::vector<Column>{{"id","int"}}), pm, rm);
        t.insert({"1"}); assert(t.getByKey("1").has_value());
    } END_TEST;
    std::remove(f.c_str());
}

int main() {
    test_key(); test_schema(); test_record(); test_page();
    test_node(); test_tree(); test_cat(); test_pm(); test_rm(); test_tbl();
    std::cout << passed << "/" << (passed+failed) << " passed\n";
    return failed ? 1 : 0;
}
