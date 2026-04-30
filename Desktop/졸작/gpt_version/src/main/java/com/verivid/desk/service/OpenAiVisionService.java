package com.verivid.desk.service;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.AnalyzeOptions;
import com.verivid.desk.model.ExtractedFrame;
import com.verivid.desk.model.GptStructuredAnalysis;
import com.verivid.desk.model.OpenAiAnalysisEnvelope;
import com.verivid.desk.model.VideoExtractionResult;
import com.verivid.desk.model.VideoMetadata;
import com.verivid.desk.util.ImageUtils;
import org.springframework.core.io.ClassPathResource;
import org.springframework.http.HttpStatus;
import org.springframework.stereotype.Service;
import org.springframework.web.server.ResponseStatusException;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

@Service
public class OpenAiVisionService {

    private final ObjectMapper objectMapper;
    private final VerividProperties properties;
    private final HttpClient httpClient;
    private final String baseInstructions;
    private final Map<String, Object> schemaMap;

    public OpenAiVisionService(ObjectMapper objectMapper, VerividProperties properties) throws IOException {
        this.objectMapper = objectMapper;
        this.properties = properties;
        this.httpClient = HttpClient.newBuilder()
                .version(HttpClient.Version.HTTP_1_1)
                .connectTimeout(Duration.ofSeconds(20))
                .build();
        this.baseInstructions = new ClassPathResource("openai/video-analysis-instructions.txt")
                .getContentAsString(StandardCharsets.UTF_8);
        this.schemaMap = objectMapper.readValue(
                new ClassPathResource("openai/analysis-schema.json").getContentAsString(StandardCharsets.UTF_8),
                new TypeReference<Map<String, Object>>() {
                }
        );
    }

    public OpenAiAnalysisEnvelope analyze(VideoExtractionResult extraction,
                                          AnalyzeOptions options,
                                          String originalFilename) {
        String apiKey = properties.getOpenai().getApiKey();
        if (apiKey == null || apiKey.isBlank()) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "OPENAI_API_KEY가 설정되지 않았습니다.");
        }

        String model = normalizeModel(options.modelOverride());
        String imageDetail = normalizeImageDetail(options.imageDetail());
        String promptText = buildPrompt(originalFilename, extraction.metadata(), extraction.frames(), options.includeHeatmapsInPrompt());
        String requestJson = buildRequestJson(extraction.frames(), promptText, model, imageDetail, options.includeHeatmapsInPrompt());
        String responseBody = sendRequest(requestJson, apiKey);
        GptStructuredAnalysis structured = parseStructuredResponse(responseBody);

        return new OpenAiAnalysisEnvelope(model, structured, promptText, requestJson, responseBody);
    }

    private String buildRequestJson(List<ExtractedFrame> frames,
                                    String promptText,
                                    String model,
                                    String imageDetail,
                                    boolean includeHeatmapsInPrompt) {
        try {
            List<Map<String, Object>> content = new ArrayList<>();
            content.add(Map.of("type", "input_text", "text", promptText));

            for (ExtractedFrame frame : frames) {
                content.add(Map.of(
                        "type", "input_text",
                        "text", String.format(
                                "Frame %d | timestamp=%.3fs | temporalResidual=%.4f | edgeJump=%.4f | sharpnessSwing=%.4f | colorShift=%.4f",
                                frame.index(),
                                frame.timestampSeconds(),
                                frame.metrics().temporalResidual(),
                                frame.metrics().edgeJump(),
                                frame.metrics().sharpnessSwing(),
                                frame.metrics().colorShift()
                        )
                ));

                content.add(Map.of(
                        "type", "input_image",
                        "image_url", ImageUtils.toDataUrl(frame.rawImagePath()),
                        "detail", imageDetail
                ));

                if (includeHeatmapsInPrompt) {
                    content.add(Map.of(
                            "type", "input_text",
                            "text", "Heatmap for previous frame. This is a frame-difference overlay, not direct proof of AI generation. Use it only to explain motion/change together with the raw frame."
                    ));
                    content.add(Map.of(
                            "type", "input_image",
                            "image_url", ImageUtils.toDataUrl(frame.heatmapImagePath()),
                            "detail", imageDetail
                    ));
                }
            }

            Map<String, Object> payload = new LinkedHashMap<>();
            payload.put("model", model);
            payload.put("store", false);
            payload.put("max_output_tokens", 6000);
            payload.put("input", List.of(Map.of(
                    "role", "user",
                    "content", content
            )));
            payload.put("text", Map.of(
                    "format", Map.of(
                            "type", "json_schema",
                            "name", "video_ai_authenticity_report",
                            "strict", true,
                            "schema", schemaMap
                    )
            ));

            return objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(payload);
        } catch (IOException e) {
            throw new ResponseStatusException(HttpStatus.INTERNAL_SERVER_ERROR, "OpenAI 요청 JSON 생성에 실패했습니다.", e);
        }
    }

    private String sendRequest(String requestJson, String apiKey) {
        try {
            HttpRequest request = HttpRequest.newBuilder()
                    .uri(URI.create(properties.getOpenai().getBaseUrl()))
                    .timeout(Duration.ofSeconds(Math.max(30, properties.getOpenai().getTimeoutSeconds())))
                    .header("Authorization", "Bearer " + apiKey)
                    .header("Content-Type", "application/json")
                    .POST(HttpRequest.BodyPublishers.ofString(requestJson, StandardCharsets.UTF_8))
                    .build();

            HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
            if (response.statusCode() < 200 || response.statusCode() >= 300) {
                throw new ResponseStatusException(
                        HttpStatus.BAD_GATEWAY,
                        "OpenAI API 호출 실패 (" + response.statusCode() + "): " + response.body()
                );
            }
            return response.body();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new ResponseStatusException(HttpStatus.BAD_GATEWAY, "OpenAI API 통신이 인터럽트되었습니다: " + e.getMessage(), e);
        } catch (IOException e) {
            throw new ResponseStatusException(HttpStatus.BAD_GATEWAY, "OpenAI API 통신에 실패했습니다: " + e.getMessage(), e);
        }
    }

    private GptStructuredAnalysis parseStructuredResponse(String responseBody) {
        try {
            JsonNode root = objectMapper.readTree(responseBody);
            rejectIncompleteResponse(root);
            String outputText = extractOutputText(root);

            if (outputText == null || outputText.isBlank()) {
                throw new ResponseStatusException(HttpStatus.BAD_GATEWAY, "OpenAI 응답에서 구조화된 결과를 찾지 못했습니다.");
            }

            JsonNode structured = objectMapper.readTree(outputText);
            return objectMapper.treeToValue(structured, GptStructuredAnalysis.class);
        } catch (IOException e) {
            throw new ResponseStatusException(HttpStatus.BAD_GATEWAY, "OpenAI 응답 파싱에 실패했습니다: " + e.getMessage(), e);
        }
    }

    private void rejectIncompleteResponse(JsonNode root) {
        String reason = incompleteReason(root);
        if (reason == null) {
            return;
        }

        throw new ResponseStatusException(
                HttpStatus.BAD_GATEWAY,
                "OpenAI 응답이 길이 제한으로 중간에 잘렸습니다. reason=" + reason
                        + ". 프레임 수를 줄이거나 max_output_tokens 설정을 늘려주세요."
        );
    }

    private String incompleteReason(JsonNode root) {
        if ("incomplete".equals(root.path("status").asText())) {
            return root.path("incomplete_details").path("reason").asText("unknown");
        }

        JsonNode output = root.path("output");
        if (output.isArray()) {
            for (JsonNode item : output) {
                if ("incomplete".equals(item.path("status").asText())) {
                    return item.path("incomplete_details").path("reason").asText("unknown");
                }
            }
        }
        return null;
    }

    private String extractOutputText(JsonNode root) {
        JsonNode direct = root.get("output_text");
        if (direct != null && direct.isTextual()) {
            return direct.asText();
        }

        JsonNode output = root.get("output");
        if (output != null && output.isArray()) {
            for (JsonNode item : output) {
                JsonNode content = item.get("content");
                if (content != null && content.isArray()) {
                    for (JsonNode part : content) {
                        if ("output_text".equals(part.path("type").asText())) {
                            return part.path("text").asText();
                        }
                    }
                }
            }
        }
        return null;
    }

    private String buildPrompt(String originalFilename,
                               VideoMetadata metadata,
                               List<ExtractedFrame> frames,
                               boolean includeHeatmapsInPrompt) {
        StringBuilder prompt = new StringBuilder();
        prompt.append(baseInstructions).append("\n\n");
        prompt.append("입력 메타데이터\n");
        prompt.append("- 파일명: ").append(originalFilename).append("\n");
        prompt.append("- 영상 길이(초): ").append(metadata.durationSeconds()).append("\n");
        prompt.append("- 해상도: ").append(metadata.width()).append("x").append(metadata.height()).append("\n");
        prompt.append("- FPS: ").append(metadata.frameRate()).append("\n");
        prompt.append("- 코덱: ").append(metadata.codec()).append("\n");
        prompt.append("- 대표 프레임 수: ").append(frames.size()).append("\n");
        prompt.append("- heatmap 이미지 포함 여부: ").append(includeHeatmapsInPrompt).append("\n\n");

        prompt.append("프레임 순서는 시간순입니다.\n");
        prompt.append("각 프레임에 대해 구체적인 시각 단서와 해석 한계를 적고, 마지막에 영상 전체 요약을 내려주세요.\n");
        prompt.append("각 프레임은 observed_area, cues, counter_cues, metric_interpretation이 서로 모순되지 않게 작성하세요.\n");
        prompt.append("최종 요약은 analysis_process, visual_evidence, realism_signals, score_rationale을 발표자가 그대로 설명할 수 있게 작성하세요.\n");
        prompt.append("모호하면 INCONCLUSIVE를 선택하세요.\n");
        prompt.append("frame_findings에는 전달된 모든 프레임에 대한 항목을 넣으세요.\n\n");

        prompt.append("프레임별 보조 수치 요약\n");
        for (ExtractedFrame frame : frames) {
            prompt.append(String.format(
                    "- Frame %d | %.3fs | temporalResidual=%.4f | edgeJump=%.4f | sharpnessSwing=%.4f | colorShift=%.4f%n",
                    frame.index(),
                    frame.timestampSeconds(),
                    frame.metrics().temporalResidual(),
                    frame.metrics().edgeJump(),
                    frame.metrics().sharpnessSwing(),
                    frame.metrics().colorShift()
            ));
        }

        return prompt.toString();
    }

    private String normalizeModel(String override) {
        if (override != null && !override.isBlank()) {
            return override.trim();
        }
        return properties.getOpenai().getModel();
    }

    private String normalizeImageDetail(String input) {
        String fallback = properties.getOpenai().getImageDetail();
        String value = (input == null || input.isBlank()) ? fallback : input.trim().toLowerCase();
        if (!value.equals("low") && !value.equals("auto") && !value.equals("high")) {
            return fallback;
        }
        return value;
    }
}
