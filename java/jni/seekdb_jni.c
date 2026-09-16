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

static void throw_illegal_argument(JNIEnv *env, const char *message)
{
    jclass cls = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
    if (cls)
        (*env)->ThrowNew(env, cls, message);
}

static char *java_string(JNIEnv *env, jstring value)
{
    if (!value)
        return NULL;

    jclass string_class = (*env)->GetObjectClass(env, value);
    if (!string_class)
        return NULL;
    jmethodID get_bytes =
        (*env)->GetMethodID(env, string_class, "getBytes", "(Ljava/lang/String;)[B");
    if (!get_bytes) {
        (*env)->DeleteLocalRef(env, string_class);
        return NULL;
    }
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    if (!utf8) {
        (*env)->DeleteLocalRef(env, string_class);
        return NULL;
    }
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, value, get_bytes, utf8);
    (*env)->DeleteLocalRef(env, utf8);
    (*env)->DeleteLocalRef(env, string_class);
    if (!bytes)
        return NULL;

    jsize length = (*env)->GetArrayLength(env, bytes);
    char *copy = malloc((size_t)length + 1);
    if (!copy) {
        (*env)->DeleteLocalRef(env, bytes);
        throw_runtime(env, "UTF-8 string allocation failed");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, bytes, 0, length, (jbyte *)copy);
    (*env)->DeleteLocalRef(env, bytes);
    if ((*env)->ExceptionCheck(env)) {
        free(copy);
        return NULL;
    }
    if (memchr(copy, '\0', (size_t)length)) {
        free(copy);
        throw_illegal_argument(env, "Java string contains an embedded NUL byte");
        return NULL;
    }
    copy[length] = '\0';
    return copy;
}

static jstring new_java_string(JNIEnv *env, const char *value)
{
    if (!value)
        return NULL;
    size_t length = strlen(value);
    if (length > INT32_MAX) {
        throw_runtime(env, "native UTF-8 string is too long");
        return NULL;
    }

    jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
    if (!bytes)
        return NULL;
    (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte *)value);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->DeleteLocalRef(env, bytes);
        return NULL;
    }
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!string_class) {
        (*env)->DeleteLocalRef(env, bytes);
        return NULL;
    }
    jmethodID ctor = (*env)->GetMethodID(env, string_class, "<init>", "([BLjava/lang/String;)V");
    if (!ctor) {
        (*env)->DeleteLocalRef(env, string_class);
        (*env)->DeleteLocalRef(env, bytes);
        return NULL;
    }
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    if (!utf8) {
        (*env)->DeleteLocalRef(env, string_class);
        (*env)->DeleteLocalRef(env, bytes);
        return NULL;
    }
    jstring result = (jstring)(*env)->NewObject(env, string_class, ctor, bytes, utf8);
    (*env)->DeleteLocalRef(env, utf8);
    (*env)->DeleteLocalRef(env, string_class);
    (*env)->DeleteLocalRef(env, bytes);
    return result;
}

JNIEXPORT jlong JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeOpen(JNIEnv *env, jclass cls,
                                                                    jstring db_dir,
                                                                    jobjectArray params)
{
    (void)cls;
    char *db = java_string(env, db_dir);
    if (!db) {
        if (!(*env)->ExceptionCheck(env))
            throw_runtime(env, "dbDir is null");
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
            if (!(*env)->ExceptionCheck(env))
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
    jstring transport = new_java_string(env, options.transport);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    jstring host = new_java_string(env, options.host);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    jstring unix_socket = new_java_string(env, options.unix_socket);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    jstring named_pipe = new_java_string(env, options.named_pipe);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    jstring user = new_java_string(env, options.user);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    jobject object = (*env)->NewObject(env, result, ctor, transport, (jint)options.port, host,
                                       unix_socket, named_pipe, user);
    if (transport)
        (*env)->DeleteLocalRef(env, transport);
    if (host)
        (*env)->DeleteLocalRef(env, host);
    if (unix_socket)
        (*env)->DeleteLocalRef(env, unix_socket);
    if (named_pipe)
        (*env)->DeleteLocalRef(env, named_pipe);
    if (user)
        (*env)->DeleteLocalRef(env, user);
    return object;
}

JNIEXPORT void JNICALL Java_com_oceanbase_seekdb_SeekDB_nativeClose(JNIEnv *env, jclass cls,
                                                                    jlong value)
{
    (void)env;
    (void)cls;
    if (value)
        seekdb_close((SeekdbHandle)(intptr_t)value);
}
