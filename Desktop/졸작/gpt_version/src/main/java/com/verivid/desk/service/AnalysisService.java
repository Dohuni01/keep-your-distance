package com.verivid.desk.service;

import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.*;
import org.springframework.http.HttpStatus;
import org.springframework.stereotype.Service;
import org.springframework.web.multipart.MultipartFile;
import org.springframework.web.server.ResponseStatusException;

import java.io.IOException;
import java.nio.file.Path;
import java.time.Instant;
import java.util.*;

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
                finding = new GptFrameFinding(
                        frame.index(),
                        "unclear",
                        fallbackScore(frame.metrics()),
                        "대표 프레임 전체",
                        "모델이 이 프레임에 대한 구체 항목을 충분히 반환하지 않았습니다.",
                        List.of("시각 단서 부족"),
                        List.of("대표 프레임만으로는 AI 여부를 단정하기 어렵습니다."),
                        "보조 수치는 참고값이며 단독 근거로 쓰지 않았습니다.",
                        List.of("대표 프레임만으로는 한계가 있습니다.")
                );
            }

            frames.add(new FrameEvidence(
                    frame.index(),
                    frame.timestampSeconds(),
                    "/api/reports/" + reportId + "/assets/raw/" + frame.rawImagePath().getFileName(),
                    "/api/reports/" + reportId + "/assets/heatmap/" + frame.heatmapImagePath().getFileName(),
                    finding.label(),
                    finding.suspicious_score(),
                    defaultText(finding.observed_area(), "대표 프레임 전체"),
                    finding.short_note(),
                    safeList(finding.cues()),
                    safeList(finding.counter_cues()),
                    defaultText(finding.metric_interpretation(), "보조 수치는 시각 단서와 함께 참고해야 합니다."),
                    safeList(finding.caveats()),
                    frame.metrics()
            ));
        }

        VerdictSummary summary = new VerdictSummary(
                envelope.analysis().verdict(),
                envelope.analysis().ai_probability(),
                envelope.analysis().confidence(),
                envelope.analysis().short_summary(),
                safeList(envelope.analysis().analysis_process()),
                safeList(envelope.analysis().primary_signals()),
                safeList(envelope.analysis().visual_evidence()),
                safeList(envelope.analysis().realism_signals()),
                defaultText(envelope.analysis().score_rationale(), "점수 산정 이유가 응답에 포함되지 않았습니다."),
                safeList(envelope.analysis().limitations()),
                envelope.analysis().caution()
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
                        "이 결과는 대표 프레임 기반의 확률적 판단이며 법적 감정 결과가 아닙니다.",
                        "heatmap은 프레임 변화량 시각화이며 단독 증거가 아닙니다.",
                        "beauty filter, denoise, 압축, 저조도 촬영은 AI처럼 보일 수 있습니다."
                )
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
        return Math.round(Math.min(1.0, weighted) * 1000.0) / 1000.0;
    }

    private List<String> safeList(List<String> source) {
        if (source == null || source.isEmpty()) {
            return List.of();
        }
        return source;
    }

    private String defaultText(String source, String fallback) {
        if (source == null || source.isBlank()) {
            return fallback;
        }
        return source;
    }
}
