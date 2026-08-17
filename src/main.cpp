#include <iostream>
#include <string>

#include "db_engine/database.hpp"
#include "db_engine/sqlserver.hpp"

int main() {
    const std::string data_dir = "db_data";
    const uint16_t port = 5433;

    try {
        Database db(data_dir);  // creates data_dir if needed; runs crash recovery if not
        SqlServer server(db, port);
        server.start();

        std::cout << "db_engine listening on port " << port << " (data: " << data_dir << ")\n";
        std::cout << "Connect with any raw TCP client and send ';'-terminated SQL, e.g.:\n";
        std::cout << "  ncat 127.0.0.1 " << port << "\n";
        std::cout << "Type 'quit' and press Enter to shut down.\n";

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit") break;
        }

        std::cout << "Shutting down...\n";
        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "db_engine: fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
