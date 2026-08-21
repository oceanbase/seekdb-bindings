#include "internal.hpp"

#include <cstring>

namespace seekdb {

Napi::Error MakeError(Napi::Env env, int code, const std::string &msg)
{
    Napi::Error err = Napi::Error::New(env, msg);
    err.Set("code", Napi::Number::New(env, code));
    err.Set("name", Napi::String::New(env, "SeekdbError"));
    return err;
}

void LastError(SeekdbConnection conn, int rc, int *out_errno, std::string *out_msg)
{
    int srv_errno = 0;
    const char *srv_msg = nullptr;
    if (conn)
        seekdb_last_error(conn, &srv_errno, &srv_msg);
    *out_errno = srv_errno;
    *out_msg = (srv_msg && *srv_msg) ? srv_msg
                                     : std::string("seekdb call failed (rc=") + std::to_string(rc) + ")";
}

} // namespace seekdb

Napi::Object InitAll(Napi::Env env, Napi::Object exports)
{
    seekdb::SeekdbInstance::Init(env, exports);
    seekdb::Connection::Init(env, exports);
    seekdb::Cursor::Init(env, exports);

    exports.Set("open", Napi::Function::New(env, seekdb::SeekdbInstance::Open));
    return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, InitAll)