#pragma once

// Platform socket compat shim. Winsock2 (Windows) and BSD/POSIX sockets
// (Linux/macOS) agree on almost everything that matters here --
// socket()/bind()/listen()/accept()/send()/recv() all have the same
// names and near-identical signatures -- so rather than wrapping every
// call site in #ifdef, this aliases the handful of names that actually
// differ (SOCKET's underlying type, the "invalid" sentinel, close vs.
// closesocket, and Winsock's own init/teardown) so the rest of this
// class's code -- everything below this block -- is written once and
// compiles identically on both.
#ifdef _WIN32
// winsock2.h pulls in windows.h. Defined before that include (and before
// any other file in this project transitively includes it) to avoid two
// classic landmines: windows.h's own winsock1 declarations conflicting
// with winsock2.h (WIN32_LEAN_AND_MEAN), and its `min`/`max` macros
// silently breaking std::min/std::max anywhere downstream (NOMINMAX).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
using addrlen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SOCKET = int;
using addrlen_t = socklen_t;
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
inline int closesocket(int fd) { return close(fd); }
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "db_engine/database.hpp"
#include "db_engine/executor.hpp"

// TCP server, Winsock2 on Windows / BSD sockets on Linux and macOS (see
// the compat shim above). Accepts connections and spawns one thread per
// connection: Database/Table/
// MVCCManager/LockManager are already safe under concurrent callers by
// construction (Phase 5 Session 5's stress testing proved this), so
// serving connections concurrently costs almost nothing and is the only
// way to actually demonstrate a write-write conflict blocking and
// resolving, or a deadlock getting detected, *through SQL* rather than
// only at the Table API directly.
//
// Protocol: a client sends one or more `;`-terminated SQL statements
// (buffered across recv() calls -- a statement can arrive split across
// multiple packets, and multiple statements can arrive in one packet);
// each gets exactly one response, in order:
//   SELECT                          -> a '|'-delimited column-name header
//                                      line, one '|'-delimited row per
//                                      line, then "OK <n> rows"
//   INSERT / UPDATE / DELETE        -> "OK <n> rows affected"
//   CREATE TABLE/BEGIN/COMMIT/ROLLBACK -> "OK"
//   a parse error or QueryResult{ok=false} -> "ERROR: <message>"
// Every response line (including multi-line SELECT output) ends in '\n'.
class SqlServer {
public:
    // db must outlive this SqlServer. Binds and starts listening
    // immediately (so a bad port surfaces as a thrown exception right
    // away, not silently inside a background thread) -- call start() to
    // begin actually accepting connections.
    SqlServer(Database& db, uint16_t port);
    // Calls stop() if it hasn't been called already.
    ~SqlServer();
    SqlServer(const SqlServer&) = delete;
    SqlServer& operator=(const SqlServer&) = delete;

    // Spawns the accept-loop thread; returns immediately.
    void start();
    // Stops accepting new connections and joins the accept thread and
    // every connection thread spawned so far. Callers are expected to
    // have already caused their own client sockets to disconnect (closed
    // or the peer hung up) before calling this -- a connection thread
    // still blocked in recv() on a client that never disconnects would
    // make this hang, same as any blocking-join shutdown.
    void stop();

private:
    Database& db;
    uint16_t port;
    SOCKET listen_socket = INVALID_SOCKET;
    std::atomic<bool> running{false};
    std::thread accept_thread;

    std::mutex conn_mu;
    std::vector<std::thread> connection_threads;

    void acceptLoop();
    void handleConnection(SOCKET client_socket);
};
