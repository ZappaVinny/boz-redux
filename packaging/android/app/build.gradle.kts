import java.net.URI
import java.security.MessageDigest

plugins {
    id("com.android.application")
}

// The client for Android phones and TVs: the same runtime as the PC (the repository's
// CMakeLists.txt builds libmain.so with Dynarmic's arm64 backend and the guest-base map), run by
// SDL's Java activity. SDL comes from the third_party/SDL2 submodule; Dynarmic needs Boost's
// headers, downloaded once into packaging/android/.deps (git-ignored) and checked by SHA-256.
data class Dep(val url: String, val sha256: String, val dir: String, val include: String)

val depsDir: File = rootProject.file(".deps")
val boost = Dep(
    "https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.gz",
    "2128a4c96862b5c0970c1e34d76b1d57e4a1016b80df85ad39667f30b1deba26",
    "boost-1.86.0", "boost-1.86.0/boost/**")

fun sha256(file: File): String {
    val md = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input ->
        val buffer = ByteArray(1 shl 16)
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            md.update(buffer, 0, n)
        }
    }
    return md.digest().joinToString("") { "%02x".format(it) }
}

fun ensure(dep: Dep): File {
    val out = File(depsDir, dep.dir)
    val stamp = File(out, ".boz-ok")
    if (stamp.isFile && stamp.readText() == dep.sha256) return out
    depsDir.mkdirs()
    val archive = File(depsDir, dep.url.substringAfterLast('/'))
    if (!archive.isFile || sha256(archive) != dep.sha256) {
        logger.lifecycle("BOZ: downloading ${dep.url}")
        URI(dep.url).toURL().openStream().use { input ->
            archive.outputStream().use { input.copyTo(it) }
        }
        val got = sha256(archive)
        if (got != dep.sha256) {
            archive.delete()
            throw GradleException("SHA-256 mismatch for ${archive.name}: $got, expected ${dep.sha256}")
        }
    }
    logger.lifecycle("BOZ: extracting ${archive.name}")
    copy {
        from(tarTree(resources.gzip(archive))) { include(dep.include) }
        into(depsDir)
    }
    stamp.writeText(dep.sha256)
    return out
}

val boostDir = ensure(boost)
val repo: File = rootProject.file("../..")

// The approved BOZ Redux icon (branding/icons), as the launcher icon.
val iconRes = layout.buildDirectory.dir("generated/boz-res").get().asFile
run {
    val icon = File(repo, "branding/icons/boz-redux-192.png")
    val target = File(iconRes, "mipmap-xxxhdpi/ic_launcher.png")
    if (icon.isFile && (!target.isFile || target.lastModified() < icon.lastModified())) {
        target.parentFile.mkdirs()
        icon.copyTo(target, overwrite = true)
    }
}

android {
    namespace = "org.bozredux.client"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        // Not com.activision.boz: installs next to the original game.
        applicationId = "org.bozredux.client"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.4.0-dev"
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DBOZ_GUEST_BASE=ON",
                    "-DBoost_INCLUDE_DIR=${boostDir.absolutePath}",
                    // one shared libc++ for SDL (hidapi) and Dynarmic, as the NDK recommends
                    "-DANDROID_STL=c++_shared",
                    // ELF segments aligned for 16 KiB-page devices too (Android 15+)
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                )
                targets += listOf("codboz_s3e_loader", "SDL2", "SDL2_mixer")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(repo, "CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets["main"].java.srcDir(File(repo, "third_party/SDL2/android-project/app/src/main/java"))
    sourceSets["main"].res.srcDir(iconRes)

    // A fixed key from the environment keeps updates installable over the previous build (so the
    // imported game files survive); without it the machine's debug key is used.
    val keystore = System.getenv("BOZ_ANDROID_KEYSTORE")
    val keystorePassword = System.getenv("BOZ_ANDROID_KEYSTORE_PASSWORD")
    val keyAlias = System.getenv("BOZ_ANDROID_KEY_ALIAS") ?: "boz"
    signingConfigs {
        if (!keystore.isNullOrEmpty() && !keystorePassword.isNullOrEmpty()) {
            create("boz") {
                storeFile = file(keystore)
                storePassword = keystorePassword
                this.keyAlias = keyAlias
                keyPassword = keystorePassword
            }
        }
    }

    buildTypes {
        // The JIT is slow at -O0: test with assembleRelease.
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("boz") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
            keepDebugSymbols += "**/*.so" // readable native crash dumps while this is in testing
        }
    }
}
