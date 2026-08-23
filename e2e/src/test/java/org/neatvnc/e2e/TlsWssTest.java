package org.neatvnc.e2e;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Map;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.openqa.selenium.chrome.ChromeOptions;
import org.openqa.selenium.remote.RemoteWebDriver;
import org.openqa.selenium.support.ui.WebDriverWait;
import org.testcontainers.containers.BrowserWebDriverContainer;
import org.testcontainers.containers.Network;
import org.testcontainers.utility.DockerImageName;

import io.qameta.allure.Allure;

/**
 * The production shape: HTTPS page, wss:// socket, a genuine secure context.
 *
 * <p>Every other test here serves plain HTTP and marks the origin trusted with
 * a Chrome flag -- which is exactly the configuration that hid the hardcoded
 * ws:// bug for so long, because browsers also exempt loopback and
 * flag-trusted origins from mixed-content blocking. This test drops the flag:
 * the page must arrive over TLS, the WebSocket must derive wss:// from the
 * page's own scheme, and WebCodecs must exist because the context is genuinely
 * secure, not because Chrome was told to pretend.
 *
 * <p>The certificate is the entrypoint's self-signed one, so the browser is
 * told to accept invalid certificates -- that is a statement about the
 * certificate, not about the context: a page whose certificate error was
 * bypassed still loads as https with isSecureContext true, which mirrors how
 * a self-signed deployment on a trusted network actually runs.
 */
class TlsWssTest {

    /** Pinned to match selenium-java on the classpath; see WestonNoVncH264Test. */
    private static final String SELENIUM_IMAGE = "selenium/standalone-chrome:4.27.0";

    /** How long H.264 rects are given to accumulate once connected. */
    private static final Duration OBSERVE = Duration.ofSeconds(6);

    /** The stream is considered flowing at this many decoded H.264 rects. */
    private static final int MIN_H264_RECTS = 20;

    private static Network network;
    private static BrowserWebDriverContainer<?> browser;
    private static RemoteWebDriver driver;
    private static String image;
    private static String codec;

    @BeforeAll
    static void startBrowser() {
        image = System.getProperty("neatvnc.testImage", "neatvnc-nvenc-test:e2e");
        codec = System.getProperty("neatvnc.h264Codec", "libx264");

        network = Network.newNetwork();

        ChromeOptions options = new ChromeOptions();
        options.addArguments("--no-sandbox", "--disable-dev-shm-usage",
            "--window-size=1400,900",
            "--user-data-dir=/tmp/neatvnc-tls-profile");
        // The self-signed certificate, not a trust flag. Applies to the page
        // and to the wss:// socket alike.
        options.setAcceptInsecureCerts(true);

        browser = new BrowserWebDriverContainer<>(
                DockerImageName.parse(SELENIUM_IMAGE)
                    .asCompatibleSubstituteFor("selenium/standalone-chrome"))
            .withNetwork(network)
            .withCapabilities(options);
        browser.start();

        driver = (RemoteWebDriver) browser.getWebDriver();
    }

    @AfterAll
    static void stopBrowser() {
        if (browser != null) {
            browser.stop();
        }
        if (network != null) {
            network.close();
        }
    }

    @Test
    @DisplayName("over HTTPS the viewer speaks wss and H.264 flows in a real secure context")
    void h264FlowsOverWss() throws InterruptedException {
        try (WestonContainer weston = new WestonContainer(image, codec)) {
            weston.serveTls();
            weston.withEnv("WESTON_WIDTH", "640");
            weston.withEnv("WESTON_HEIGHT", "480");
            weston.withNetwork(network);
            weston.start();

            String url = weston.internalTlsOrigin() + "/e2e.html?username="
                + WestonContainer.USERNAME + "&password=" + WestonContainer.PASSWORD;
            try {
                driver.get(url);
            } catch (RuntimeException e) {
                throw new AssertionError("could not load " + url + "\n"
                    + weston.serverLog(), e);
            }

            new WebDriverWait(driver, Duration.ofSeconds(60))
                .withMessage(() -> "the viewer never connected over wss\n"
                    + state() + "\n" + weston.serverLog())
                .until(d -> Boolean.TRUE.equals(
                    ((RemoteWebDriver) d).executeScript(
                        "return !!(window.__e2e && window.__e2e.connected)")));

            // Long enough for rects to accumulate and for noVNC's WebCodecs
            // probe, which resolves asynchronously about 1.5 s after load.
            Thread.sleep(OBSERVE.toMillis());

            Map<String, Object> s = state();
            Allure.addAttachment("state after observation", "text/plain",
                s.toString());
            Allure.addAttachment("server log", "text/plain", weston.serverLog());

            assertEquals(Boolean.TRUE, s.get("secureContext"),
                () -> "the page did not load as a secure context -- H.264 "
                    + "cannot work in production without one\n" + s);
            assertTrue(String.valueOf(s.get("rfbUrl")).startsWith("wss://"),
                () -> "the RFB socket was not wss://; the page failed to "
                    + "derive the scheme from its origin\n" + s);
            assertEquals(Boolean.TRUE, s.get("webcodecsH264"),
                () -> "WebCodecs H.264 was not available despite the secure "
                    + "context\n" + s);
            assertEquals(null, s.get("error"),
                () -> "the page reported an error\n" + s + "\n"
                    + weston.serverLog());

            int h264 = ((Number) s.getOrDefault("h264Rects", 0)).intValue();
            assertTrue(h264 >= MIN_H264_RECTS,
                () -> "only " + h264 + " H.264 rects in " + OBSERVE.toSeconds()
                    + "s; expected at least " + MIN_H264_RECTS + "\n" + s
                    + "\n" + weston.serverLog());
        }
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> state() {
        return (Map<String, Object>) driver.executeScript("""
            const s = window.__e2e || {};
            return {
              secureContext: window.isSecureContext,
              rfbUrl: window.__rfb ? window.__rfb._url : null,
              connected: !!s.connected,
              webcodecsH264: !!s.webcodecsH264,
              error: s.error || null,
              h264Rects: (s.counts && s.counts['50']) || 0,
              counts: JSON.stringify(s.counts || {}),
            };
            """);
    }
}
