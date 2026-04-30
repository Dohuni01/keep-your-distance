package com.verivid.desk.controller;

import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.AnalysisReport;
import com.verivid.desk.model.AnalyzeOptions;
import com.verivid.desk.model.ReportSummary;
import com.verivid.desk.model.UiConfigResponse;
import com.verivid.desk.service.AnalysisService;
import com.verivid.desk.service.ReportStore;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import org.springframework.core.io.PathResource;
import org.springframework.core.io.Resource;
import org.springframework.http.CacheControl;
import org.springframework.http.MediaType;
import org.springframework.http.MediaTypeFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.validation.annotation.Validated;
import org.springframework.web.bind.annotation.*;
import org.springframework.web.multipart.MultipartFile;

import java.io.IOException;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;

@RestController
@RequestMapping("/api")
@Validated
public class AnalysisController {

    private final AnalysisService analysisService;
    private final ReportStore reportStore;
    private final VerividProperties properties;

    public AnalysisController(AnalysisService analysisService,
                              ReportStore reportStore,
                              VerividProperties properties) {
        this.analysisService = analysisService;
        this.reportStore = reportStore;
        this.properties = properties;
    }

    @GetMapping("/config")
    public UiConfigResponse config() {
        return new UiConfigResponse(
                properties.getAnalysis().getDefaultFrameCount(),
                properties.getAnalysis().getMaxFrameCount(),
                properties.getOpenai().getImageDetail(),
                properties.getOpenai().getModel()
        );
    }

    @PostMapping(value = "/analyze", consumes = MediaType.MULTIPART_FORM_DATA_VALUE)
    public AnalysisReport analyze(@RequestParam("file") MultipartFile file,
                                  @RequestParam(value = "frameCount", required = false, defaultValue = "6")
                                  @Min(1) @Max(20) int frameCount,
                                  @RequestParam(value = "imageDetail", required = false, defaultValue = "high")
                                  String imageDetail,
                                  @RequestParam(value = "includeHeatmapsInPrompt", required = false, defaultValue = "true")
                                  boolean includeHeatmapsInPrompt,
                                  @RequestParam(value = "model", required = false) String model) throws IOException {
        return analysisService.analyze(file, new AnalyzeOptions(frameCount, imageDetail, includeHeatmapsInPrompt, model));
    }

    @GetMapping("/reports")
    public List<ReportSummary> reports() throws IOException {
        return analysisService.listReports();
    }

    @GetMapping("/reports/{reportId}")
    public AnalysisReport report(@PathVariable String reportId) throws IOException {
        return analysisService.getReport(reportId);
    }

    @DeleteMapping("/reports/{reportId}")
    public ResponseEntity<Void> deleteReport(@PathVariable String reportId) throws IOException {
        analysisService.deleteReport(reportId);
        return ResponseEntity.noContent().build();
    }

    @DeleteMapping("/reports")
    public ResponseEntity<Void> clearReports() throws IOException {
        analysisService.clearReports();
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/reports/{reportId}/assets/{kind}/{filename:.+}")
    public ResponseEntity<Resource> asset(@PathVariable String reportId,
                                          @PathVariable String kind,
                                          @PathVariable String filename) {
        Path path = reportStore.resolveAsset(reportId, kind, filename);
        MediaType mediaType = MediaTypeFactory.getMediaType(path.getFileName().toString())
                .orElse(MediaType.APPLICATION_OCTET_STREAM);

        return ResponseEntity.ok()
                .cacheControl(CacheControl.maxAge(Duration.ofHours(1)).cachePublic())
                .contentType(mediaType)
                .body(new PathResource(path));
    }
}
