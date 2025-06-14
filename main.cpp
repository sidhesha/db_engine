#include <iostream>
#include "src/page.cpp"
int main() {
    Page p(0);
    std::string a(10,'a');
    p.insertRecord(a);
    
    std::string b(4068,'b');
    // 4068 + 5 + 10 + 5 + 8 == 4096 so should be able to fit
    p.insertRecord(b);

    // p.insertRecord("a");
    // std::cout << p.readRecord(0) << "\n";

    // p.insertRecord("my first record");
    // p.insertRecord("my second record");    
    // p.deleteRecord(0);
    // p.insertRecord("my third record");
    std::cout << p.readRecord(0) << "\n";
    // std::cout << p.readRecord(1) << "\n";
    // std::cout << p.readRecord(0) << "\n";
    return 0;
}