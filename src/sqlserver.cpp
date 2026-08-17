#include "sqlserver.hpp"

#include <stdexcept>
#include <string>

#include "lexer.hpp"
#include "parser.hpp"

namespace {
std::string formatResult(const Stmt& stmt, const QueryResult& result) {
    if (!result.ok) {
        return "ERROR: " + result.message + "\n";
    }
    if (std::holds_alternative<SelectStmt>(stmt)) {
        std::string out;
        for (std::size_t i = 0; i < result.columns.size(); i++) {
            if (i) out += "|";
            out += result.columns[i];
        }
        out += "\n";
        for (const auto& row : result.rows) {
            for (std::size_t i = 0; i < row.size(); i++) {
                if (i) out += "|";
                out += row[i];
            }
            out += "\n";
        }
        out += "OK " + std::to_string(result.rows.size()) + " rows\n";
        return out;
    }
    if (std::holds_alternative<InsertStmt>(stmt) || std::holds_alternative<UpdateStmt>(stmt) ||
        std::holds_alternative<DeleteStmt>(stmt)) {
        return "OK " + std::to_string(result.affected_rows) + " rows affected\n";
    }
    return "OK\n";  // CREATE TABLE / BEGIN / COMMIT / ROLLBACK
}
}  // namespace

SqlServer::SqlServer(Database& db, uint16_t port) : db(db), port(port) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("SqlServer: WSAStartup failed");
    }

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error("SqlServer: socket() failed");
    }

    // Lets a server restarted on the same port bind immediately instead
    // of failing with "address already in use" while the OS still has
    // the previous socket lingering in TIME_WAIT -- relevant for tests
    // that start/stop a server repeatedly against a fixed port.
    BOOL reuse = TRUE;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_socket);
        WSACleanup();
        throw std::runtime_error("SqlServer: bind() failed on port " + std::to_string(port));
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_socket);
        WSACleanup();
        throw std::runtime_error("SqlServer: listen() failed");
    }
}

SqlServer::~SqlServer() {
    stop();
    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
    }
    WSACleanup();
}

void SqlServer::start() {
    running = true;
    accept_thread = std::thread(&SqlServer::acceptLoop, this);
}

void SqlServer::stop() {
    if (!running.exchange(false)) return;  // already stopped (or never started)

    // Winsock has no separate "cancel a blocked accept()" call -- closing
    // the listening socket is what unblocks the accept-loop thread.
    closesocket(listen_socket);
    listen_socket = INVALID_SOCKET;
    if (accept_thread.joinable()) accept_thread.join();

    std::lock_guard<std::mutex> lock(conn_mu);
    for (auto& t : connection_threads) {
        if (t.joinable()) t.join();
    }
    connection_threads.clear();
}

void SqlServer::acceptLoop() {
    while (running) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == INVALID_SOCKET) {
            // Either a real transient error, or stop() just closed
            // listen_socket out from under this call -- `running` being
            // false in that case is what ends the loop, not a retry.
            continue;
        }

        std::lock_guard<std::mutex> lock(conn_mu);
        connection_threads.emplace_back(&SqlServer::handleConnection, this, client);
    }
}

void SqlServer::handleConnection(SOCKET client) {
    uint64_t session_txn_id = 0;
    std::string buffer;
    char chunk[4096];

    while (true) {
        // Consume one full statement already sitting in `buffer` before
        // asking recv() for more -- a single packet can hold more than
        // one `;`-terminated statement.
        std::size_t semi = buffer.find(';');
        if (semi == std::string::npos) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;  // client closed, or a real socket error
            buffer.append(chunk, n);
            continue;
        }

        std::string stmt_text = buffer.substr(0, semi + 1);
        buffer.erase(0, semi + 1);

        std::string response;
        try {
            Lexer lexer(stmt_text);
            Parser parser(lexer.tokenize());
            Stmt stmt = parser.parseStatement();
            QueryResult result = execute(stmt, db, session_txn_id);
            response = formatResult(stmt, result);
        } catch (const std::exception& e) {
            response = std::string("ERROR: ") + e.what() + "\n";
        }

        send(client, response.data(), static_cast<int>(response.size()), 0);
    }

    closesocket(client);
}
