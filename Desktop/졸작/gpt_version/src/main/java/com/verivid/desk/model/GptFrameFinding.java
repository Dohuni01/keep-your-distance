package com.verivid.desk.model;

import java.util.List;

public record GptFrameFinding(
        int frame_index,
        String label,
        double suspicious_score,
        String observed_area,
        String short_note,
        List<String> cues,
        List<String> counter_cues,
        String metric_interpretation,
        List<String> caveats
) {
}
