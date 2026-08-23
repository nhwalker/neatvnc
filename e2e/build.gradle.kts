import java.time.Duration

plugins {
    java
}

repositories {
    mavenCentral()
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(21)
    }
}

val allureVersion = "2.29.0"

// Resolved from Maven Central rather than fetched from a release page, so the
// report can be generated with nothing but the build's own dependencies.
val allureCli: Configuration by configurations.creating

dependencies {
    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")

    testImplementation(platform("org.testcontainers:testcontainers-bom:1.21.3"))
    testImplementation("org.testcontainers:testcontainers")
    testImplementation("org.testcontainers:junit-jupiter")
    testImplementation("org.testcontainers:selenium")

    testImplementation("org.seleniumhq.selenium:selenium-java:4.27.0")

    // allure-junit5 registers itself through the ServiceLoader, so no Gradle
    // plugin is needed; the results directory is set below.
    testImplementation("io.qameta.allure:allure-junit5:$allureVersion")

    testImplementation("org.slf4j:slf4j-simple:2.0.16")

    allureCli("io.qameta.allure:allure-commandline:$allureVersion@zip")
}

val allureResults = layout.buildDirectory.dir("allure-results")
val allureReport = layout.buildDirectory.dir("allure-report")
val allureCliDir = layout.buildDirectory.dir("allure-cli")

tasks.test {
    useJUnitPlatform()

    systemProperty("allure.results.directory", allureResults.get().asFile.absolutePath)

    /*
     * Testcontainers falls back to Docker API 1.32 whenever the configuration
     * does not name a version, and Docker Engine 26 and later refuse anything
     * below 1.40 outright. docker-java reads the version from the "api.version"
     * property -- not from DOCKER_API_VERSION, which is what the daemon and the
     * CLI use -- so set that. 1.41 is understood by every engine still in
     * support.
     */
    systemProperty("api.version",
        providers.systemProperty("api.version").getOrElse("1.41"))

    // The image is built by the workflow, or by hand, before the test runs.
    systemProperty("neatvnc.testImage",
        providers.systemProperty("neatvnc.testImage").getOrElse("neatvnc-nvenc-test:e2e"))
    // Swap for h264_nvenc on a machine that has an NVIDIA GPU.
    systemProperty("neatvnc.h264Codec",
        providers.systemProperty("neatvnc.h264Codec").getOrElse("libx264"))
    systemProperty("neatvnc.videoDir",
        layout.buildDirectory.dir("videos").get().asFile.absolutePath)

    // Pulling browser images and recording a session is not fast.
    timeout = Duration.ofMinutes(20)

    testLogging {
        events("passed", "failed", "skipped")
        showStandardStreams = true
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }

    doFirst {
        allureResults.get().asFile.deleteRecursively()
        allureResults.get().asFile.mkdirs()
    }

    outputs.upToDateWhen { false }
}

val unpackAllureCli by tasks.registering(Copy::class) {
    from(zipTree(allureCli.singleFile))
    into(allureCliDir)
}

/*
 * --single-file folds the report and its attachments into one index.html. A
 * directory report does not survive being downloaded from a CI artifact and
 * opened from the filesystem; a single file does, which is the whole point of
 * keeping a video around for someone to look at later.
 */
val allureSingleFileReport by tasks.registering {
    group = "verification"
    description = "Generates a self-contained Allure report at build/allure-report/index.html"

    dependsOn(unpackAllureCli)

    val results = allureResults.get().asFile
    val report = allureReport.get().asFile
    val cliRoot = allureCliDir.get().asFile

    doLast {
        if (!results.exists() || results.listFiles().isNullOrEmpty()) {
            logger.lifecycle("No Allure results in $results; nothing to report on.")
            return@doLast
        }

        val cli = cliRoot.walkTopDown()
            .firstOrNull { it.isFile && it.name == "allure" && it.parentFile.name == "bin" }
            ?: throw GradleException("allure launcher not found under $cliRoot")
        cli.setExecutable(true)

        val process = ProcessBuilder(
            cli.absolutePath, "generate", "--single-file", "--clean",
            results.absolutePath, "-o", report.absolutePath,
        ).redirectErrorStream(true).start()

        process.inputStream.bufferedReader().forEachLine { logger.lifecycle(it) }

        val code = process.waitFor()
        if (code != 0) {
            throw GradleException("allure generate failed with exit code $code")
        }

        logger.lifecycle("Allure report: $report/index.html")
    }
}
