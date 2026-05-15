package org.peergos.thumbnailer;

import org.junit.*;
import java.io.*;
import java.util.Optional;

public class ThumbnailerTest {

    @BeforeClass
    public static void checkAvailable() {
        Assume.assumeTrue("Native thumbnailer not available on this platform",
                VideoThumbnailer.isAvailable());
    }

    @Test public void testPng()  { assertThumbnail("resources/img/mandelbrot.png"); }
    @Test public void testJpeg() { assertThumbnail("resources/img/meme.jpg"); }
    @Test public void testGif()  { assertThumbnail("resources/img/scene.gif"); }
    @Test public void testWebp() { assertThumbnail("resources/img/sunset.webp"); }
    @Test public void testTiff() { assertThumbnail("resources/img/scene.tiff"); }
    @Test public void testBmp()  { assertThumbnail("resources/img/sample.bmp"); }

    @Test public void testAvif() {
        Optional<byte[]> result = VideoThumbnailer.generateImageWebP(new File("resources/img/tree.avif"), 200);
        Assume.assumeTrue("AVIF decoding not available (requires libdav1d)", result.isPresent());
        assertWebP("resources/img/tree.avif", result.get());
    }

    private static void assertThumbnail(String path) {
        Optional<byte[]> result = VideoThumbnailer.generateImageWebP(new File(path), 200);
        Assert.assertTrue("No thumbnail for " + path, result.isPresent());
        assertWebP(path, result.get());
    }

    private static void assertWebP(String path, byte[] webp) {
        Assert.assertTrue("Result too small for " + path, webp.length > 12);
        Assert.assertEquals("Not RIFF for " + path, 'R', (char) webp[0]);
        Assert.assertEquals("Not RIFF for " + path, 'I', (char) webp[1]);
        Assert.assertEquals("Not RIFF for " + path, 'F', (char) webp[2]);
        Assert.assertEquals("Not RIFF for " + path, 'F', (char) webp[3]);
        Assert.assertEquals("Not WEBP for " + path, 'W', (char) webp[8]);
        Assert.assertEquals("Not WEBP for " + path, 'E', (char) webp[9]);
        Assert.assertEquals("Not WEBP for " + path, 'B', (char) webp[10]);
        Assert.assertEquals("Not WEBP for " + path, 'P', (char) webp[11]);
    }
}
