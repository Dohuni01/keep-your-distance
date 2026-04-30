package com.verivid.desk.service;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.verivid.desk.config.VerividProperties;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.web.server.ResponseStatusException;

import java.util.List;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

class OpenAiVisionServiceTest {

    private final OpenAiVisionService service = new OpenAiVisionService(new ObjectMapper(), new VerividProperties());

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

        assertThat(requestJson).contains("\"max_output_tokens\" : 6000");
    }
}
