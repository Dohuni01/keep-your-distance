package com.verivid.desk.service;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.verivid.desk.config.VerividProperties;
import com.verivid.desk.model.ExtractedFrame;
import com.verivid.desk.model.FrameMetrics;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.web.server.ResponseStatusException;

import javax.imageio.ImageIO;
import java.awt.*;
import java.awt.image.BufferedImage;
import java.net.http.HttpClient;
import java.nio.file.Path;
import java.util.List;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

class OpenAiVisionServiceTest {

    private final OpenAiVisionService service = new OpenAiVisionService(new ObjectMapper(), new VerividProperties());

    @TempDir
    Path tempDir;

    OpenAiVisionServiceTest() throws Exception {
    }

    @Test
    void parseStructuredResponseReportsIncompleteResponsesBeforeParsingOutputText() {
        String responseBody = """
                {
                  "status": "incomplete",
                  "incomplete_details": {
                    "reason": "max_output_tokens"
                  },
                  "output": [
                    {
                      "content": [
                        {
                          "type": "output_text",
                          "text": "{\\"short_summary\\":\\"unfinished"
                        }
                      ]
                    }
                  ]
                }
                """;

        assertThatThrownBy(() -> ReflectionTestUtils.invokeMethod(service, "parseStructuredResponse", responseBody))
                .isInstanceOf(ResponseStatusException.class)
                .satisfies(error -> assertThat(((ResponseStatusException) error).getReason())
                        .contains("OpenAI 응답이 길이 제한으로 중간에 잘렸습니다")
                        .contains("max_output_tokens"));
    }

    @Test
    void buildRequestJsonAllowsLongStructuredVideoReports() {
        String requestJson = ReflectionTestUtils.invokeMethod(
                service,
                "buildRequestJson",
                List.of(),
                "Analyze this video.",
                "gpt-5.4-mini",
                "low",
                false
        );

        assertThat(requestJson).contains("\"max_output_tokens\" : 8000");
    }

    @Test
    void openAiHttpClientUsesHttp11ToAvoidHttp2StreamResets() {
        HttpClient httpClient = (HttpClient) ReflectionTestUtils.getField(service, "httpClient");

        assertThat(httpClient).isNotNull();
        assertThat(httpClient.version()).isEqualTo(HttpClient.Version.HTTP_1_1);
    }

    @Test
    void buildRequestJsonIncludesHeatmapEvidenceWhenRequested() throws Exception {
        Path raw = writeImage("raw.jpg", Color.BLUE);
        Path heatmap = writeImage("heatmap.jpg", Color.RED);
        ExtractedFrame frame = new ExtractedFrame(
                1,
                0.5,
                raw,
                heatmap,
                new FrameMetrics(0.21, 0.12, 0.34, 0.05)
        );

        String requestJson = ReflectionTestUtils.invokeMethod(
                service,
                "buildRequestJson",
                List.of(frame),
                "Analyze this video.",
                "gpt-5.4-mini",
                "high",
                true
        );

        assertThat(requestJson)
                .contains("Temporal-difference heatmap")
                .contains("temporalResidual=0.2100")
                .contains("data:image/jpeg;base64");
    }

    private Path writeImage(String filename, Color color) throws Exception {
        Path path = tempDir.resolve(filename);
        BufferedImage image = new BufferedImage(4, 4, BufferedImage.TYPE_INT_RGB);
        Graphics2D graphics = image.createGraphics();
        try {
            graphics.setColor(color);
            graphics.fillRect(0, 0, 4, 4);
        } finally {
            graphics.dispose();
        }
        ImageIO.write(image, "jpg", path.toFile());
        return path;
    }
}
