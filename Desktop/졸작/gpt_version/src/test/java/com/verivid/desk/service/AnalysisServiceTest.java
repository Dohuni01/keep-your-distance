package com.verivid.desk.service;

import com.verivid.desk.model.AnalyzeOptions;
import com.verivid.desk.model.ExtractedFrame;
import com.verivid.desk.model.FrameMetrics;
import com.verivid.desk.model.GptStructuredAnalysis;
import com.verivid.desk.model.VideoExtractionResult;
import com.verivid.desk.model.VideoMetadata;
import org.junit.jupiter.api.Test;

import java.nio.file.Path;
import java.util.List;

import static org.assertj.core.api.Assertions.assertThat;

class AnalysisServiceTest {

    @Test
    void calibratedAiProbabilityUsesHighestSyntheticCategory() {
        GptStructuredAnalysis analysis = new GptStructuredAnalysis(
                "FACE_SWAP_OR_DEEPFAKE",
                0.25,
                0.31,
                0.74,
                0.21,
                0.18,
                0.82,
                "",
                "summary",
                List.of("step 1", "step 2", "step 3"),
                List.of("signal"),
                List.of("evidence"),
                List.of("realism"),
                "rationale",
                List.of(),
                "caution",
                List.of()
        );

        assertThat(AnalysisService.calibratedAiProbability(analysis)).isEqualTo(0.74);
    }

    @Test
    void calibratedConfidenceCapsWeakEvidenceInputs() {
        VideoExtractionResult extraction = new VideoExtractionResult(
                new VideoMetadata(3.0, 640, 480, 30.0, "h264"),
                List.of(
                        frame(1),
                        frame(2),
                        frame(3)
                )
        );
        AnalyzeOptions options = new AnalyzeOptions(3, "low", false, null);

        assertThat(AnalysisService.calibratedConfidence(0.95, extraction, options)).isEqualTo(0.6);
        assertThat(AnalysisService.confidenceCapReason("", extraction, options))
                .contains("샘플 프레임 수")
                .contains("heatmap evidence")
                .contains("low detail")
                .contains("보조 수치");
    }

    private static ExtractedFrame frame(int index) {
        return new ExtractedFrame(
                index,
                index,
                Path.of("raw-" + index + ".jpg"),
                Path.of("heat-" + index + ".jpg"),
                new FrameMetrics(0.0, 0.0, 0.0, 0.0)
        );
    }
}
