package org.neatvnc.e2e;

import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

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
 * The viewer page reports stream health and manages picture quality by itself.
 *
 * <p>Two properties matter and they pull in opposite directions, so both are
 * checked here. On a link with room to spare the loop must leave quality alone
 * -- a control loop that fidgets on a healthy stream costs a key frame every
 * time it moves and is worse than none. When the stream genuinely cannot keep
 * up, it must actually descend and the gumball must say so.
 *
 * <p>The two cases are produced by changing the desktop size rather than by
 * shaping the network, because websockify is the real bottleneck in this stack:
 * a 1920x1200 desktop does not fit through it, and a small one does.
 */
class ViewerQualityTest {

    /** Pinned to match selenium-java on the classpath; see WestonNoVncH264Test. */
    private static final String SELENIUM_IMAGE = "selenium/standalone-chrome:4.27.0";

    /**
     * 0.35 of a core. Enough to start and serve, not enough to keep up with a
     * 1920x1200 desktop -- measured at 5 to 8 fps with 70 to 80% of frames
     * discarded, on hosts of quite different speeds.
     */
    private static final long CONGESTED_NANO_CPUS = 350_000_000L;

    /** Must match QUALITY_START in viewer.html. */
    private static final int QUALITY_START = 6;
    private static final int QUALITY_MAX = 9;
    private static final int QUALITY_FLOOR = 3;

    /** Long enough for the loop to settle; it steps at most once every 2 s. */
    private static final Duration SETTLE = Duration.ofSeconds(20);
    private static final Duration SAMPLE_INTERVAL = Duration.ofMillis(500);

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

        browser = new BrowserWebDriverContainer<>(
                DockerImageName.parse(SELENIUM_IMAGE)
                    .asCompatibleSubstituteFor("selenium/standalone-chrome"))
            .withNetwork(network)
            .withCapabilities(chromeOptions());
        browser.start();

        driver = (RemoteWebDriver) browser.getWebDriver();
    }

    private static ChromeOptions chromeOptions() {
        ChromeOptions options = new ChromeOptions();
        options.addArguments("--no-sandbox", "--disable-dev-shm-usage",
            "--window-size=1400,900");
        // Without a secure context noVNC has no H.264 decoder to offer and
        // quietly falls back to Tight. See WestonNoVncH264Test.
        options.addArguments(
            "--unsafely-treat-insecure-origin-as-secure="
                + "http://" + WestonContainer.NETWORK_ALIAS + ":" + WestonContainer.WEB_PORT,
            "--user-data-dir=/tmp/neatvnc-viewer-profile");
        return options;
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
    @DisplayName("a stream with headroom is left alone and reads as excellent")
    void healthyStreamHoldsQuality() {
        try (WestonContainer weston = new WestonContainer(image, codec)) {
            // Small enough that even a modest host carries it comfortably. No
            // CPU limit here, deliberately: this is the no-regression case.
            weston.withEnv("WESTON_WIDTH", "640");
            weston.withEnv("WESTON_HEIGHT", "480");
            weston.withNetwork(network);
            weston.start();

            List<Sample> samples = observe(weston);

            Sample last = samples.get(samples.size() - 1);
            Allure.addAttachment("healthy link, samples", "text/plain", render(samples));

            /* The loop starts at 6 and, on a link with headroom, is allowed --
             * expected, even -- to probe upward one step per clean window. What
             * it must never do here is descend.
             */
            assertTrue(last.quality >= QUALITY_START && last.quality <= QUALITY_MAX,
                () -> "the loop should never descend on a healthy link, but it "
                    + "settled at " + last.quality + "\n" + render(samples));
            for (int i = 1; i < samples.size(); ++i) {
                int previous = samples.get(i - 1).quality;
                int current = samples.get(i).quality;
                final int index = i;
                assertTrue(current >= previous,
                    () -> "quality descended at sample " + index
                        + " on a healthy link\n" + render(samples));
            }
            assertTrue(last.tier.equals("excellent") || last.tier.equals("good"),
                () -> "expected a healthy tier, got " + last.tier + "\n" + render(samples));
            assertTrue(last.statsFresh,
                () -> "the page never saw a fresh stats snapshot\n" + render(samples));
        }
    }

    @Test
    @DisplayName("a stream that cannot keep up is degraded down to the floor")
    void congestedStreamDegrades() {
        try (WestonContainer weston = new WestonContainer(image, codec)) {
            weston.withEnv("WESTON_WIDTH", "1920");
            weston.withEnv("WESTON_HEIGHT", "1200");

            /*
             * Congestion has to be induced, not hoped for. Picking a desktop
             * size large enough to overwhelm the stack only works if the host
             * is slow: a CI runner carries 1920x1200 through websockify at
             * around 25 fps with barely any skipping, and the test then fails
             * for want of anything to react to.
             *
             * An absolute CPU limit is the same everywhere. At this share the
             * encoder and websockify between them cannot keep up with the
             * frames weston offers, the client falls behind acknowledging what
             * it has been sent, and the congestion limiter starts discarding
             * frames -- which is exactly the condition under test.
             */
            weston.withCreateContainerCmdModifier(cmd ->
                cmd.getHostConfig().withNanoCPUs(CONGESTED_NANO_CPUS));
            weston.withNetwork(network);
            weston.start();

            List<Sample> samples = observe(weston);

            Sample last = samples.get(samples.size() - 1);
            Allure.addAttachment("congested link, samples", "text/plain", render(samples));

            assertTrue(last.quality < QUALITY_START,
                () -> "the loop should have lowered quality on a congested link\n"
                    + render(samples));
            assertTrue(last.quality >= QUALITY_FLOOR,
                () -> "quality fell below the floor of " + QUALITY_FLOOR + "\n"
                    + render(samples));
            assertTrue(last.tier.equals("poor") || last.tier.equals("fair"),
                () -> "expected a degraded tier, got " + last.tier + "\n" + render(samples));

            // Monotonic: the loop should descend, never oscillate upwards while
            // the link is still struggling.
            for (int i = 1; i < samples.size(); ++i) {
                int previous = samples.get(i - 1).quality;
                int current = samples.get(i).quality;
                final int index = i;
                assertTrue(current <= previous,
                    () -> "quality went back up at sample " + index
                        + " while the link was still congested\n" + render(samples));
            }
        }
    }

    private record Sample(long atMillis, int quality, String tier, boolean statsFresh,
            String detail) { }

    private List<Sample> observe(WestonContainer weston) {
        String url = weston.internalOrigin() + "/viewer.html?username="
            + WestonContainer.USERNAME + "&password=" + WestonContainer.PASSWORD;
        try {
            driver.get(url);
        } catch (RuntimeException e) {
            // A bare ERR_CONNECTION_REFUSED says nothing about which side is at
            // fault; the server's own log usually does.
            throw new AssertionError("could not load " + url + "\n"
                + weston.serverLog(), e);
        }

        new WebDriverWait(driver, Duration.ofSeconds(60))
            .withMessage(() -> "the viewer never connected\n" + weston.serverLog())
            .until(d -> Boolean.TRUE.equals(
                ((RemoteWebDriver) d).executeScript(
                    "return !!(window.__viewer && window.__viewer.connected)")));

        List<Sample> samples = new ArrayList<>();
        long start = System.currentTimeMillis();
        long deadline = start + SETTLE.toMillis();

        while (System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(SAMPLE_INTERVAL.toMillis());
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }

            @SuppressWarnings("unchecked")
            java.util.Map<String, Object> state =
                (java.util.Map<String, Object>) driver.executeScript("""
                    return {
                      quality: window.__viewer.quality,
                      tier: window.__viewer.tier,
                      detail: document.getElementById('detail').textContent,
                    };
                    """);

            String tier = String.valueOf(state.get("tier"));
            samples.add(new Sample(
                System.currentTimeMillis() - start,
                ((Number) state.get("quality")).intValue(),
                tier,
                !"unknown".equals(tier),
                String.valueOf(state.get("detail"))));
        }

        assertNotNull(samples);
        assertTrue(!samples.isEmpty(), "no samples were taken");
        return samples;
    }

    private static String render(List<Sample> samples) {
        StringBuilder out = new StringBuilder("   t     quality  tier        detail\n");
        for (Sample s : samples) {
            out.append(String.format("%5.1fs  %7d  %-10s  %s%n",
                s.atMillis() / 1000.0, s.quality(), s.tier(), s.detail()));
        }
        return out.toString();
    }
}
