package org.peergos.thumbnailer;

import org.junit.*;
import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Optional;
import java.util.zip.CRC32;

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

    @Test public void testAvif()    { assertThumbnail("resources/img/tree.avif"); }
    @Test public void testAvifFox() { assertThumbnail("resources/img/fox.avif"); }

    @Test public void testMp4()       { assertVideo("resources/video/sample.mp4"); }
    @Test public void testMp4Hevc()   { assertVideo("resources/video/buck-bunny-hevc.mp4"); }
    @Test public void testMkv()       { assertVideo("resources/video/sample.mkv"); }
    @Test public void testMov()       { assertVideo("resources/video/sample.mov"); }
    @Test public void testAvi()       { assertVideo("resources/video/sample.avi"); }
    @Test public void testFlv()       { assertVideo("resources/video/sample.flv"); }
    @Test public void testMpeg()      { assertVideo("resources/video/sample.mpeg"); }
    @Test public void testWebm()      { assertVideo("resources/video/sample.webm"); }
    @Test public void testWmv()       { assertVideo("resources/video/sample.wmv"); }

    /** A video whose first seconds are black gets the first frame worth looking at, not the
     *  black one: an all black 200px thumbnail is about 200 bytes, a real frame far more. */
    @Test public void testSkipsBlackOpening() {
        String path = "resources/video/starts-black.mp4";
        Optional<byte[]> result = VideoThumbnailer.generateWebP(new File(path), 200);
        Assert.assertTrue("No thumbnail for " + path, result.isPresent());
        assertWebP(path, result.get());
        Assert.assertTrue("Thumbnail of " + path + " looks blank: " + result.get().length + " bytes",
                result.get().length > 600);
    }

    /** A camera stores a portrait as landscape pixels and an EXIF Orientation saying how to turn them.
     *  The picture is in flat 16px blocks, so each stored copy decodes to exactly the upright pixels
     *  turned, and at 400px it is cropped, not scaled: every thumbnail is the upright one. */
    @Test public void testExifOrientation() throws IOException {
        BufferedImage upright = picture();
        int w = upright.getWidth(), h = upright.getHeight();
        byte[] expected = thumbnail(jpeg(upright, 1), ".jpg");
        for (int o = 2; o <= 8; o++) {
            BufferedImage stored = o >= 5 ? new BufferedImage(h, w, BufferedImage.TYPE_INT_RGB) :
                    new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
            for (int r = 0; r < stored.getHeight(); r++)
                for (int c = 0; c < stored.getWidth(); c++) { // TIFF 6.0: the visual sides of row 0 and column 0
                    int x = o >= 5 ? (o == 5 || o == 8 ? r : w - 1 - r) : (o == 4 ? c : w - 1 - c);
                    int y = o >= 5 ? (o == 5 || o == 6 ? c : h - 1 - c) : (o == 2 ? r : h - 1 - r);
                    stored.setRGB(c, r, upright.getRGB(x, y));
                }
            Assert.assertArrayEquals("orientation " + o, expected, thumbnail(jpeg(stored, o), ".jpg"));
            Assert.assertFalse("orientation " + o + " stored upright",
                    Arrays.equals(expected, thumbnail(jpeg(stored, 1), ".jpg")));
        }
    }

    /** Browsers ignore a PNG's eXIf after its image data, so the thumbnail is of the pixels as stored. */
    @Test public void testPngExifAfterImageDataIgnored() throws IOException {
        byte[] png = encode(picture(), "png"), tiff = tiff(6);
        ByteBuffer chunk = ByteBuffer.allocate(12 + tiff.length).putInt(tiff.length)
                .put("eXIf".getBytes(StandardCharsets.US_ASCII)).put(tiff);
        CRC32 crc = new CRC32();
        crc.update(chunk.array(), 4, 4 + tiff.length);
        chunk.putInt((int) crc.getValue());
        int iend = png.length - 12; // IEND is the last chunk, and empty
        byte[] tagged = ByteBuffer.allocate(png.length + chunk.capacity())
                .put(png, 0, iend).put(chunk.array()).put(png, iend, 12).array();
        Assert.assertArrayEquals(thumbnail(png, ".png"), thumbnail(tagged, ".png"));
    }

    /** Red at the top left and green at the top right, in flat 16px blocks. */
    private static BufferedImage picture() {
        BufferedImage image = new BufferedImage(400, 608, BufferedImage.TYPE_INT_RGB);
        for (int y = 0; y < image.getHeight(); y++)
            for (int x = 0; x < image.getWidth(); x++)
                image.setRGB(x, y, y < 144 || y >= 272 ? 0xC8C8C8 :
                        x >= 32 && x < 160 ? 0xFF0000 : x >= 240 && x < 368 ? 0x00FF00 : 0xC8C8C8);
        return image;
    }

    private static byte[] encode(BufferedImage image, String format) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ImageIO.write(image, format, out);
        return out.toByteArray();
    }

    /** A big-endian TIFF header and an IFD0 holding just the orientation. */
    private static byte[] tiff(int orientation) {
        return ByteBuffer.allocate(26).put("MM".getBytes(StandardCharsets.US_ASCII)).putShort((short) 42).putInt(8)
                .putShort((short) 1).putShort((short) 274).putShort((short) 3).putInt(1)
                .putShort((short) orientation).putShort((short) 0).putInt(0).array();
    }

    /** The image as a JPEG with an Exif APP1 segment holding just the orientation. */
    private static byte[] jpeg(BufferedImage image, int orientation) throws IOException {
        byte[] jpeg = encode(image, "jpg"), tiff = tiff(orientation);
        return ByteBuffer.allocate(jpeg.length + 10 + tiff.length).put(jpeg, 0, 2)
                .putShort((short) 0xFFE1).putShort((short) (8 + tiff.length))
                .put("Exif\0\0".getBytes(StandardCharsets.US_ASCII)).put(tiff)
                .put(jpeg, 2, jpeg.length - 2).array();
    }

    private static byte[] thumbnail(byte[] data, String extension) throws IOException {
        File file = File.createTempFile("thumbnail-", extension);
        try {
            Files.write(file.toPath(), data);
            Optional<byte[]> result = VideoThumbnailer.generateImageWebP(file, 400);
            Assert.assertTrue("No thumbnail for " + file.getName(), result.isPresent());
            return result.get();
        } finally {
            Files.deleteIfExists(file.toPath());
        }
    }

    private static void assertVideo(String path) {
        Optional<byte[]> result = VideoThumbnailer.generateWebP(new File(path), 200);
        Assert.assertTrue("No thumbnail for " + path, result.isPresent());
        assertWebP(path, result.get());
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
