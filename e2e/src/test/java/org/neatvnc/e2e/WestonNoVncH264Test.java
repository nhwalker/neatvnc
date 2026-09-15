package org.neatvnc.e2e;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Optional;
import java.util.Set;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.openqa.selenium.JavascriptExecutor;
import org.openqa.selenium.OutputType;
import org.openqa.selenium.TakesScreenshot;
import org.openqa.selenium.chrome.ChromeOptions;
import org.openqa.selenium.remote.RemoteWebDriver;
import org.openqa.selenium.support.ui.WebDriverWait;
import org.testcontainers.containers.BrowserWebDriverContainer;
import org.testcontainers.containers.BrowserWebDriverContainer.VncRecordingMode;
import org.testcontainers.containers.Network;
import org.testcontainers.containers.VncRecordingContainer.VncRecordingFormat;
import org.testcontainers.lifecycle.TestDescription;
import org.testcontainers.utility.DockerImageName;

import io.qameta.allure.Allure;

/**
 * Proves the thing the other tests cannot: that a browser pointed at Weston
 * running the patched neatvnc negotiates H.264 and renders a live desktop.
 *
 * <p>The failure this guards against is silent. If noVNC has no H.264 decoder,
 * or the page is not a secure context, or the browser lacks the codec, noVNC
 * quietly falls back to Tight and everything still "works".
 *
 * <p>NVENC itself is still not exercised: build machines have no GPU, so
 * {@code neatvnc.h264Codec} points the encoder at libx264. Everything up to the
 * encoder hand-off is real.
 */
@DisplayName("Weston over noVNC uses H.264 end to end")
class WestonNoVncH264Test {

    /*
     * Pinned rather than :latest. The image tracks the current Selenium server,
     * which has run ahead of the client on the classpath; the mismatch shows up
     * as session creation timing out with nothing useful in the log. Bump this
     * and selenium-java together.
     */
    private static final String SELENIUM_IMAGE = "selenium/standalone-chrome:4.27.0";

    private static final Duration OBSERVATION = Duration.ofSeconds(5);
    private static final Duration SAMPLE_INTERVAL = Duration.ofMillis(250);

    /** RFB encoding numbers, as noVNC keys its decoders. */
    private static final String H264 = "50";
    private static final String RAW = "0";
    private static final String TIGHT = "7";

    private static final int MIN_H264_RECTS = 20;
    private static final int MIN_DISTINCT_FRAMES = 3;

    private static Network network;
    private static WestonContainer weston;
    private static BrowserWebDriverContainer<?> browser;
    private static RemoteWebDriver driver;
    private static File videoDir;

    private static String westonLogAtEnd = "";

    /*
     * Testcontainers records the whole browser session, which includes Chrome
     * starting up and shutting down. These mark the span the assertions
     * actually cover, so a clip of just that can be cut out of it.
     */
    private static long recordingStartedAtMillis;
    private static long windowStartedAtMillis;

    private static final TestDescription DESCRIPTION = new TestDescription() {
        @Override
        public String getTestId() {
            return "weston-novnc-h264";
        }

        @Override
        public String getFilesystemFriendlyName() {
            return "weston-novnc-h264";
        }
    };

    @BeforeAll
    static void startStack() {
        String image = System.getProperty("neatvnc.testImage", "neatvnc-nvenc-test:e2e");
        String codec = System.getProperty("neatvnc.h264Codec", "libx264");
        videoDir = new File(System.getProperty("neatvnc.videoDir", "build/videos"));
        // Otherwise recordings pile up across runs and every one of them gets
        // attached to the report.
        File[] stale = videoDir.listFiles();
        if (stale != null) {
            for (File file : stale) {
                file.delete();
            }
        }
        videoDir.mkdirs();

        network = Network.newNetwork();

        weston = new WestonContainer(image, codec);
        weston.withNetwork(network);
        weston.start();

        browser = new BrowserWebDriverContainer<>(
                DockerImageName.parse(SELENIUM_IMAGE)
                    .asCompatibleSubstituteFor("selenium/standalone-chrome"))
            .withNetwork(network)
            .withCapabilities(chromeOptions())
            .withRecordingMode(VncRecordingMode.RECORD_ALL, videoDir, VncRecordingFormat.MP4);
        browser.start();
        recordingStartedAtMillis = System.currentTimeMillis();

        // The container already opened a session with the capabilities above;
        // opening a second one would just leave an idle browser around.
        driver = (RemoteWebDriver) browser.getWebDriver();
    }

    private static ChromeOptions chromeOptions() {
        ChromeOptions options = new ChromeOptions();
        options.addArguments("--no-sandbox", "--disable-dev-shm-usage",
            "--window-size=1400,900");

        /*
         * WebCodecs VideoDecoder exists only in a secure context, and without
         * it noVNC silently serves Tight instead of H.264. Rather than manage a
         * certificate, tell Chrome to treat this origin as trustworthy; the
         * user-data-dir is required for the flag to take effect.
         */
        options.addArguments(
            "--unsafely-treat-insecure-origin-as-secure="
                + "http://" + WestonContainer.NETWORK_ALIAS + ":" + WestonContainer.WEB_PORT,
            "--user-data-dir=/tmp/neatvnc-e2e-profile");

        return options;
    }

    /**
     * Evidence has to be gathered while the test is still running: Allure only
     * links attachments to whatever result is live at the time, and anything
     * added from @AfterAll is written to disk and then referenced by nothing.
     */
    private static void collectEvidence() {
        if (driver != null) {
            attachText("browser console", browserConsole());
            attachPng("final screenshot", driver.getScreenshotAs(OutputType.BYTES));
            driver.quit();
            driver = null;
        }
        if (weston != null) {
            attachText("weston log", weston.westonLog());
            attachText("neatvnc log (container stdout)", weston.getLogs());
        }
        if (browser != null) {
            /*
             * The recording is written by afterTest, which the JUnit extension
             * would normally call. This test drives the containers by hand, so
             * it has to make that call itself; without it the recorder runs and
             * silently discards everything.
             */
            browser.afterTest(DESCRIPTION, Optional.empty());
            browser.stop();
            browser = null;
            attachVideos();
        }
    }

    @AfterAll
    static void tearDown() {
        if (driver != null) {
            driver.quit();
        }
        if (browser != null) {
            browser.stop();
        }
        if (weston != null) {
            weston.stop();
        }
        if (network != null) {
            network.close();
        }
    }

    @Test
    void browserNegotiatesAndRendersH264() {
        try {
            runScenario();
        } finally {
            collectEvidence();
        }
    }

    private void runScenario() {
        String url = weston.internalOrigin() + "/e2e.html"
            + "?username=" + WestonContainer.USERNAME
            + "&password=" + WestonContainer.PASSWORD;

        Allure.step("Open the viewer at " + url, () -> driver.get(url));

        WebDriverWait wait = new WebDriverWait(driver, Duration.ofSeconds(60));

        Allure.step("Wait for the RFB connection", () ->
            wait.withMessage(() -> "the viewer never connected. State: " + safeState())
                .until(d -> Boolean.TRUE.equals(js("return !!(window.__e2e && window.__e2e.connected);"))));

        // A timeout here is the interesting failure: it means the client never
        // decoded an H.264 rect, which usually means a silent fallback to
        // Tight. Say so, and say what both ends thought was going on.
        Allure.step("Wait for the first decoded H.264 rect", () ->
            wait.withMessage(() -> "no H.264 rect was ever decoded, so the client "
                    + "fell back to another encoding or never got pixels at all."
                    + "\nViewer state: " + safeState()
                    + "\nServer log:\n" + weston.serverLog())
                .until(d -> rectCount(H264) > 0));

        Set<Object> frameHashes = new LinkedHashSet<>();
        List<String> samples = new ArrayList<>();

        Allure.step("Observe for " + OBSERVATION.toSeconds() + " seconds", () -> {
            js("window.__e2eMarkWindow(true);");
            windowStartedAtMillis = System.currentTimeMillis();
            long deadline = System.nanoTime() + OBSERVATION.toNanos();
            while (System.nanoTime() < deadline) {
                frameHashes.add(js(CANVAS_HASH_JS));
                samples.add(js("return JSON.stringify(window.__e2e);").toString());
                sleep(SAMPLE_INTERVAL);
            }
            js("window.__e2eMarkWindow(false);");
        });

        String state = js("return JSON.stringify(window.__e2e);").toString();
        westonLogAtEnd = weston.serverLog();

        attachText("window.__e2e at the end of the window", state);
        attachText("samples during the window", String.join("\n", samples));

        // The server chose H.264 and used the encoder we asked for.
        assertTrue(westonLogAtEnd.contains("Choosing open-h264 encoding for client"),
            "neatvnc did not choose open-h264. Log:\n" + westonLogAtEnd);
        assertTrue(westonLogAtEnd.contains("for H.264 encoding"),
            "neatvnc never reported which H.264 encoder it opened. Log:\n" + westonLogAtEnd);
        assertTrue(!westonLogAtEnd.contains("Failed to encode"),
            "neatvnc reported encode failures. Log:\n" + westonLogAtEnd);

        // The browser could have decoded H.264, so a fallback to Tight would
        // have been a real fallback rather than a missing codec.
        assertTrue(Boolean.TRUE.equals(js("return window.__e2e.webcodecsH264;")),
            "the browser reported no WebCodecs H.264 support, so this run proves "
                + "nothing about encoding choice. State: " + state);
        assertTrue(Boolean.TRUE.equals(js("return window.__e2e.hookInstalled;")),
            "the decoder hook was not installed, so the counts are meaningless. State: " + state);

        // H.264 is what the browser actually decoded, and it kept coming.
        long h264 = rectCount(H264);
        long fallback = rectCount(RAW) + rectCount(TIGHT);
        assertTrue(h264 >= MIN_H264_RECTS,
            "only " + h264 + " H.264 rects in " + OBSERVATION.toSeconds()
                + "s; expected at least " + MIN_H264_RECTS + ". State: " + state);
        assertTrue(h264 > 10 * fallback,
            "H.264 did not dominate: " + h264 + " H.264 rects against " + fallback
                + " raw/tight. State: " + state);

        // And the picture moved, rather than freezing on the first keyframe.
        assertTrue(frameHashes.size() >= MIN_DISTINCT_FRAMES,
            "the framebuffer produced only " + frameHashes.size()
                + " distinct images during the window; the desktop is not animating");

        assertTrue(Boolean.TRUE.equals(js("return window.__e2e.error === null;")),
            "the viewer reported an error. State: " + state);
    }

    /**
     * A cheap sparse checksum of the framebuffer. Sampling every 997th pixel is
     * enough to tell "the picture changed" from "the picture is frozen" without
     * pulling a megabyte of pixels across the wire each time.
     */
    private static final String CANVAS_HASH_JS = """
        const c = document.querySelector('#screen canvas');
        if (!c) return 'no-canvas';
        const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
        let h = 2166136261;
        for (let i = 0; i < d.length; i += 997 * 4) { h ^= d[i]; h = Math.imul(h, 16777619); }
        return String(h | 0);
        """;

    /** Never throws: it is only ever used to explain another failure. */
    private static String safeState() {
        try {
            Object state = js("return JSON.stringify(window.__e2e);");
            return state == null ? "(no window.__e2e)" : state.toString();
        } catch (RuntimeException e) {
            return "(could not read window.__e2e: " + e + ")";
        }
    }

    private static long rectCount(String encoding) {
        Object value = js("return (window.__e2e && window.__e2e.counts['" + encoding + "']) || 0;");
        return value instanceof Number n ? n.longValue() : 0;
    }

    private static Object js(String script) {
        return ((JavascriptExecutor) driver).executeScript(script);
    }

    private static void sleep(Duration duration) {
        try {
            Thread.sleep(duration.toMillis());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static String browserConsole() {
        try {
            StringBuilder out = new StringBuilder();
            driver.manage().logs().get("browser").forEach(entry -> out.append(entry).append('\n'));
            return out.toString();
        } catch (RuntimeException e) {
            return "browser logs unavailable: " + e;
        }
    }

    private static void attachVideos() {
        File[] found = videoDir.listFiles((dir, name) ->
            (name.endsWith(".mp4") || name.endsWith(".flv")) && !name.startsWith("observation-window"));
        if (found == null || found.length == 0) {
            attachText("session video", "no recording was produced in " + videoDir);
            return;
        }
        for (File video : found) {
            attachVideo("full session", video);
            File clip = trimToObservationWindow(video);
            if (clip != null) {
                attachVideo("observation window", clip);
            }
        }
    }

    private static void attachVideo(String label, File video) {
        try {
            Allure.addAttachment(label + " (" + video.getName() + ")", "video/mp4",
                Files.newInputStream(video.toPath()), ".mp4");
        } catch (IOException e) {
            attachText(label, "could not attach " + video + ": " + e);
        }
    }

    /**
     * Cuts the observation window out of the full recording, so the evidence is
     * the span the assertions cover rather than several seconds of Chrome
     * starting up. Best effort: without ffmpeg the full recording is still
     * attached, and it is still watchable.
     */
    private static File trimToObservationWindow(File video) {
        if (windowStartedAtMillis == 0) {
            return null;
        }

        double offset = Math.max(0,
            (windowStartedAtMillis - recordingStartedAtMillis) / 1000.0);
        File clip = new File(videoDir, "observation-window.mp4");

        try {
            Process process = new ProcessBuilder("ffmpeg", "-v", "error", "-y",
                "-ss", String.format(java.util.Locale.ROOT, "%.2f", offset),
                "-i", video.getAbsolutePath(),
                "-t", String.valueOf(OBSERVATION.toSeconds()),
                clip.getAbsolutePath())
                .redirectErrorStream(true)
                .start();

            String output = new String(process.getInputStream().readAllBytes(),
                java.nio.charset.StandardCharsets.UTF_8);

            if (process.waitFor() != 0 || !clip.isFile()) {
                attachText("observation window", "ffmpeg could not cut the clip: " + output);
                return null;
            }
            return clip;
        } catch (IOException e) {
            attachText("observation window",
                "ffmpeg is not available, so only the full recording is attached: " + e);
            return null;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return null;
        }
    }

    private static void attachText(String name, String body) {
        Allure.addAttachment(name, "text/plain", body == null ? "" : body, ".txt");
    }

    private static void attachPng(String name, byte[] png) {
        Allure.getLifecycle().addAttachment(name, "image/png", ".png", png);
    }
}
