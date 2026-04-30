package com.verivid.desk.model;

import java.util.List;

public record FrameEvidence(
        int index,
        double timestampSeconds,
        String rawImageUrl,
        String heatmapImageUrl,
        String label,
        double suspiciousScore,
        String observedArea,
        String shortNote,
        List<String> cues,
        List<String> counterCues,
        String metricInterpretation,
        List<String> caveats,
        FrameMetrics metrics
) {
}
