package com.oceanbase.seekdb;

/** A libseekdb-managed embedded instance, independent of any client driver. */
public final class EmbeddedSeekDB implements AutoCloseable {
    private long handle;

    EmbeddedSeekDB(long handle) {
        this.handle = handle;
    }

    public ConnectionOptions connectionOptions() {
        ensureOpen();
        return SeekDB.nativeConnectionOptions(handle);
    }

    @Override
    public void close() {
        if (handle != 0) {
            SeekDB.nativeClose(handle);
            handle = 0;
        }
    }

    private void ensureOpen() {
        if (handle == 0) {
            throw new IllegalStateException("SeekDB is closed");
        }
    }
}
