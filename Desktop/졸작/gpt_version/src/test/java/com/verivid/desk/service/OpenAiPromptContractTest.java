package com.verivid.desk.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import org.springframework.core.io.ClassPathResource;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import static org.assertj.core.api.Assertions.assertThat;

class OpenAiPromptContractTest {

    private final ObjectMapper objectMapper = new ObjectMapper();

    @Test
    void instructionsDescribeDeepfakeEvidenceLimitsAndScoreCalibration() throws Exception {
        String instructions = new ClassPathResource("openai/video-analysis-instructions.txt")
                .getContentAsString(StandardCharsets.UTF_8);

        assertThat(instructions)
                .contains("evidence-first")
                .contains("FACE_SWAP_OR_DEEPFAKE")
                .contains("입/치아/혀/립싱크")
                .contains("confidence_cap_reason")
                .contains("0.00~0.20")
                .contains("0.81~1.00")
                .contains("metric_interpretation");
    }

    @Test
    void schemaRequiresDeepfakeProbabilitiesAndFrameEvidenceFields() throws Exception {
        String schemaJson = new ClassPathResource("openai/analysis-schema.json")
                .getContentAsString(StandardCharsets.UTF_8);
        JsonNode schema = objectMapper.readTree(schemaJson);

        assertThat(requiredNames(schema))
                .contains(
                        "ai_generated_probability",
                        "deepfake_probability",
                        "edited_probability",
                        "real_camera_probability",
                        "confidence_cap_reason",
                        "analysis_process",
                        "visual_evidence",
                        "realism_signals",
                        "score_rationale"
                );

        List<String> verdicts = new ArrayList<>();
        schema.path("properties").path("verdict").path("enum").forEach(node -> verdicts.add(node.asText()));
        assertThat(verdicts)
                .contains("AI_GENERATED", "FACE_SWAP_OR_DEEPFAKE", "EDITED_OR_COMPOSITED");

        JsonNode frameSchema = schema.path("properties").path("frame_findings").path("items");
        assertThat(requiredNames(frameSchema))
                .contains(
                        "observed_area",
                        "face_consistency",
                        "temporal_consistency",
                        "counter_cues",
                        "metric_interpretation"
                );
    }

    private static List<String> requiredNames(JsonNode schema) {
        List<String> names = new ArrayList<>();
        schema.path("required").forEach(node -> names.add(node.asText()));
        return names;
    }
}
