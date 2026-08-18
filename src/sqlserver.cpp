#include "db_engine/sqlserver.hpp"

#include <stdexcept>
#include <string>

#include "db_engine/lexer.hpp"
#include "db_engine/parser.hpp"

namespace {
// Winsock needs an explicit init/teardown pair around any socket use;
// BSD sockets need nothing here. Centralized so the constructor/
// destructor below don't repeat an #ifdef at every call site (three
// error paths in the constructor alone).
#ifdef _WIN32
void platformSocketInit() {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("SqlServer: WSAStartup failed");
    }
}
void platformSocketCleanup() { WSACleanup(); }
#else
void platformSocketInit() {}
void platformSocketCleanup() {}
#endif

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
    platformSocketInit();

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        platformSocketCleanup();
        throw std::runtime_error("SqlServer: socket() failed");
    }

    // Lets a server restarted on the same port bind immediately instead
    // of failing with "address already in use" while the OS still has
    // the previous socket lingering in TIME_WAIT -- relevant for tests
    // that start/stop a server repeatedly against a fixed port. `int`
    // (not Windows' BOOL) works identically on both platforms here --
    // BOOL is itself just `typedef int BOOL` in the Windows headers.
    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_socket);
        platformSocketCleanup();
        throw std::runtime_error("SqlServer: bind() failed on port " + std::to_string(port));
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_socket);
        platformSocketCleanup();
        throw std::runtime_error("SqlServer: listen() failed");
    }
}

SqlServer::~SqlServer() {
    stop();
    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
    }
    platformSocketCleanup();
}

void SqlServer::start() {
    running = true;
    accept_thread = std::thread(&SqlServer::acceptLoop, this);
}

void SqlServer::stop() {
    if (!running.exchange(false)) return;  // already stopped (or never started)

    // acceptLoop() polls `running` at least every ~200ms (see below)
    // rather than blocking indefinitely in accept(), so this join always
    // returns promptly on its own -- no separate "cancel a blocked
    // accept()" trick needed. That matters because there isn't a
    // portable one: Windows' closesocket() reliably wakes a *different*
    // thread's blocking call on the same socket, but POSIX leaves that
    // case unspecified, and on Linux specifically another thread's
    // accept() often does NOT wake up when the fd is merely closed
    // elsewhere -- confirmed the hard way, this hung the test suite
    // indefinitely the first time this port ran in CI on Linux. Only
    // touching listen_socket here, after the accept thread has fully
    // joined (and so provably isn't reading it anymore), also avoids a
    // data race that a "close it to unblock the other thread" approach
    // would otherwise have on this member.
    if (accept_thread.joinable()) accept_thread.join();

    closesocket(listen_socket);
    listen_socket = INVALID_SOCKET;

    std::lock_guard<std::mutex> lock(conn_mu);
    for (auto& t : connection_threads) {
        if (t.joinable()) t.join();
    }
    connection_threads.clear();
}

void SqlServer::acceptLoop() {
    // Captured once: stable for this thread's whole lifetime by
    // construction (stop() -- the only thing that ever changes
    // listen_socket -- only runs after this thread has already started,
    // per this class's documented start()-then-stop() contract, and only
    // touches listen_socket again after joining this thread; see stop()).
    SOCKET fd = listen_socket;

    while (running) {
        // Poll with a short timeout rather than blocking in accept()
        // indefinitely, so stop() setting `running = false` is always
        // noticed within one poll interval instead of relying on
        // unblocking a concurrent blocking call from another thread --
        // see stop()'s comment for why that isn't portable.
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval tv{0, 200000};  // 200ms
        int ready = select(static_cast<int>(fd) + 1, &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;  // timeout, or a transient error -- recheck `running`

        sockaddr_in client_addr{};
        addrlen_t addr_len = sizeof(client_addr);
        SOCKET client = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == INVALID_SOCKET) {
            continue;  // transient -- select() said readable but accept() still failed
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
