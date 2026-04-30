package com.verivid.desk.service;

import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.AnalysisReport;
import com.verivid.desk.model.AnalyzeOptions;
import com.verivid.desk.model.ExtractedFrame;
import com.verivid.desk.model.FrameEvidence;
import com.verivid.desk.model.FrameMetrics;
import com.verivid.desk.model.GptFrameFinding;
import com.verivid.desk.model.GptStructuredAnalysis;
import com.verivid.desk.model.OpenAiAnalysisEnvelope;
import com.verivid.desk.model.ReportSummary;
import com.verivid.desk.model.VerdictSummary;
import com.verivid.desk.model.VideoExtractionResult;
import org.springframework.http.HttpStatus;
import org.springframework.stereotype.Service;
import org.springframework.web.multipart.MultipartFile;
import org.springframework.web.server.ResponseStatusException;

import java.io.IOException;
import java.nio.file.Path;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

@Service
public class AnalysisService {

    private final VerividProperties properties;
    private final ReportStore reportStore;
    private final VideoFrameExtractor videoFrameExtractor;
    private final OpenAiVisionService openAiVisionService;

    public AnalysisService(VerividProperties properties,
                           ReportStore reportStore,
                           VideoFrameExtractor videoFrameExtractor,
                           OpenAiVisionService openAiVisionService) {
        this.properties = properties;
        this.reportStore = reportStore;
        this.videoFrameExtractor = videoFrameExtractor;
        this.openAiVisionService = openAiVisionService;
    }

    public AnalysisReport analyze(MultipartFile file, AnalyzeOptions options) throws IOException {
        if (file == null || file.isEmpty()) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "업로드된 파일이 없습니다.");
        }

        int frameCount = normalizeFrameCount(options.frameCount());
        String imageDetail = normalizeImageDetail(options.imageDetail());
        AnalyzeOptions normalized = new AnalyzeOptions(
                frameCount,
                imageDetail,
                options.includeHeatmapsInPrompt(),
                options.modelOverride()
        );

        String reportId = reportStore.newReportId();
        Path reportDir = reportStore.createReportDirectory(reportId);
        Path savedVideo = reportStore.saveUploadedVideo(reportId, file);

        long started = System.currentTimeMillis();

        VideoExtractionResult extraction = videoFrameExtractor.extract(savedVideo, reportDir, frameCount);
        OpenAiAnalysisEnvelope envelope = openAiVisionService.analyze(
                extraction,
                normalized,
                ReportStore.sanitizeFilename(file.getOriginalFilename())
        );

        reportStore.saveText(reportId, "prompt.txt", envelope.promptText());
        reportStore.saveText(reportId, "openai-request.json", envelope.requestJson());
        reportStore.saveText(reportId, "openai-response.json", envelope.responseJson());

        AnalysisReport report = buildReport(
                reportId,
                file,
                savedVideo,
                extraction,
                normalized,
                envelope,
                System.currentTimeMillis() - started
        );

        reportStore.saveReport(reportId, report);
        return report;
    }

    public List<ReportSummary> listReports() throws IOException {
        return reportStore.listReports();
    }

    public AnalysisReport getReport(String reportId) throws IOException {
        return reportStore.loadReport(reportId);
    }

    public void deleteReport(String reportId) throws IOException {
        reportStore.deleteReport(reportId);
    }

    public void clearReports() throws IOException {
        reportStore.clearAllReports();
    }

    private AnalysisReport buildReport(String reportId,
                                       MultipartFile file,
                                       Path savedVideo,
                                       VideoExtractionResult extraction,
                                       AnalyzeOptions options,
                                       OpenAiAnalysisEnvelope envelope,
                                       long processingMillis) {
        Map<Integer, GptFrameFinding> findingsByIndex = new HashMap<>();
        if (envelope.analysis().frame_findings() != null) {
            for (GptFrameFinding finding : envelope.analysis().frame_findings()) {
                findingsByIndex.put(finding.frame_index(), finding);
            }
        }

        List<FrameEvidence> frames = new ArrayList<>();
        for (ExtractedFrame frame : extraction.frames()) {
            GptFrameFinding finding = findingsByIndex.get(frame.index());
            if (finding == null) {
                finding = fallbackFinding(frame);
            }

            frames.add(new FrameEvidence(
                    frame.index(),
                    frame.timestampSeconds(),
                    "/api/reports/" + reportId + "/assets/raw/" + frame.rawImagePath().getFileName(),
                    "/api/reports/" + reportId + "/assets/heatmap/" + frame.heatmapImagePath().getFileName(),
                    finding.label(),
                    finding.suspicious_score(),
                    defaultText(finding.observed_area(), "샘플 프레임 전체"),
                    finding.short_note(),
                    safeList(finding.cues()),
                    safeList(finding.counter_cues()),
                    metricInterpretation(finding),
                    safeList(finding.caveats()),
                    frame.metrics()
            ));
        }

        GptStructuredAnalysis analysis = envelope.analysis();
        double aiProbability = calibratedAiProbability(analysis);
        double confidence = calibratedConfidence(analysis.confidence(), extraction, options);
        String confidenceCapReason = confidenceCapReason(analysis.confidence_cap_reason(), extraction, options);

        VerdictSummary summary = new VerdictSummary(
                analysis.verdict(),
                aiProbability,
                analysis.ai_generated_probability(),
                analysis.deepfake_probability(),
                analysis.edited_probability(),
                analysis.real_camera_probability(),
                confidence,
                confidenceCapReason,
                analysis.short_summary(),
                safeList(analysis.analysis_process()),
                safeList(analysis.primary_signals()),
                safeList(analysis.visual_evidence()),
                safeList(analysis.realism_signals()),
                defaultText(analysis.score_rationale(), "점수 산정 이유가 응답에 포함되지 않았습니다."),
                safeList(analysis.limitations()),
                analysis.caution()
        );

        return new AnalysisReport(
                reportId,
                Instant.now(),
                file.getOriginalFilename(),
                file.getSize(),
                envelope.model(),
                options.frameCount(),
                options.imageDetail(),
                options.includeHeatmapsInPrompt(),
                processingMillis,
                "/api/reports/" + reportId + "/assets/input/" + savedVideo.getFileName(),
                extraction.metadata(),
                summary,
                frames,
                List.of(
                        "이 결과는 샘플 프레임 기반 확률 판단이며 법적 감정 결과가 아닙니다.",
                        "heatmap은 프레임 변화량 시각화이며 단독 증거가 아닙니다.",
                        "압축, 화면녹화, 보정, 조명, 모션블러는 AI처럼 보이는 오판 요인이 될 수 있습니다."
                )
        );
    }

    private GptFrameFinding fallbackFinding(ExtractedFrame frame) {
        return new GptFrameFinding(
                frame.index(),
                "unclear",
                fallbackScore(frame.metrics()),
                "샘플 프레임 전체",
                "모델이 이 프레임에 대한 구체 항목을 충분히 반환하지 않았습니다.",
                "얼굴 일관성 근거가 응답에 포함되지 않았습니다.",
                "시간 일관성 근거가 응답에 포함되지 않았습니다.",
                List.of("시각 단서 부족"),
                List.of("샘플 프레임만으로는 AI 여부를 단정하기 어렵습니다."),
                "보조 수치는 참고값이며 단독 근거로 쓰지 않습니다.",
                List.of("샘플 프레임 기반 분석의 한계가 있습니다.")
        );
    }

    private int normalizeFrameCount(int frameCount) {
        if (frameCount <= 0) {
            return properties.getAnalysis().getDefaultFrameCount();
        }
        if (frameCount > properties.getAnalysis().getMaxFrameCount()) {
            throw new ResponseStatusException(
                    HttpStatus.BAD_REQUEST,
                    "frameCount는 최대 " + properties.getAnalysis().getMaxFrameCount() + "까지 가능합니다."
            );
        }
        return frameCount;
    }

    private String normalizeImageDetail(String detail) {
        String value = (detail == null || detail.isBlank())
                ? properties.getOpenai().getImageDetail()
                : detail.trim().toLowerCase(Locale.ROOT);

        if (!value.equals("low") && !value.equals("auto") && !value.equals("high")) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "imageDetail은 low, auto, high 중 하나여야 합니다.");
        }
        return value;
    }

    private double fallbackScore(FrameMetrics metrics) {
        double weighted = (metrics.temporalResidual() * 0.45)
                + (metrics.edgeJump() * 0.20)
                + (metrics.sharpnessSwing() * 0.25)
                + (metrics.colorShift() * 0.10);
        return round3(weighted);
    }

    static double calibratedAiProbability(GptStructuredAnalysis analysis) {
        double categoryMax = Math.max(
                Math.max(analysis.ai_generated_probability(), analysis.deepfake_probability()),
                analysis.edited_probability()
        );
        return round3(Math.max(analysis.ai_probability(), categoryMax));
    }

    static double calibratedConfidence(double modelConfidence, VideoExtractionResult extraction, AnalyzeOptions options) {
        return round3(Math.min(modelConfidence, confidenceCap(extraction, options)));
    }

    static String confidenceCapReason(String modelReason, VideoExtractionResult extraction, AnalyzeOptions options) {
        List<String> reasons = new ArrayList<>();
        if (modelReason != null && !modelReason.isBlank()) {
            reasons.add(modelReason);
        }
        if (extraction.frames().size() <= 3) {
            reasons.add("샘플 프레임 수가 적어 confidence를 제한했습니다.");
        }
        if (!options.includeHeatmapsInPrompt()) {
            reasons.add("heatmap evidence가 GPT 입력에 포함되지 않아 confidence를 제한했습니다.");
        }
        if ("low".equalsIgnoreCase(options.imageDetail())) {
            reasons.add("low detail 이미지 입력이라 미세한 얼굴/경계 단서 confidence를 제한했습니다.");
        }
        if (allMetricsNearZero(extraction)) {
            reasons.add("프레임 간 보조 수치가 모두 0에 가까워 시간 변화 근거 confidence를 제한했습니다.");
        }
        return String.join(" ", reasons);
    }

    private static double confidenceCap(VideoExtractionResult extraction, AnalyzeOptions options) {
        double cap = 0.92;
        if (extraction.frames().size() <= 3) {
            cap = Math.min(cap, 0.68);
        }
        if (!options.includeHeatmapsInPrompt()) {
            cap = Math.min(cap, 0.70);
        }
        if ("low".equalsIgnoreCase(options.imageDetail())) {
            cap = Math.min(cap, 0.72);
        }
        if (allMetricsNearZero(extraction)) {
            cap = Math.min(cap, 0.60);
        }
        return cap;
    }

    private static boolean allMetricsNearZero(VideoExtractionResult extraction) {
        return extraction.frames().stream().allMatch(frame -> {
            FrameMetrics metrics = frame.metrics();
            return metrics.temporalResidual() < 0.0001
                    && metrics.edgeJump() < 0.0001
                    && metrics.sharpnessSwing() < 0.0001
                    && metrics.colorShift() < 0.0001;
        });
    }

    private List<String> safeList(List<String> source) {
        if (source == null || source.isEmpty()) {
            return List.of();
        }
        return source;
    }

    private String metricInterpretation(GptFrameFinding finding) {
        String face = defaultText(finding.face_consistency(), "얼굴 일관성 설명 없음");
        String temporal = defaultText(finding.temporal_consistency(), "시간 일관성 설명 없음");
        return defaultText(finding.metric_interpretation(), "보조 수치는 시각 단서와 함께 참고해야 합니다.")
                + " 얼굴 일관성: " + face
                + " 시간 일관성: " + temporal;
    }

    private String defaultText(String source, String fallback) {
        if (source == null || source.isBlank()) {
            return fallback;
        }
        return source;
    }

    private static double round3(double value) {
        return Math.round(Math.max(0.0, Math.min(1.0, value)) * 1000.0) / 1000.0;
    }
}
