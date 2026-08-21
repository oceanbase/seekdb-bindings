#include "internal.hpp"

namespace seekdb {

// ---------------------------------------------------------------------------
// CursorState
// ---------------------------------------------------------------------------

CursorState::CursorState(std::shared_ptr<ConnectionState> conn) : conn_(std::move(conn)) {}

CursorState::~CursorState() { free_result(); }

int64_t CursorState::execute(const std::string &sql)
{
    if (!conn_ || !conn_->raw())
        throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "Cursor.execute: no connection");
    free_result();
    const int rc =
        seekdb_query(conn_->raw(), sql.c_str(), static_cast<int64_t>(sql.size()), &result_);
    if (rc != SEEKDB_SUCCESS) {
        int srv_errno = 0;
        const char *srv_msg = nullptr;
        seekdb_last_error(conn_->raw(), &srv_errno, &srv_msg);
        throw SeekdbErrorC(srv_errno != 0 ? srv_errno : rc,
                           (srv_msg && *srv_msg) ? srv_msg : "seekdb_query failed");
    }
    int64_t n = 0;
    if (seekdb_result_row_count(result_, &n) != SEEKDB_SUCCESS)
        n = 0;
    return n;
}

bool CursorState::next_row(std::vector<CellValue> &out)
{
    if (!result_)
        return false;
    if (seekdb_result_next(result_) != SEEKDB_SUCCESS)
        return false;
    int64_t ncol = 0;
    SDB_CHECK(seekdb_result_column_count(result_, &ncol));
    out.clear();
    out.reserve(static_cast<size_t>(ncol));
    for (int64_t i = 0; i < ncol; ++i) {
        CellValue cell;
        ReadCell(result_, i, cell);
        out.push_back(std::move(cell));
    }
    return true;
}

int64_t CursorState::row_count()
{
    if (!result_)
        return 0;
    int64_t n = 0;
    if (seekdb_result_row_count(result_, &n) != SEEKDB_SUCCESS)
        n = 0;
    return n;
}

void CursorState::free_result()
{
    if (result_) {
        seekdb_result_free(result_);
        result_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Async workers
// ---------------------------------------------------------------------------

namespace {

class ExecuteWorker : public Napi::AsyncWorker {
  public:
    ExecuteWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                  std::shared_ptr<CursorState> state, std::string sql)
        : Napi::AsyncWorker(env), deferred_(deferred), state_(std::move(state)),
          sql_(std::move(sql))
    {
    }

    void Execute() override
    {
        try {
            std::lock_guard<std::mutex> lock(state_->mutex_);
            if (state_->closed())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "cursor is closed");
            affected_ = state_->execute(sql_);
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        deferred_.Resolve(Napi::Number::New(Env(), static_cast<double>(affected_)));
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<CursorState> state_;
    std::string sql_;
    int64_t affected_ = 0;
    int code_ = 0;
};

class FetchOneWorker : public Napi::AsyncWorker {
  public:
    FetchOneWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                   std::shared_ptr<CursorState> state)
        : Napi::AsyncWorker(env), deferred_(deferred), state_(std::move(state))
    {
    }

    void Execute() override
    {
        try {
            std::lock_guard<std::mutex> lock(state_->mutex_);
            if (state_->closed())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "cursor is closed");
            has_row_ = state_->next_row(row_);
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        Napi::Env env = Env();
        try {
            deferred_.Resolve(has_row_ ? RowToNapi(env, row_) : env.Null());
        }
        catch (const SeekdbErrorC &e) {
            deferred_.Reject(MakeError(env, e.code(), e.what()).Value());
        }
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<CursorState> state_;
    std::vector<CellValue> row_;
    bool has_row_ = false;
    int code_ = 0;
};

class FetchAllWorker : public Napi::AsyncWorker {
  public:
    FetchAllWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                   std::shared_ptr<CursorState> state)
        : Napi::AsyncWorker(env), deferred_(deferred), state_(std::move(state))
    {
    }

    void Execute() override
    {
        try {
            std::lock_guard<std::mutex> lock(state_->mutex_);
            if (state_->closed())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "cursor is closed");
            std::vector<CellValue> row;
            while (state_->next_row(row))
                rows_.push_back(std::move(row));
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        Napi::Env env = Env();
        try {
            Napi::Array arr = Napi::Array::New(env, rows_.size());
            for (size_t i = 0; i < rows_.size(); ++i)
                arr.Set(i, RowToNapi(env, rows_[i]));
            deferred_.Resolve(arr);
        }
        catch (const SeekdbErrorC &e) {
            deferred_.Reject(MakeError(env, e.code(), e.what()).Value());
        }
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<CursorState> state_;
    std::vector<std::vector<CellValue>> rows_;
    int code_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Cursor wrapper
// ---------------------------------------------------------------------------

Napi::Object MakeCursor(Napi::Env env, std::shared_ptr<CursorState> state)
{
    Napi::Object obj = Cursor::constructor.New({});
    Napi::ObjectWrap<Cursor>::Unwrap(obj)->Adopt(std::move(state));
    return obj;
}

Napi::FunctionReference Cursor::constructor;

void Cursor::Adopt(std::shared_ptr<CursorState> state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(state);
}

std::shared_ptr<CursorState> Cursor::require_open(Napi::Env env)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || state_->closed())
        throw MakeError(env, SEEKDB_INVALID_ARGUMENT, "cursor is closed");
    return state_;
}

void Cursor::Init(Napi::Env env, Napi::Object exports)
{
    Napi::Function func = DefineClass(env, "Cursor",
                                      {
                                          InstanceMethod("execute", &Cursor::Execute),
                                          InstanceMethod("fetchOne", &Cursor::FetchOne),
                                          InstanceMethod("fetchAll", &Cursor::FetchAll),
                                          InstanceMethod("close", &Cursor::Close),
                                          InstanceAccessor("closed", &Cursor::GetClosed, nullptr),
                                      });
    constructor = Napi::Persistent(func);
    exports.Set("Cursor", func);
}

Napi::Value Cursor::Execute(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString())
        throw MakeError(env, SEEKDB_INVALID_ARGUMENT, "execute(sql) requires a string");
    std::string sql = info[0].As<Napi::String>().Utf8Value();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new ExecuteWorker(env, deferred, std::move(state), std::move(sql));
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Cursor::FetchOne(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new FetchOneWorker(env, deferred, std::move(state));
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Cursor::FetchAll(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new FetchAllWorker(env, deferred, std::move(state));
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Cursor::Close(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::shared_ptr<CursorState> to_close;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to_close = std::move(state_);
    }
    if (to_close) {
        std::lock_guard<std::mutex> lock(to_close->mutex_);
        to_close->free_result();
        to_close->mark_closed();
    }
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Resolve(env.Undefined());
    return deferred.Promise();
}

Napi::Value Cursor::GetClosed(const Napi::CallbackInfo &info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Napi::Boolean::New(info.Env(), !state_);
}

} // namespace seekdb