#include "internal.hpp"

namespace seekdb {

void ReadCell(SeekdbResult result, int64_t idx, CellValue &out)
{
    out = CellValue{};
    SeekdbTypeId t = SEEKDB_TYPE_NULL;
    SDB_CHECK(seekdb_result_column_type_id(result, idx, &t));

    // Probe the raw cell first so SQL NULL maps to null regardless of column
    // type — the typed getters return 0/0.0 for NULL silently (same as Python).
    const char *data = nullptr;
    size_t len = 0;
    int is_null = 0;
    SDB_CHECK(seekdb_result_get_str(result, idx, &data, &len, &is_null));
    if (is_null) {
        out.kind = CellValue::Kind::Null;
        return;
    }

    switch (t) {
    case SEEKDB_TYPE_NULL:
        out.kind = CellValue::Kind::Null;
        break;
    case SEEKDB_TYPE_INT64: {
        int64_t v = 0;
        SDB_CHECK(seekdb_result_get_int64(result, idx, &v));
        out.kind = CellValue::Kind::Int64;
        out.i64 = v;
        break;
    }
    case SEEKDB_TYPE_UINT64: {
        uint64_t v = 0;
        SDB_CHECK(seekdb_result_get_uint64(result, idx, &v));
        out.kind = CellValue::Kind::Uint64;
        out.u64 = v;
        break;
    }
    case SEEKDB_TYPE_FLOAT: {
        double v = 0.0;
        SDB_CHECK(seekdb_result_get_float(result, idx, &v));
        out.kind = CellValue::Kind::Double;
        out.d = v;
        break;
    }
    case SEEKDB_TYPE_DECIMAL:
    case SEEKDB_TYPE_DATE:
    case SEEKDB_TYPE_DATETIME:
    case SEEKDB_TYPE_TIMESTAMP:
    case SEEKDB_TYPE_VARCHAR:
        out.kind = CellValue::Kind::String;
        out.str.assign(data, len);
        break;
    default:
        throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT,
                           "Cursor.get_value: unknown column type id=" + std::to_string((int)t));
    }
}

static Napi::Value NumberFromI64(Napi::Env env, int64_t v)
{
    if (v > 9007199254740991LL || v < -9007199254740991LL)
        return Napi::BigInt::New(env, v);
    return Napi::Number::New(env, static_cast<double>(v));
}

static Napi::Value NumberFromU64(Napi::Env env, uint64_t v)
{
    if (v > 9007199254740991ULL)
        return Napi::BigInt::New(env, v);
    return Napi::Number::New(env, static_cast<double>(v));
}

Napi::Value CellToNapi(Napi::Env env, const CellValue &cell)
{
    switch (cell.kind) {
    case CellValue::Kind::Null:
        return env.Null();
    case CellValue::Kind::Int64:
        return NumberFromI64(env, cell.i64);
    case CellValue::Kind::Uint64:
        return NumberFromU64(env, cell.u64);
    case CellValue::Kind::Double:
        return Napi::Number::New(env, cell.d);
    case CellValue::Kind::String:
        return Napi::String::New(env, cell.str);
    }
    return env.Undefined();
}

Napi::Value RowToNapi(Napi::Env env, const std::vector<CellValue> &row)
{
    Napi::Array arr = Napi::Array::New(env, row.size());
    for (size_t i = 0; i < row.size(); ++i)
        arr.Set(i, CellToNapi(env, row[i]));
    return arr;
}

Napi::Object ConnectionOptionsToNapi(Napi::Env env, const SeekdbConnectionOptions &opts)
{
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("user", Napi::String::New(env, opts.user ? opts.user : ""));
    std::string transport = opts.transport ? opts.transport : "";
    if (transport == SEEKDB_CONNECTION_TRANSPORT_TCP) {
        obj.Set("port", Napi::Number::New(env, opts.port));
    }
    else if (transport == SEEKDB_CONNECTION_TRANSPORT_UNIX_SOCKET) {
        obj.Set("unix_socket", Napi::String::New(env, opts.endpoint ? opts.endpoint : ""));
    }
    else if (transport == SEEKDB_CONNECTION_TRANSPORT_NAMED_PIPE) {
        throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT,
                           "Windows named-pipe Node.js clients are not supported");
    }
    else {
        throw SeekdbErrorC(SEEKDB_INVALID_ARGUMENT,
                           "unknown seekdb connection transport: " +
                               (transport.empty() ? std::string("<empty>") : transport));
    }
    return obj;
}

} // namespace seekdb