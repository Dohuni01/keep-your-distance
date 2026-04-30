package com.verivid.desk.model;

import java.util.List;

public record VerdictSummary(
        String verdict,
        double aiProbability,
        double confidence,
        String shortSummary,
        List<String> analysisProcess,
        List<String> primarySignals,
        List<String> visualEvidence,
        List<String> realismSignals,
        String scoreRationale,
        List<String> limitations,
        String caution
) {
}
