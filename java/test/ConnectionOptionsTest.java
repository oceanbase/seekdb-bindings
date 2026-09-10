import com.oceanbase.seekdb.ConnectionOptions;

public final class ConnectionOptionsTest {
    public static void main(String[] args) {
        ConnectionOptions tcp = new ConnectionOptions("tcp", 2881, "db.example", null, null, "root");
        if (!tcp.jdbcUrl("test").startsWith("jdbc:mariadb://db.example:2881/test?")) {
            throw new AssertionError("TCP host was not preserved");
        }
        ConnectionOptions unix = new ConnectionOptions("unix_socket", 0, null, "/proc/self/fd/42/sql.sock", null, "root");
        if (unix.named_pipe != null || unix.host != null
                || !unix.jdbcUrl("test", "example.SocketFactory").contains("socketFactory=example.SocketFactory")) {
            throw new AssertionError("Unix fields or factory not preserved");
        }
        ConnectionOptions pipe = new ConnectionOptions("named_pipe", 0, null, null, "\\\\.\\pipe\\seekdb", "root");
        if (pipe.unix_socket != null || pipe.host != null || !pipe.named_pipe.equals("\\\\.\\pipe\\seekdb")) {
            throw new AssertionError("Named pipe fields not preserved");
        }
        System.out.println("COMMON_JAVA_OK tcp/unix_socket/named_pipe; no Android SDK");
    }
}
