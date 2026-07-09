#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "seekdb.h"
}

namespace py = pybind11;

constexpr const char *kPublicModule = "pylibseekdb";

static py::object &date_class()
{
    static py::object o = py::module_::import("datetime").attr("date");
    return o;
}
static py::object &datetime_class()
{
    static py::object o = py::module_::import("datetime").attr("datetime");
    return o;
}
static py::object &decimal_class()
{
    static py::object o = py::module_::import("decimal").attr("Decimal");
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

class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection() : c_(nullptr) {}
    explicit Connection(SeekdbConnection c) : c_(c) {}
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
    }
    void begin() { SDB_CHECK(seekdb_trx_begin(c_)); }
    void commit() { SDB_CHECK(seekdb_trx_commit(c_)); }
    void rollback() { SDB_CHECK(seekdb_trx_rollback(c_)); }

    SeekdbConnection raw() const { return c_; }

  private:
    SeekdbConnection c_;
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

    py::object fetchone()
    {
        if (!result_)
            return py::none();
        if (seekdb_result_next(result_) != SEEKDB_SUCCESS)
            return py::none();
        return build_row();
    }

    std::vector<py::tuple> fetchall()
    {
        std::vector<py::tuple> rows;
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

    py::tuple build_row()
    {
        int64_t ncol = 0;
        SDB_CHECK(seekdb_result_column_count(result_, &ncol));
        py::tuple t(ncol);
        for (int64_t i = 0; i < ncol; ++i) {
            t[i] = get_value(i);
        }
        return t;
    }

    py::object get_value(int64_t idx)
    {
        SeekdbTypeId t = SEEKDB_TYPE_NULL;
        SDB_CHECK(seekdb_result_column_type_id(result_, idx, &t));

        // Probe the raw cell first so SQL NULL maps to py::none() regardless
        // of column type — the typed getters return 0/0.0 for NULL silently.
        const char *data = nullptr;
        size_t len = 0;
        int is_null = 0;
        SDB_CHECK(seekdb_result_get_str(result_, idx, &data, &len, &is_null));
        if (is_null)
            return py::none();

        switch (t) {
        case SEEKDB_TYPE_NULL:
            return py::none();
        case SEEKDB_TYPE_INT64: {
            int64_t v = 0;
            SDB_CHECK(seekdb_result_get_int64(result_, idx, &v));
            return py::int_(v);
        }
        case SEEKDB_TYPE_UINT64: {
            uint64_t v = 0;
            SDB_CHECK(seekdb_result_get_uint64(result_, idx, &v));
            return py::int_(v);
        }
        case SEEKDB_TYPE_FLOAT: {
            double v = 0.0;
            SDB_CHECK(seekdb_result_get_float(result_, idx, &v));
            return py::float_(v);
        }
        case SEEKDB_TYPE_DECIMAL:
            return decimal_class()(py::str(data, len));
        case SEEKDB_TYPE_DATE: {
            int y = 0, m = 0, d = 0;
            if (std::sscanf(std::string(data, len).c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
                return py::str(data, len);
            }
            return date_class()(y, m, d);
        }
        case SEEKDB_TYPE_DATETIME:
        case SEEKDB_TYPE_TIMESTAMP: {
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, us = 0;
            int n = std::sscanf(std::string(data, len).c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d,
                                &h, &mi, &s, &us);
            if (n < 6)
                return py::str(data, len);
            return datetime_class()(y, mo, d, h, mi, s, us);
        }
        case SEEKDB_TYPE_VARCHAR:
            return py::str(data, len);
        default:
            throw std::runtime_error("Cursor.get_value: unknown column type id=" +
                                     std::to_string(static_cast<int>(t)));
        }
    }

    std::shared_ptr<Connection> conn_; // keeps connection alive while cursor uses it
    SeekdbResult result_;
};

Cursor Connection::cursor() { return Cursor(shared_from_this()); }

static SeekdbHandle handle = nullptr;

void open(const std::string &db_dir)
{
    if (handle)
        return;
    SDB_CHECK(seekdb_open(db_dir.c_str(), 0, &handle));
}

std::shared_ptr<Connection> connect(const std::string &database, bool autocommit)
{
    if (!handle) {
        throw std::runtime_error("seekdb not opened — call open() or open_with_service() first");
    }
    SeekdbConnection c = nullptr;
    int rc = seekdb_connect(handle, database.c_str(), autocommit, &c);
    if (rc != SEEKDB_SUCCESS) {
        int srv_errno = 0;
        const char *srv_msg = nullptr;
        if (c)
            seekdb_last_error(c, &srv_errno, &srv_msg);
        std::string msg = (srv_msg && *srv_msg) ? srv_msg : "seekdb_connect failed";
        if (c)
            seekdb_disconnect(c);
        throw SeekdbError(srv_errno, msg);
    }
    return std::make_shared<Connection>(c);
}

void close()
{
    if (handle) {
        seekdb_close(handle);
        handle = nullptr;
    }
}

} // namespace seekdb

PYBIND11_MODULE(pylibseekdb, m)
{
    m.doc() = "Python bindings for seekdb-driver (out-of-process MySQL-compatible client). "
              "Surface mirrors seekdb's ob_embed_impl.cpp.";
    m.attr("__version__") = "0.1.0";

    auto seekdb_error = py::register_exception<SeekdbError>(m, "SeekdbError", PyExc_RuntimeError);
    seekdb_error.attr("__module__") = kPublicModule;

    const char *default_service_path = "./seekdb.db";

    m.def("open", &seekdb::open, py::arg("db_dir") = default_service_path, "open db");

    m.def("connect", &seekdb::connect, py::arg("database") = "test", py::arg("autocommit") = false,
          "connect seekdb");

    auto connection_class =
        py::class_<seekdb::Connection, std::shared_ptr<seekdb::Connection>>(m, "Connection");
    connection_class.attr("__module__") = kPublicModule;
    connection_class.def(py::init<>())
        .def("cursor", &seekdb::Connection::cursor)
        .def("close", &seekdb::Connection::reset)
        .def("begin", &seekdb::Connection::begin, py::call_guard<py::gil_scoped_release>())
        .def("commit", &seekdb::Connection::commit, py::call_guard<py::gil_scoped_release>())
        .def("rollback", &seekdb::Connection::rollback, py::call_guard<py::gil_scoped_release>());

    auto cursor_class = py::class_<seekdb::Cursor>(m, "Cursor");
    cursor_class.attr("__module__") = kPublicModule;
    cursor_class.def("execute", &seekdb::Cursor::execute, py::call_guard<py::gil_scoped_release>())
        .def("fetchone", &seekdb::Cursor::fetchone)
        .def("fetchall", &seekdb::Cursor::fetchall)
        .def("close", &seekdb::Cursor::close);

    py::object atexit = py::module::import("atexit");
    atexit.attr("register")(py::cpp_function(&seekdb::close));
}
