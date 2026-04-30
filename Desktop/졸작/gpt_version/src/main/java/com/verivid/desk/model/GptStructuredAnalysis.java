package com.verivid.desk.model;

import java.util.List;

public record GptStructuredAnalysis(
        String verdict,
        double ai_probability,
        double ai_generated_probability,
        double deepfake_probability,
        double edited_probability,
        double real_camera_probability,
        double confidence,
        String confidence_cap_reason,
        String short_summary,
        List<String> analysis_process,
        List<String> primary_signals,
        List<String> visual_evidence,
        List<String> realism_signals,
        String score_rationale,
        List<String> limitations,
        String caution,
        List<GptFrameFinding> frame_findings
) {
}
