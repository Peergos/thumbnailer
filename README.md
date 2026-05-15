# Jar for native thumbnail creation

This builds a cross platform jar with native libraries for video and image thumbnail creation.

It wraps ffmpeg and libwebp to create webp thumbnails for all common image formats and videos. For video, it seeks 1s into the video. For images it sets the smaller dimension to 400px then crops a center square. 

At load time it creates a content addressed tmp file if it doesn't exist to call System.load() on. The path for this is controlled by the "java.io.tmpdir" system property. 
