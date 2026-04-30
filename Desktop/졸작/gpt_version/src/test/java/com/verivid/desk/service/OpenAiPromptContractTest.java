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
    void instructionsDescribeEvidenceCounterEvidenceLimitsAndScoreCalibration() throws Exception {
        String instructions = new ClassPathResource("openai/video-analysis-instructions.txt")
                .getContentAsString(StandardCharsets.UTF_8);

        assertThat(instructions)
                .contains("증거-반증-한계")
                .contains("0.00~0.20")
                .contains("0.81~1.00")
                .contains("primary_signals에는")
                .contains("realism_signals에는")
                .contains("score_rationale에는");
    }

    @Test
    void schemaRequiresExplainableSummaryAndFrameEvidenceFields() throws Exception {
        String schemaJson = new ClassPathResource("openai/analysis-schema.json")
                .getContentAsString(StandardCharsets.UTF_8);
        JsonNode schema = objectMapper.readTree(schemaJson);

        assertThat(requiredNames(schema))
                .contains(
                        "analysis_process",
                        "visual_evidence",
                        "realism_signals",
                        "score_rationale"
                );

        JsonNode frameSchema = schema.path("properties").path("frame_findings").path("items");
        assertThat(requiredNames(frameSchema))
                .contains(
                        "observed_area",
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
