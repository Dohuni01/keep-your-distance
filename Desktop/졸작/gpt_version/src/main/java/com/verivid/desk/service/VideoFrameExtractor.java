package com.verivid.desk.service;

import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.ExtractedFrame;
import com.verivid.desk.model.FrameMetrics;
import com.verivid.desk.model.VideoExtractionResult;
import com.verivid.desk.model.VideoMetadata;
import com.verivid.desk.util.ImageUtils;
import org.bytedeco.javacv.FFmpegFrameGrabber;
import org.bytedeco.javacv.Frame;
import org.bytedeco.javacv.Java2DFrameConverter;
import org.springframework.http.HttpStatus;
import org.springframework.stereotype.Service;
import org.springframework.web.server.ResponseStatusException;

import java.awt.image.BufferedImage;
import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

@Service
public class VideoFrameExtractor {

    private final VerividProperties properties;

    public VideoFrameExtractor(VerividProperties properties) {
        this.properties = properties;
    }

    public VideoExtractionResult extract(Path videoPath, Path reportDir, int frameCount) {
        FFmpegFrameGrabber grabber = new FFmpegFrameGrabber(videoPath.toFile());
        Java2DFrameConverter converter = new Java2DFrameConverter();

        try {
            grabber.start();

            long durationUs = grabber.getLengthInTime();
            if (durationUs <= 0) {
                throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "영상 길이를 확인할 수 없습니다.");
            }

            VideoMetadata metadata = new VideoMetadata(
                    round4(durationUs / 1_000_000.0),
                    grabber.getImageWidth(),
                    grabber.getImageHeight(),
                    round4(grabber.getFrameRate()),
                    safeCodec(grabber.getVideoCodecName())
            );

            List<Long> timestamps = buildSampleTimestamps(durationUs, frameCount);
            List<SampledFrame> sampledFrames = new ArrayList<>();
            List<ExtractedFrame> frames = new ArrayList<>();

            Path rawDir = reportDir.resolve("raw");
            Path heatmapDir = reportDir.resolve("heatmap");

            for (int i = 0; i < timestamps.size(); i++) {
                long ts = timestamps.get(i);
                BufferedImage current = grabFrameAt(grabber, converter, ts);
                if (current == null) {
                    continue;
                }

                BufferedImage resizedCurrent = ImageUtils.resizePreservingAspect(current, properties.getAnalysis().getMaxSidePx());
                sampledFrames.add(new SampledFrame(i + 1, round4(ts / 1_000_000.0), resizedCurrent));
            }

            for (int i = 0; i < sampledFrames.size(); i++) {
                SampledFrame current = sampledFrames.get(i);
                SampledFrame comparison = sampledFrames.get(comparisonIndexFor(i, sampledFrames.size()));

                BufferedImage resizedComparison = ImageUtils.resizeToMatch(
                        comparison.image(),
                        current.image().getWidth(),
                        current.image().getHeight()
                );

                FrameMetrics metrics = ImageUtils.computeMetrics(current.image(), resizedComparison);
                BufferedImage heatmap = ImageUtils.createHeatmapOverlay(current.image(), resizedComparison);

                String rawName = String.format("frame-%02d.jpg", current.index());
                String heatName = String.format("frame-%02d-heatmap.jpg", current.index());

                Path rawPath = rawDir.resolve(rawName);
                Path heatPath = heatmapDir.resolve(heatName);

                ImageUtils.writeJpeg(current.image(), rawPath, (float) properties.getAnalysis().getJpegQuality());
                ImageUtils.writeJpeg(heatmap, heatPath, (float) properties.getAnalysis().getJpegQuality());

                frames.add(new ExtractedFrame(
                        current.index(),
                        current.timestampSeconds(),
                        rawPath,
                        heatPath,
                        metrics
                ));
            }

            if (frames.isEmpty()) {
                throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "영상에서 분석 가능한 프레임을 추출하지 못했습니다.");
            }

            return new VideoExtractionResult(metadata, frames);
        } catch (IOException e) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "영상 프레임 추출에 실패했습니다: " + e.getMessage(), e);
        } finally {
            try {
                grabber.stop();
            } catch (Exception ignored) {
            }
            try {
                grabber.close();
            } catch (Exception ignored) {
            }
        }
    }

    private List<Long> buildSampleTimestamps(long durationUs, int frameCount) {
        List<Long> timestamps = new ArrayList<>();
        double start = 0.15;
        double end = 0.85;

        if (frameCount <= 1) {
            timestamps.add((long) (durationUs * 0.50));
            return timestamps;
        }

        for (int i = 0; i < frameCount; i++) {
            double fraction = start + ((end - start) * ((double) i / (double) (frameCount - 1)));
            timestamps.add(Math.max(0L, Math.min(durationUs - 1, (long) (durationUs * fraction))));
        }
        return timestamps;
    }

    static int comparisonIndexFor(int frameIndex, int frameCount) {
        if (frameCount <= 1) {
            return 0;
        }
        if (frameIndex < frameCount - 1) {
            return frameIndex + 1;
        }
        return frameIndex - 1;
    }

    private BufferedImage grabFrameAt(FFmpegFrameGrabber grabber, Java2DFrameConverter converter, long timestampUs)
            throws org.bytedeco.javacv.FrameGrabber.Exception {
        grabber.setTimestamp(Math.max(0L, timestampUs));

        for (int attempt = 0; attempt < 6; attempt++) {
            Frame frame = grabber.grabImage();
            if (frame != null && frame.image != null) {
                BufferedImage converted = converter.convert(frame);
                if (converted != null) {
                    return converted;
                }
            }
        }
        return null;
    }

    private static String safeCodec(String value) {
        return value == null ? "unknown" : value;
    }

    private static double round4(double value) {
        return Math.round(value * 10_000.0) / 10_000.0;
    }

    private record SampledFrame(int index, double timestampSeconds, BufferedImage image) {
    }
}
