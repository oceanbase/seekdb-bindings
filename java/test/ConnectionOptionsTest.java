import com.oceanbase.seekdb.ConnectionOptions;

public final class ConnectionOptionsTest {
    public static void main(String[] args) {
        ConnectionOptions tcp = new ConnectionOptions("tcp", 2881, "db.example", null, null, "root");
        if (!"db.example".equals(tcp.host) || tcp.port != 2881
                || tcp.unix_socket != null || tcp.named_pipe != null) {
            throw new AssertionError("TCP host was not preserved");
        }
        ConnectionOptions unix = new ConnectionOptions("unix_socket", 0, null, "/proc/self/fd/42/sql.sock", null, "root");
        if (unix.named_pipe != null || unix.host != null
                || !"/proc/self/fd/42/sql.sock".equals(unix.unix_socket) || unix.port != 0) {
            throw new AssertionError("Unix fields not preserved");
        }
        ConnectionOptions pipe = new ConnectionOptions("named_pipe", 0, null, null, "\\\\.\\pipe\\seekdb", "root");
        if (pipe.unix_socket != null || pipe.host != null || !pipe.named_pipe.equals("\\\\.\\pipe\\seekdb")) {
            throw new AssertionError("Named pipe fields not preserved");
        }
        System.out.println("COMMON_JAVA_OK tcp/unix_socket/named_pipe; no Android SDK");
    }
}
