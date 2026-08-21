#pragma once

#include <napi.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "seekdb.h"
}

namespace seekdb {

// ---------------------------------------------------------------------------
// Error translation
// ---------------------------------------------------------------------------

// C++ error carrying the server errno alongside a message. Mirrors the Python
// SeekdbError (bindings.cpp) and is translated to a JS Error with a `code`
// property at the N-API boundary.
class SeekdbErrorC : public std::runtime_error {
  public:
    SeekdbErrorC(int code, std::string msg) : std::runtime_error(std::move(msg)), code_(code) {}
    int code() const { return code_; }

  private:
    int code_;
};

#define SDB_CHECK(expr)                                                                            \
    do {                                                                                           \
        int _rc = (expr);                                                                          \
        if (_rc != SEEKDB_SUCCESS)                                                                 \
            throw SeekdbErrorC(_rc, #expr);                                                        \
    } while (0)

// Build a JS Error with .name = "SeekdbError" and .code = `code`.
Napi::Error MakeError(Napi::Env env, int code, const std::string &msg);

// Fill (errno, message) from seekdb_last_error, falling back to `rc`.
void LastError(SeekdbConnection conn, int rc, int *out_errno, std::string *out_msg);

// ---------------------------------------------------------------------------
// Instance lifecycle (shared per normalized db_dir, aligned with Python)
// ---------------------------------------------------------------------------

class InstanceState {
  public:
    explicit InstanceState(const std::string &db_dir);
    ~InstanceState();
    InstanceState(const InstanceState &) = delete;
    InstanceState &operator=(const InstanceState &) = delete;

    SeekdbHandle raw() const { return handle_; }
    void close_checked();

  private:
    SeekdbHandle handle_;
};

std::string NormalizeDbDir(const std::string &db_dir);
std::shared_ptr<InstanceState> GetOrCreateInstance(const std::string &db_dir);
void ReleaseInstance(const std::string &db_dir, InstanceState *state);

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

class ConnectionState {
  public:
    ConnectionState(SeekdbConnection c, std::shared_ptr<InstanceState> instance);
    ~ConnectionState();
    ConnectionState(const ConnectionState &) = delete;
    ConnectionState &operator=(const ConnectionState &) = delete;

    void reset();
    SeekdbConnection raw() const { return c_; }

  private:
    SeekdbConnection c_;
    std::shared_ptr<InstanceState> instance_;
};

// ---------------------------------------------------------------------------
// Cursor result state
// ---------------------------------------------------------------------------

// Cross-thread-safe snapshot of one result cell. Worker threads fill these in
// Execute(); the main thread converts them to JS values in OnOK().
struct CellValue {
    enum class Kind { Null, Int64, Uint64, Double, String };
    Kind kind = Kind::Null;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double d = 0.0;
    std::string str;
};

class CursorState {
  public:
    explicit CursorState(std::shared_ptr<ConnectionState> conn);
    ~CursorState();
    CursorState(const CursorState &) = delete;
    CursorState &operator=(const CursorState &) = delete;

    // Run a query, replacing the current result set. Returns the affected-row
    // count. Caller must hold mutex_.
    int64_t execute(const std::string &sql);
    // Advance one row and snapshot it. Returns false at end of result.
    // Caller must hold mutex_.
    bool next_row(std::vector<CellValue> &out);
    int64_t row_count();
    void free_result();

    bool closed() const { return closed_; }
    void mark_closed() { closed_ = true; }

    std::shared_ptr<ConnectionState> conn_;
    mutable std::mutex mutex_;

  private:
    SeekdbResult result_ = nullptr;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Value conversion
// ---------------------------------------------------------------------------

void ReadCell(SeekdbResult result, int64_t idx, CellValue &out);
Napi::Value CellToNapi(Napi::Env env, const CellValue &cell);
Napi::Value RowToNapi(Napi::Env env, const std::vector<CellValue> &row);
Napi::Object ConnectionOptionsToNapi(Napi::Env env, const SeekdbConnectionOptions &opts);

// Factories building wrapped objects (defined in connection.cpp / cursor.cpp).
Napi::Value MakeConnectionAsync(Napi::Env env, std::shared_ptr<InstanceState> instance,
                                std::string database, bool autocommit);
Napi::Object MakeCursor(Napi::Env env, std::shared_ptr<CursorState> state);

// Wrapper classes registered on the module (full definitions in their own .cpp).

class SeekdbInstance : public Napi::ObjectWrap<SeekdbInstance> {
  public:
    // ObjectWrap's ConstructorCallbackWrapper does `new T(callbackInfo)`,
    // so the constructor must be reachable from the base class template.
    explicit SeekdbInstance(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SeekdbInstance>(info) {}
    static Napi::FunctionReference constructor;

    static void Init(Napi::Env env, Napi::Object exports);
    static Napi::Value Open(const Napi::CallbackInfo &info);

    void Adopt(std::string db_dir, std::shared_ptr<InstanceState> instance);

  private:

    Napi::Value Connect(const Napi::CallbackInfo &info);
    Napi::Value ConnectionOptions(const Napi::CallbackInfo &info);
    Napi::Value Close(const Napi::CallbackInfo &info);
    Napi::Value GetClosed(const Napi::CallbackInfo &info);
    Napi::Value GetDbDir(const Napi::CallbackInfo &info);

    std::mutex mutex_;
    std::string db_dir_;
    std::shared_ptr<InstanceState> instance_;
};

class Connection : public Napi::ObjectWrap<Connection> {
  public:
    explicit Connection(const Napi::CallbackInfo &info) : Napi::ObjectWrap<Connection>(info) {}
    static Napi::FunctionReference constructor;

    static void Init(Napi::Env env, Napi::Object exports);
    void Adopt(std::shared_ptr<ConnectionState> state);

  private:

    std::shared_ptr<ConnectionState> require_open(Napi::Env env);

    Napi::Value Cursor(const Napi::CallbackInfo &info);
    Napi::Value Begin(const Napi::CallbackInfo &info);
    Napi::Value Commit(const Napi::CallbackInfo &info);
    Napi::Value Rollback(const Napi::CallbackInfo &info);
    Napi::Value Close(const Napi::CallbackInfo &info);
    Napi::Value GetClosed(const Napi::CallbackInfo &info);

    std::mutex mutex_;
    std::shared_ptr<ConnectionState> state_;
};

class Cursor : public Napi::ObjectWrap<Cursor> {
  public:
    explicit Cursor(const Napi::CallbackInfo &info) : Napi::ObjectWrap<Cursor>(info) {}
    static Napi::FunctionReference constructor;

    static void Init(Napi::Env env, Napi::Object exports);
    void Adopt(std::shared_ptr<CursorState> state);

  private:

    std::shared_ptr<CursorState> require_open(Napi::Env env);

    Napi::Value Execute(const Napi::CallbackInfo &info);
    Napi::Value FetchOne(const Napi::CallbackInfo &info);
    Napi::Value FetchAll(const Napi::CallbackInfo &info);
    Napi::Value Close(const Napi::CallbackInfo &info);
    Napi::Value GetClosed(const Napi::CallbackInfo &info);

    std::mutex mutex_;
    std::shared_ptr<CursorState> state_;
};

} // namespace seekdb