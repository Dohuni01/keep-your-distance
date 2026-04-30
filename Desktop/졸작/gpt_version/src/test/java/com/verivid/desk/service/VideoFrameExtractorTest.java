package com.verivid.desk.service;

import org.junit.jupiter.api.Test;

import static org.assertj.core.api.Assertions.assertThat;

class VideoFrameExtractorTest {

    @Test
    void comparisonFrameUsesAdjacentSampleInsteadOfTimestampSeekNeighbor() {
        assertThat(VideoFrameExtractor.comparisonIndexFor(0, 4)).isEqualTo(1);
        assertThat(VideoFrameExtractor.comparisonIndexFor(1, 4)).isEqualTo(2);
        assertThat(VideoFrameExtractor.comparisonIndexFor(2, 4)).isEqualTo(3);
        assertThat(VideoFrameExtractor.comparisonIndexFor(3, 4)).isEqualTo(2);
    }

    @Test
    void singleFrameComparesToItselfWhenNoAdjacentSampleExists() {
        assertThat(VideoFrameExtractor.comparisonIndexFor(0, 1)).isEqualTo(0);
    }
}
