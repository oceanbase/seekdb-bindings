#include "internal.hpp"

namespace seekdb {

// ---------------------------------------------------------------------------
// ConnectionState
// ---------------------------------------------------------------------------

ConnectionState::ConnectionState(SeekdbConnection c, std::shared_ptr<InstanceState> instance)
    : c_(c), instance_(std::move(instance))
{
}

ConnectionState::~ConnectionState() { reset(); }

void ConnectionState::reset()
{
    if (c_) {
        seekdb_disconnect(c_);
        c_ = nullptr;
    }
    instance_.reset();
}

// ---------------------------------------------------------------------------
// Async helpers
// ---------------------------------------------------------------------------

namespace {

// Connect on the worker thread. seekdb_connect may block on the local MySQL
// handshake, so it runs off the main thread (mirrors Python's GIL release).
class ConnectWorker : public Napi::AsyncWorker {
  public:
    ConnectWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                  std::shared_ptr<InstanceState> instance, std::string database, bool autocommit)
        : Napi::AsyncWorker(env), deferred_(deferred), instance_(std::move(instance)),
          database_(std::move(database)), autocommit_(autocommit)
    {
    }

    void Execute() override
    {
        try {
            if (!instance_ || !instance_->raw())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "seekdb instance is closed");
            SeekdbConnection c = nullptr;
            const int rc = seekdb_connect(instance_->raw(), database_.c_str(), autocommit_, &c);
            if (rc != SEEKDB_SUCCESS) {
                int srv_errno = 0;
                const char *srv_msg = nullptr;
                if (c)
                    seekdb_last_error(c, &srv_errno, &srv_msg);
                std::string msg = (srv_msg && *srv_msg) ? srv_msg : "seekdb_connect failed";
                if (c)
                    seekdb_disconnect(c);
                throw SeekdbErrorC(srv_errno != 0 ? srv_errno : rc, std::move(msg));
            }
            conn_ = std::make_shared<ConnectionState>(c, instance_);
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        Napi::Object obj = Connection::constructor.New({});
        Napi::ObjectWrap<Connection>::Unwrap(obj)->Adopt(conn_);
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<InstanceState> instance_;
    std::string database_;
    bool autocommit_;
    std::shared_ptr<ConnectionState> conn_;
    int code_ = 0;
};

class TrxWorker : public Napi::AsyncWorker {
  public:
    enum class Op { Begin, Commit, Rollback };

    TrxWorker(Napi::Env env, Napi::Promise::Deferred deferred,
              std::shared_ptr<ConnectionState> conn, Op op)
        : Napi::AsyncWorker(env), deferred_(deferred), conn_(std::move(conn)), op_(op)
    {
    }

    void Execute() override
    {
        try {
            if (!conn_ || !conn_->raw())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "connection is closed");
            int rc = SEEKDB_SUCCESS;
            switch (op_) {
            case Op::Begin:
                rc = seekdb_trx_begin(conn_->raw());
                break;
            case Op::Commit:
                rc = seekdb_trx_commit(conn_->raw());
                break;
            case Op::Rollback:
                rc = seekdb_trx_rollback(conn_->raw());
                break;
            }
            if (rc != SEEKDB_SUCCESS) {
                int srv_errno = 0;
                std::string msg;
                LastError(conn_->raw(), rc, &srv_errno, &msg);
                throw SeekdbErrorC(srv_errno != 0 ? srv_errno : rc, std::move(msg));
            }
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override { deferred_.Resolve(Env().Undefined()); }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<ConnectionState> conn_;
    Op op_;
    int code_ = 0;
};

class DisconnectWorker : public Napi::AsyncWorker {
  public:
    DisconnectWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                     std::shared_ptr<ConnectionState> conn)
        : Napi::AsyncWorker(env), deferred_(deferred), conn_(std::move(conn))
    {
    }

    void Execute() override
    {
        if (conn_)
            conn_->reset();
    }

    void OnOK() override { deferred_.Resolve(Env().Undefined()); }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<ConnectionState> conn_;
    int code_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Connection factory (used by SeekdbInstance::Connect)
// ---------------------------------------------------------------------------

Napi::Value MakeConnectionAsync(Napi::Env env, std::shared_ptr<InstanceState> instance,
                                std::string database, bool autocommit)
{
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker =
        new ConnectWorker(env, deferred, std::move(instance), std::move(database), autocommit);
    worker->Queue();
    return deferred.Promise();
}

// ---------------------------------------------------------------------------
// Connection wrapper
// ---------------------------------------------------------------------------

Napi::FunctionReference Connection::constructor;

void Connection::Adopt(std::shared_ptr<ConnectionState> state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(state);
}

std::shared_ptr<ConnectionState> Connection::require_open(Napi::Env env)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || !state_->raw())
        throw MakeError(env, SEEKDB_INVALID_ARGUMENT, "connection is closed");
    return state_;
}

void Connection::Init(Napi::Env env, Napi::Object exports)
{
    Napi::Function func =
        DefineClass(env, "Connection",
                    {
                        InstanceMethod("cursor", &Connection::Cursor),
                        InstanceMethod("begin", &Connection::Begin),
                        InstanceMethod("commit", &Connection::Commit),
                        InstanceMethod("rollback", &Connection::Rollback),
                        InstanceMethod("close", &Connection::Close),
                        InstanceAccessor("closed", &Connection::GetClosed, nullptr),
                    });
    constructor = Napi::Persistent(func);
    exports.Set("Connection", func);
    // Reset the module-static constructor when the env is torn down. Without
    // this the static FunctionReference is destroyed after V8 has already
    // shut down, which SIGSEGVs on exit (Node 18/20).
    env.AddCleanupHook([]() { Connection::constructor.Reset(); });
}

Napi::Value Connection::Cursor(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto cursor_state = std::make_shared<CursorState>(std::move(state));
    return MakeCursor(env, std::move(cursor_state));
}

Napi::Value Connection::Begin(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new TrxWorker(env, deferred, std::move(state), TrxWorker::Op::Begin);
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Connection::Commit(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new TrxWorker(env, deferred, std::move(state), TrxWorker::Op::Commit);
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Connection::Rollback(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    auto state = require_open(env);
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new TrxWorker(env, deferred, std::move(state), TrxWorker::Op::Rollback);
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Connection::Close(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::shared_ptr<ConnectionState> to_close;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to_close = std::move(state_);
    }
    if (!to_close) {
        auto deferred = Napi::Promise::Deferred::New(env);
        deferred.Resolve(env.Undefined());
        return deferred.Promise();
    }
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new DisconnectWorker(env, deferred, std::move(to_close));
    worker->Queue();
    return deferred.Promise();
}

Napi::Value Connection::GetClosed(const Napi::CallbackInfo &info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Napi::Boolean::New(info.Env(), !state_);
}

} // namespace seekdb