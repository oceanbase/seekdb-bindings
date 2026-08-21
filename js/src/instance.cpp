#include "internal.hpp"

#include <filesystem>
#include <unistd.h>
#include <unordered_map>

namespace seekdb {

Napi::FunctionReference SeekdbInstance::constructor;

// ---------------------------------------------------------------------------
// InstanceState
// ---------------------------------------------------------------------------

InstanceState::InstanceState(const std::string &db_dir) : handle_(nullptr)
{
    const int rc = seekdb_open(db_dir.c_str(), nullptr, &handle_);
    if (rc != SEEKDB_SUCCESS) {
        if (handle_)
            seekdb_close(handle_);
        throw SeekdbErrorC(rc, "seekdb_open");
    }
}

InstanceState::~InstanceState()
{
    if (handle_)
        seekdb_close(handle_);
}

void InstanceState::close_checked()
{
    if (handle_) {
        SDB_CHECK(seekdb_close(handle_));
        handle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Instance sharing (per normalized db_dir; aligned with Python bindings.cpp)
// ---------------------------------------------------------------------------

std::string NormalizeDbDir(const std::string &db_dir)
{
    return std::filesystem::absolute(std::filesystem::path(db_dir)).lexically_normal().string();
}

static std::unordered_map<std::string, std::weak_ptr<InstanceState>> g_instance_states;
static std::mutex g_instance_states_mutex;

std::shared_ptr<InstanceState> GetOrCreateInstance(const std::string &db_dir)
{
    const std::string normalized = NormalizeDbDir(db_dir);
    std::lock_guard<std::mutex> lock(g_instance_states_mutex);

    const auto it = g_instance_states.find(normalized);
    if (it != g_instance_states.end()) {
        if (std::shared_ptr<InstanceState> instance = it->second.lock())
            return instance;
        g_instance_states.erase(it);
    }

    std::shared_ptr<InstanceState> instance = std::make_shared<InstanceState>(normalized);
    g_instance_states.emplace(normalized, instance);
    return instance;
}

void ReleaseInstance(const std::string &db_dir, InstanceState *state)
{
    const std::string normalized = NormalizeDbDir(db_dir);
    std::lock_guard<std::mutex> lock(g_instance_states_mutex);
    const auto it = g_instance_states.find(normalized);
    if (it != g_instance_states.end()) {
        std::shared_ptr<InstanceState> registered = it->second.lock();
        if (registered.get() == state)
            g_instance_states.erase(it);
    }
}

// ---------------------------------------------------------------------------
// SeekdbInstance wrapper
// ---------------------------------------------------------------------------

namespace {

class OpenWorker : public Napi::AsyncWorker {
  public:
    OpenWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string db_dir)
        : Napi::AsyncWorker(env), deferred_(deferred), db_dir_(std::move(db_dir))
    {
    }

    void Execute() override
    {
        try {
            instance_ = GetOrCreateInstance(db_dir_);
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        Napi::Object obj = SeekdbInstance::constructor.New({});
        SeekdbInstance *inst = Napi::ObjectWrap<SeekdbInstance>::Unwrap(obj);
        inst->Adopt(db_dir_, instance_);
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::string db_dir_;
    std::shared_ptr<InstanceState> instance_;
    int code_ = 0;
};

class ConnectionOptionsWorker : public Napi::AsyncWorker {
  public:
    ConnectionOptionsWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                            std::shared_ptr<InstanceState> instance)
        : Napi::AsyncWorker(env), deferred_(deferred), instance_(std::move(instance))
    {
    }

    void Execute() override
    {
        try {
            if (!instance_ || !instance_->raw())
                throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT, "seekdb instance is closed");
            SeekdbConnectionOptions options = {};
            SDB_CHECK(seekdb_connection_options(instance_->raw(), &options));
            transport_ = options.transport ? options.transport : "";
            port_ = options.port;
            endpoint_ = options.endpoint ? options.endpoint : "";
            user_ = options.user ? options.user : "";
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
            SeekdbConnectionOptions opts = {};
            opts.transport = transport_.c_str();
            opts.port = port_;
            opts.endpoint = endpoint_.c_str();
            opts.user = user_.c_str();
            deferred_.Resolve(ConnectionOptionsToNapi(env, opts));
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
    std::shared_ptr<InstanceState> instance_;
    std::string transport_;
    unsigned int port_ = 0;
    std::string endpoint_;
    std::string user_;
    int code_ = 0;
};

class CloseWorker : public Napi::AsyncWorker {
  public:
    CloseWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::shared_ptr<InstanceState> instance,
                std::string db_dir, bool erase)
        : Napi::AsyncWorker(env), deferred_(deferred), instance_(std::move(instance)),
          db_dir_(std::move(db_dir)), erase_(erase)
    {
    }

    void Execute() override
    {
        try {
            if (instance_)
                instance_->close_checked();
        }
        catch (const SeekdbErrorC &e) {
            code_ = e.code();
            SetError(e.what());
        }
    }

    void OnOK() override
    {
        if (erase_ && instance_)
            ReleaseInstance(db_dir_, instance_.get());
        deferred_.Resolve(Env().Undefined());
    }

    void OnError(const Napi::Error &e) override
    {
        deferred_.Reject(MakeError(Env(), code_, e.Message()).Value());
    }

  private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<InstanceState> instance_;
    std::string db_dir_;
    bool erase_;
    int code_ = 0;
};

} // namespace

void SeekdbInstance::Init(Napi::Env env, Napi::Object exports)
{
    Napi::Function func = DefineClass(
        env, "SeekdbInstance",
        {
            InstanceMethod("connect", &SeekdbInstance::Connect),
            InstanceMethod("connectionOptions", &SeekdbInstance::ConnectionOptions),
            InstanceMethod("close", &SeekdbInstance::Close),
            InstanceAccessor("closed", &SeekdbInstance::GetClosed, nullptr),
            InstanceAccessor("dbDir", &SeekdbInstance::GetDbDir, nullptr),
        });
    constructor = Napi::Persistent(func);
    exports.Set("SeekdbInstance", func);
}

Napi::Value SeekdbInstance::Open(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::string db_dir = info.Length() > 0 && info[0].IsString()
                             ? info[0].As<Napi::String>().Utf8Value()
                             : "./seekdb.db";
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new OpenWorker(env, deferred, std::move(db_dir));
    worker->Queue();
    return deferred.Promise();
}

void SeekdbInstance::Adopt(std::string db_dir, std::shared_ptr<InstanceState> instance)
{
    db_dir_ = std::move(db_dir);
    instance_ = std::move(instance);
}

Napi::Value SeekdbInstance::Connect(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::string database = info.Length() > 0 && info[0].IsString()
                               ? info[0].As<Napi::String>().Utf8Value()
                               : "test";
    bool autocommit = info.Length() > 1 && info[1].IsBoolean() ? info[1].As<Napi::Boolean>().Value()
                                                               : false;
    std::shared_ptr<InstanceState> instance;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance_)
            throw MakeError(env, 0, "seekdb instance is closed");
        instance = instance_;
    }
    return MakeConnectionAsync(env, std::move(instance), std::move(database), autocommit);
}

Napi::Value SeekdbInstance::ConnectionOptions(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::shared_ptr<InstanceState> instance;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance_)
            throw MakeError(env, 0, "seekdb instance is closed");
        instance = instance_;
    }
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new ConnectionOptionsWorker(env, deferred, std::move(instance));
    worker->Queue();
    return deferred.Promise();
}

Napi::Value SeekdbInstance::Close(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::shared_ptr<InstanceState> to_close;
    bool erase = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance_) {
            auto deferred = Napi::Promise::Deferred::New(env);
            deferred.Resolve(env.Undefined());
            return deferred.Promise();
        }
        if (instance_.use_count() == 1) {
            to_close = instance_;
            erase = true;
        }
        instance_.reset();
    }
    auto deferred = Napi::Promise::Deferred::New(env);
    auto *worker = new CloseWorker(env, deferred, std::move(to_close), db_dir_, erase);
    worker->Queue();
    return deferred.Promise();
}

Napi::Value SeekdbInstance::GetClosed(const Napi::CallbackInfo &info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Napi::Boolean::New(info.Env(), !instance_);
}

Napi::Value SeekdbInstance::GetDbDir(const Napi::CallbackInfo &info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Napi::String::New(info.Env(), db_dir_);
}

} // namespace seekdb
