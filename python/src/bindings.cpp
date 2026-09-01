#include <nanobind/nanobind.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "seekdb.h"
}

namespace nb = nanobind;

constexpr const char *kPublicModule = "pylibseekdb";

static nb::object &date_class()
{
    static nb::object o = nb::module_::import_("datetime").attr("date");
    return o;
}
static nb::object &datetime_class()
{
    static nb::object o = nb::module_::import_("datetime").attr("datetime");
    return o;
}
static nb::object &decimal_class()
{
    static nb::object o = nb::module_::import_("decimal").attr("Decimal");
    return o;
}

// ---------- error translation ----------

class SeekdbError : public std::runtime_error {
  public:
    SeekdbError(int code, const char *where)
        : std::runtime_error(std::string(where) + " failed: code=" + std::to_string(code)),
          code_(code)
    {
    }
    SeekdbError(int code, std::string msg) : std::runtime_error(std::move(msg)), code_(code) {}
    int code() const { return code_; }

  private:
    int code_;
};

#define SDB_CHECK(expr)                                                                            \
    do {                                                                                           \
        int _rc = (expr);                                                                          \
        if (_rc != SEEKDB_SUCCESS)                                                                 \
            throw SeekdbError(_rc, #expr);                                                         \
    } while (0)

namespace seekdb {

class Cursor;

class InstanceState {
  public:
    explicit InstanceState(const std::string &db_dir) : handle_(nullptr)
    {
        const int rc = seekdb_open(db_dir.c_str(), nullptr, &handle_);
        if (rc != SEEKDB_SUCCESS) {
            if (handle_)
                seekdb_close(handle_);
            throw SeekdbError(rc, "seekdb_open");
        }
    }
    ~InstanceState()
    {
        if (handle_)
            seekdb_close(handle_);
    }
    InstanceState(const InstanceState &) = delete;
    InstanceState &operator=(const InstanceState &) = delete;

    SeekdbHandle raw() const { return handle_; }

    void close_checked()
    {
        if (handle_) {
            SDB_CHECK(seekdb_close(handle_));
            handle_ = nullptr;
        }
    }

  private:
    SeekdbHandle handle_;
};

class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection() : c_(nullptr) {}
    Connection(SeekdbConnection c, std::shared_ptr<InstanceState> instance)
        : c_(c), instance_(std::move(instance))
    {
    }
    ~Connection() { reset(); }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Cursor cursor(); // out-of-line; needs Cursor's full type

    void reset()
    {
        if (c_) {
            seekdb_disconnect(c_);
            c_ = nullptr;
        }
        instance_.reset();
    }
    void begin() { SDB_CHECK(seekdb_trx_begin(c_)); }
    void commit() { SDB_CHECK(seekdb_trx_commit(c_)); }
    void rollback() { SDB_CHECK(seekdb_trx_rollback(c_)); }

    SeekdbConnection raw() const { return c_; }

  private:
    SeekdbConnection c_;
    std::shared_ptr<InstanceState> instance_;
};

class Cursor {
  public:
    Cursor() : result_(nullptr) {}
    explicit Cursor(std::shared_ptr<Connection> conn) : conn_(std::move(conn)), result_(nullptr) {}
    ~Cursor() { close(); }
    Cursor(const Cursor &) = delete;
    Cursor &operator=(const Cursor &) = delete;
    Cursor(Cursor &&o) noexcept : conn_(std::move(o.conn_)), result_(o.result_)
    {
        o.result_ = nullptr;
    }

    uint64_t execute(const std::string &sql)
    {
        if (!conn_ || !conn_->raw()) {
            throw std::runtime_error("Cursor.execute: no connection");
        }
        free_result();
        int rc =
            seekdb_query(conn_->raw(), sql.c_str(), static_cast<int64_t>(sql.size()), &result_);
        if (rc != SEEKDB_SUCCESS) {
            int srv_errno = 0;
            const char *srv_msg = nullptr;
            seekdb_last_error(conn_->raw(), &srv_errno, &srv_msg);
            throw SeekdbError(srv_errno, srv_msg ? srv_msg : "");
        }
        int64_t n = 0;
        if (seekdb_result_row_count(result_, &n) != SEEKDB_SUCCESS)
            n = 0;
        return static_cast<uint64_t>(n);
    }

    nb::object fetchone()
    {
        if (!result_)
            return nb::none();
        if (seekdb_result_next(result_) != SEEKDB_SUCCESS)
            return nb::none();
        return build_row();
    }

    std::vector<nb::tuple> fetchall()
    {
        std::vector<nb::tuple> rows;
        if (!result_)
            return rows;
        while (seekdb_result_next(result_) == SEEKDB_SUCCESS) {
            rows.push_back(build_row());
        }
        return rows;
    }

    void close() { free_result(); }

  private:
    void free_result()
    {
        if (result_) {
            seekdb_result_free(result_);
            result_ = nullptr;
        }
    }

    nb::tuple build_row()
    {
        int64_t ncol = 0;
        SDB_CHECK(seekdb_result_column_count(result_, &ncol));

        std::vector<nb::object> cells;
        cells.reserve(static_cast<size_t>(ncol));
        for (int64_t i = 0; i < ncol; ++i)
            cells.push_back(get_value(i));

        Py_ssize_t n = static_cast<Py_ssize_t>(cells.size());
        PyObject *raw = PyTuple_New(n);
        if (!raw)
            throw nb::python_error();
        nb::tuple row = nb::steal<nb::tuple>(raw);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *item = cells[i].release().ptr();
            if (PyTuple_SetItem(row.ptr(), i, item) < 0)
                throw nb::python_error();
        }
        return row;
    }

    nb::object get_value(int64_t idx)
    {
        SeekdbTypeId t = SEEKDB_TYPE_NULL;
        SDB_CHECK(seekdb_result_column_type_id(result_, idx, &t));

        // Probe the raw cell first so SQL NULL maps to nb::none() regardless
        // of column type — the typed getters return 0/0.0 for NULL silently.
        const char *data = nullptr;
        size_t len = 0;
        int is_null = 0;
        SDB_CHECK(seekdb_result_get_str(result_, idx, &data, &len, &is_null));
        if (is_null)
            return nb::none();

        switch (t) {
        case SEEKDB_TYPE_NULL:
            return nb::none();
        case SEEKDB_TYPE_INT64: {
            int64_t v = 0;
            SDB_CHECK(seekdb_result_get_int64(result_, idx, &v));
            return nb::int_(v);
        }
        case SEEKDB_TYPE_UINT64: {
            uint64_t v = 0;
            SDB_CHECK(seekdb_result_get_uint64(result_, idx, &v));
            return nb::int_(v);
        }
        case SEEKDB_TYPE_FLOAT: {
            double v = 0.0;
            SDB_CHECK(seekdb_result_get_float(result_, idx, &v));
            return nb::float_(v);
        }
        case SEEKDB_TYPE_DECIMAL:
            return decimal_class()(nb::str(data, len));
        case SEEKDB_TYPE_DATE: {
            int y = 0, m = 0, d = 0;
            if (std::sscanf(std::string(data, len).c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
                return nb::str(data, len);
            }
            return date_class()(y, m, d);
        }
        case SEEKDB_TYPE_DATETIME:
        case SEEKDB_TYPE_TIMESTAMP: {
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, us = 0;
            int n = std::sscanf(std::string(data, len).c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d,
                                &h, &mi, &s, &us);
            if (n < 6)
                return nb::str(data, len);
            return datetime_class()(y, mo, d, h, mi, s, us);
        }
        case SEEKDB_TYPE_VARCHAR:
            return nb::str(data, len);
        default:
            throw std::runtime_error("Cursor.get_value: unknown column type id=" +
                                     std::to_string(static_cast<int>(t)));
        }
    }

    std::shared_ptr<Connection> conn_; // keeps connection alive while cursor uses it
    SeekdbResult result_;
};

Cursor Connection::cursor() { return Cursor(shared_from_this()); }

static std::string normalize_db_dir(const std::string &db_dir)
{
    return std::filesystem::absolute(std::filesystem::path(db_dir)).lexically_normal().string();
}

static std::unordered_map<std::string, std::weak_ptr<InstanceState>> instance_states;
static std::mutex instance_states_mutex;

class SeekdbInstance {
  public:
    static std::shared_ptr<SeekdbInstance> open(const std::string &db_dir)
    {
        const std::string normalized = normalize_db_dir(db_dir);
        std::lock_guard<std::mutex> lock(instance_states_mutex);

        const auto it = instance_states.find(normalized);
        if (it != instance_states.end()) {
            if (std::shared_ptr<InstanceState> instance = it->second.lock()) {
                return std::shared_ptr<SeekdbInstance>(
                    new SeekdbInstance(normalized, std::move(instance)));
            }
            instance_states.erase(it);
        }

        std::shared_ptr<InstanceState> instance = std::make_shared<InstanceState>(normalized);
        instance_states.emplace(normalized, instance);
        return std::shared_ptr<SeekdbInstance>(new SeekdbInstance(normalized, std::move(instance)));
    }

    ~SeekdbInstance() = default;
    SeekdbInstance(const SeekdbInstance &) = delete;
    SeekdbInstance &operator=(const SeekdbInstance &) = delete;

    std::shared_ptr<Connection> connect(const std::string &database, bool autocommit)
    {
        std::shared_ptr<InstanceState> instance = require_open();
        SeekdbConnection c = nullptr;
        int rc = seekdb_connect(instance->raw(), database.c_str(), autocommit, &c);
        if (rc != SEEKDB_SUCCESS) {
            int srv_errno = 0;
            const char *srv_msg = nullptr;
            if (c)
                seekdb_last_error(c, &srv_errno, &srv_msg);
            std::string msg = (srv_msg && *srv_msg) ? srv_msg : "seekdb_connect failed";
            int error_code = srv_errno != 0 ? srv_errno : rc;
            if (c)
                seekdb_disconnect(c);
            throw SeekdbError(error_code, msg);
        }
        return std::make_shared<Connection>(c, std::move(instance));
    }

    nb::dict connection_options()
    {
        std::string transport;
        unsigned int port = 0;
        std::string host;
        std::string user;

        {
            nb::gil_scoped_release release;
            std::shared_ptr<InstanceState> instance = require_open();
            SeekdbConnectionOptions options = {};
            SDB_CHECK(seekdb_connection_options(instance->raw(), &options));
            if (options.transport)
                transport = options.transport;
            port = options.port;
            if (options.endpoint)
                host = options.endpoint;
            if (options.user)
                user = options.user;
        }

        nb::dict result;
        result["user"] = user;
        if (transport == SEEKDB_CONNECTION_TRANSPORT_TCP) {
            if (host.empty() || port == 0)
                throw std::runtime_error("seekdb returned an invalid TCP endpoint");
            result["host"] = host;
            result["port"] = port;
        }
        else {
            throw std::runtime_error("unknown seekdb connection transport: " +
                                     (transport.empty() ? std::string("<empty>") : transport));
        }
        return result;
    }

    void close()
    {
        std::lock_guard<std::mutex> instance_lock(mutex_);
        if (!instance_)
            return;

        std::lock_guard<std::mutex> states_lock(instance_states_mutex);
        if (instance_.use_count() == 1) {
            instance_->close_checked();

            const auto it = instance_states.find(db_dir_);
            if (it != instance_states.end()) {
                std::shared_ptr<InstanceState> registered = it->second.lock();
                if (registered.get() == instance_.get())
                    instance_states.erase(it);
            }
        }
        instance_.reset();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !instance_;
    }

    std::string db_dir() const { return db_dir_; }

  private:
    SeekdbInstance(std::string db_dir, std::shared_ptr<InstanceState> instance)
        : db_dir_(std::move(db_dir)), instance_(std::move(instance))
    {
    }

    std::shared_ptr<InstanceState> require_open() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance_)
            throw std::runtime_error("seekdb instance is closed");
        return instance_;
    }

    std::string db_dir_;
    mutable std::mutex mutex_;
    std::shared_ptr<InstanceState> instance_;
};

static std::shared_ptr<SeekdbInstance> default_instance;
static std::mutex default_instance_mutex;

static std::shared_ptr<SeekdbInstance> require_default_instance()
{
    std::lock_guard<std::mutex> lock(default_instance_mutex);
    if (!default_instance || default_instance->closed())
        throw std::runtime_error("seekdb not opened — call open() or aopen() first");
    return default_instance;
}

std::shared_ptr<SeekdbInstance> open(const std::string &db_dir)
{
    std::shared_ptr<SeekdbInstance> instance = SeekdbInstance::open(db_dir);
    std::lock_guard<std::mutex> lock(default_instance_mutex);
    if (!default_instance || default_instance->closed())
        default_instance = instance;
    return instance;
}

std::shared_ptr<Connection> connect(const std::string &database, bool autocommit)
{
    return require_default_instance()->connect(database, autocommit);
}

nb::dict connection_options() { return require_default_instance()->connection_options(); }

void close()
{
    std::shared_ptr<SeekdbInstance> instance;
    {
        std::lock_guard<std::mutex> lock(default_instance_mutex);
        instance.swap(default_instance);
    }
    if (instance) {
        try {
            instance->close();
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(default_instance_mutex);
            if (!default_instance)
                default_instance = instance;
            throw;
        }
    }
}

} // namespace seekdb

NB_MODULE(pylibseekdb, m)
{
    m.doc() = "Python bindings for seekdb-driver (out-of-process MySQL-compatible client). "
              "Surface mirrors seekdb's ob_embed_impl.cpp.";
    m.attr("__version__") = "0.1.0";

    auto seekdb_error = nb::exception<SeekdbError>(m, "SeekdbError", PyExc_RuntimeError);
    seekdb_error.attr("__module__") = kPublicModule;

    const char *default_service_path = "./seekdb.db";

    m.def("open", &seekdb::open, nb::arg("db_dir") = default_service_path, "open db",
          nb::call_guard<nb::gil_scoped_release>());
    m.def("close", &seekdb::close, "close the default seekdb instance",
          nb::call_guard<nb::gil_scoped_release>());
    m.def("connection_options", &seekdb::connection_options,
          "return options for a Python MySQL-protocol driver");

    m.def("connect", &seekdb::connect, nb::arg("database") = "test", nb::arg("autocommit") = false,
          "connect the default seekdb instance", nb::call_guard<nb::gil_scoped_release>());

    auto instance_class = nb::class_<seekdb::SeekdbInstance>(m, "SeekdbInstance");
    instance_class.attr("__module__") = kPublicModule;
    instance_class
        .def("connect", &seekdb::SeekdbInstance::connect, nb::arg("database") = "test",
             nb::arg("autocommit") = false, nb::call_guard<nb::gil_scoped_release>())
        .def("connection_options", &seekdb::SeekdbInstance::connection_options)
        .def("close", &seekdb::SeekdbInstance::close, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("closed", &seekdb::SeekdbInstance::closed)
        .def_prop_ro("db_dir", &seekdb::SeekdbInstance::db_dir);

    auto connection_class = nb::class_<seekdb::Connection>(m, "Connection");
    connection_class.attr("__module__") = kPublicModule;
    connection_class.def("cursor", &seekdb::Connection::cursor)
        .def("close", &seekdb::Connection::reset)
        .def("begin", &seekdb::Connection::begin, nb::call_guard<nb::gil_scoped_release>())
        .def("commit", &seekdb::Connection::commit, nb::call_guard<nb::gil_scoped_release>())
        .def("rollback", &seekdb::Connection::rollback, nb::call_guard<nb::gil_scoped_release>());

    auto cursor_class = nb::class_<seekdb::Cursor>(m, "Cursor");
    cursor_class.attr("__module__") = kPublicModule;
    cursor_class.def("execute", &seekdb::Cursor::execute, nb::call_guard<nb::gil_scoped_release>())
        .def("fetchone", &seekdb::Cursor::fetchone)
        .def("fetchall", &seekdb::Cursor::fetchall)
        .def("close", &seekdb::Cursor::close);

    nb::object atexit = nb::module_::import_("atexit");
    atexit.attr("register")(m.attr("close"));
}
