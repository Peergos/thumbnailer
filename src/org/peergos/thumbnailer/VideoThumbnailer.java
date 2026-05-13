package org.peergos.thumbnailer;

import java.io.*;
import java.nio.file.*;
import java.util.Optional;

public class VideoThumbnailer {

    private static final boolean LOADED;

    static {
        LOADED = tryLoad();
    }

    private static boolean tryLoad() {
        try {
            String resource = nativeResource();
            if (resource == null)
                return false;
            String ext = resource.substring(resource.lastIndexOf('.'));
            Path tmp = Files.createTempFile("thumbnailer-", ext);
            tmp.toFile().deleteOnExit();
            try (InputStream in = VideoThumbnailer.class.getResourceAsStream(resource)) {
                if (in == null)
                    return false;
                Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
            }
            System.load(tmp.toAbsolutePath().toString());
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static String nativeResource() {
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = normalizeArch(System.getProperty("os.arch", "").toLowerCase());
        String platform, lib;
        if (os.contains("linux")) {
            platform = "linux-" + arch;
            lib = "thumbnailer.so";
        } else if (os.contains("mac")) {
            platform = "macos-" + arch;
            lib = "thumbnailer.dylib";
        } else if (os.contains("windows")) {
            platform = "windows-" + arch;
            lib = "thumbnailer.dll";
        } else {
            return null;
        }
        return "/native/" + platform + "/" + lib;
    }

    private static String normalizeArch(String arch) {
        if (arch.equals("amd64") || arch.equals("x86_64"))
            return "x86_64";
        if (arch.equals("aarch64") || arch.equals("arm64"))
            return "aarch64";
        return arch;
    }

    public static boolean isAvailable() {
        return LOADED;
    }

    /**
     * Generate a WebP thumbnail from a video file.
     *
     * @param videoFile the video to thumbnail
     * @param maxSize   longest edge in pixels (aspect ratio preserved)
     * @return WebP bytes, or empty if the file cannot be thumbnailed
     */
    public static Optional<byte[]> generateWebP(File videoFile, int maxSize) {
        if (!LOADED)
            return Optional.empty();
        byte[] result = generate(videoFile.getAbsolutePath(), maxSize);
        return Optional.ofNullable(result);
    }

    private static native byte[] generate(String path, int maxSize);
}
