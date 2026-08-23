package org.neatvnc.e2e;

import java.io.IOException;
import java.time.Duration;

import org.testcontainers.containers.GenericContainer;
import org.testcontainers.containers.wait.strategy.Wait;
import org.testcontainers.containers.wait.strategy.WaitAllStrategy;
import org.testcontainers.utility.DockerImageName;

/**
 * Weston's VNC backend running against the patched neatvnc, with websockify and
 * noVNC in front.
 *
 * <p>Serves plain HTTP: the browser marks the origin trusted instead, which
 * avoids managing a certificate. That is safe here and nowhere else -- noVNC
 * only offers H.264 in a secure context, so a real deployment needs TLS.
 */
public class WestonContainer extends GenericContainer<WestonContainer> {

    public static final int WEB_PORT = 6080;
    public static final String NETWORK_ALIAS = "weston";
    public static final String PASSWORD = "e2e-secret";

    /** Weston's PAM check rejects any username but the one it runs as. */
    public static final String USERNAME = "root";

    private static final String WESTON_LOG = "/tmp/weston.log";

    public WestonContainer(String image, String h264Codec) {
        super(DockerImageName.parse(image));

        withNetworkAliases(NETWORK_ALIAS);
        withExposedPorts(WEB_PORT);

        withEnv("VNC_PASSWORD", PASSWORD);
        withEnv("WEB_TLS", "0");

        // A fullscreen animated client. The desktop shell leaves its windows
        // unmapped headless, and an H.264 test of a still screen proves
        // nothing. Software rendered, because with no GPU weston runs the
        // pixman renderer and no EGL client can start.
        withEnv("WESTON_SHELL", "kiosk");
        withEnv("WESTON_DEMO_CLIENTS", "weston-simple-shm");

        withEnv("NEATVNC_H264_ENCODER", "nvenc");
        withEnv("NEATVNC_H264_NVENC_CODEC", h264Codec);
        withEnv("NVNC_LOG_LEVEL", "debug");

        /*
         * The log line and the listening port are both needed. The entrypoint
         * echoes "noVNC on http..." and only then execs websockify, so the
         * message alone lets a test race ahead of the port being bound and get
         * a connection refused -- which it does whenever the test does not
         * happen to spend a while doing something else first.
         */
        waitingFor(new WaitAllStrategy()
            .withStrategy(Wait.forLogMessage(".*noVNC on http.*", 1))
            .withStrategy(Wait.forListeningPort())
            .withStartupTimeout(Duration.ofMinutes(2)));
    }

    /**
     * Everything the server side had to say.
     *
     * <p>The two halves land in different places: weston writes its own
     * messages to the file named by --log, while neatvnc writes to stderr,
     * which ends up on the container's stdout. The lines naming the chosen
     * encoder and encoding are neatvnc's.
     */
    public String serverLog() {
        return westonLog() + "\n----- container stdout -----\n" + getLogs();
    }

    /** Weston's own log file, which does not contain neatvnc's messages. */
    public String westonLog() {
        try {
            return execInContainer("cat", WESTON_LOG).getStdout();
        } catch (IOException | InterruptedException e) {
            Thread.currentThread().interrupt();
            return "could not read " + WESTON_LOG + ": " + e;
        }
    }

    /** The URL as seen from another container on the same network. */
    public String internalOrigin() {
        return "http://" + NETWORK_ALIAS + ":" + WEB_PORT;
    }
}
