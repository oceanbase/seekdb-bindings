#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "seekdb.h"

static void throw_runtime(JNIEnv *env, const char *message)
{
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls)
        (*env)->ThrowNew(env, cls, message);
}

static char *java_string(JNIEnv *env, jstring value)
{
    if (!value)
        return NULL;
    const char *utf = (*env)->GetStringUTFChars(env, value, NULL);
    if (!utf)
        return NULL;
    char *copy = strdup(utf);
    (*env)->ReleaseStringUTFChars(env, value, utf);
    return copy;
}

JNIEXPORT void JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeSetBinaryPath(JNIEnv *env, jclass cls,
                                                                            jstring path)
{
    (void)cls;
    char *copy = java_string(env, path);
    if (!copy) {
        throw_runtime(env, "binary path is null or allocation failed");
        return;
    }
    int rc = seekdb_set_binary_path(copy);
    free(copy);
    if (rc != SEEKDB_SUCCESS)
        throw_runtime(env, "seekdb_set_binary_path failed");
}

JNIEXPORT jlong JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeOpen(JNIEnv *env, jclass cls,
                                                                    jstring db_dir,
                                                                    jobjectArray params)
{
    (void)cls;
    char *db = java_string(env, db_dir);
    if (!db) {
        throw_runtime(env, "dbDir is null or allocation failed");
        return 0;
    }

    jsize count = params ? (*env)->GetArrayLength(env, params) : 0;
    char **values = calloc((size_t)count + 1, sizeof(*values));
    if (!values) {
        free(db);
        throw_runtime(env, "parameter allocation failed");
        return 0;
    }
    for (jsize i = 0; i < count; ++i) {
        jstring item = (jstring)(*env)->GetObjectArrayElement(env, params, i);
        values[i] = java_string(env, item);
        (*env)->DeleteLocalRef(env, item);
        if (!values[i]) {
            for (jsize j = 0; j < i; ++j)
                free(values[j]);
            free(values);
            free(db);
            throw_runtime(env, "parameter conversion failed");
            return 0;
        }
    }

    SeekdbHandle handle = NULL;
    int rc = seekdb_open(db, count ? (const char **)values : NULL, &handle);
    for (jsize i = 0; i < count; ++i)
        free(values[i]);
    free(values);
    free(db);
    if (rc != SEEKDB_SUCCESS) {
        char message[2304];
        snprintf(message, sizeof(message), "seekdb_open failed (rc=%d): %s", rc,
                 seekdb_last_open_error());
        throw_runtime(env, message);
        return 0;
    }
    return (jlong)(intptr_t)handle;
}

JNIEXPORT jobject JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeConnectionOptions(JNIEnv *env,
                                                                                   jclass cls,
                                                                                   jlong value)
{
    (void)cls;
    SeekdbConnectionOptions options = {0};
    if (seekdb_connection_options((SeekdbHandle)(intptr_t)value, &options) != SEEKDB_SUCCESS) {
        throw_runtime(env, "seekdb_connection_options failed");
        return NULL;
    }
    jclass result = (*env)->FindClass(env, "com/oceanbase/seekdb/ConnectionOptions");
    if (!result) {
        return NULL;
    }
    jmethodID ctor = (*env)->GetMethodID(env, result, "<init>",
                                         "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/"
                                         "String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!ctor) {
        return NULL;
    }
    jstring transport = options.transport ? (*env)->NewStringUTF(env, options.transport) : NULL;
    jstring host = options.host ? (*env)->NewStringUTF(env, options.host) : NULL;
    jstring unix_socket =
        options.unix_socket ? (*env)->NewStringUTF(env, options.unix_socket) : NULL;
    jstring named_pipe = options.named_pipe ? (*env)->NewStringUTF(env, options.named_pipe) : NULL;
    jstring user = options.user ? (*env)->NewStringUTF(env, options.user) : NULL;
    if ((*env)->ExceptionCheck(env)) {
        return NULL;
    }
    return (*env)->NewObject(env, result, ctor, transport, (jint)options.port, host, unix_socket,
                             named_pipe, user);
}

JNIEXPORT void JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeClose(JNIEnv *env, jclass cls,
                                                                    jlong value)
{
    (void)env;
    (void)cls;
    if (value)
        seekdb_close((SeekdbHandle)(intptr_t)value);
}
