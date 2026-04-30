const state = {
    config: null,
    reports: [],
    current: null
};

const els = {
    form: document.getElementById('analyzeForm'),
    fileInput: document.getElementById('fileInput'),
    fileMeta: document.getElementById('fileMeta'),
    frameCount: document.getElementById('frameCount'),
    imageDetail: document.getElementById('imageDetail'),
    model: document.getElementById('model'),
    includeHeatmaps: document.getElementById('includeHeatmaps'),
    submitButton: document.getElementById('submitButton'),
    jobState: document.getElementById('jobState'),
    reportSection: document.getElementById('reportSection'),
    framesSection: document.getElementById('framesSection'),
    summaryHeadline: document.getElementById('summaryHeadline'),
    summaryCopy: document.getElementById('summaryCopy'),
    aiProbability: document.getElementById('aiProbability'),
    confidence: document.getElementById('confidence'),
    verdictPill: document.getElementById('verdictPill'),
    cautionText: document.getElementById('cautionText'),
    analysisProcessList: document.getElementById('analysisProcessList'),
    signalsList: document.getElementById('signalsList'),
    visualEvidenceList: document.getElementById('visualEvidenceList'),
    realismSignalsList: document.getElementById('realismSignalsList'),
    scoreRationaleText: document.getElementById('scoreRationaleText'),
    limitationsList: document.getElementById('limitationsList'),
    metaGrid: document.getElementById('metaGrid'),
    framesGrid: document.getElementById('framesGrid'),
    globalCaveats: document.getElementById('globalCaveats'),
    recentList: document.getElementById('recentList'),
    refreshReports: document.getElementById('refreshReports'),
    clearReports: document.getElementById('clearReports'),
    directionVerdict: document.getElementById('directionVerdict'),
    directionSummary: document.getElementById('directionSummary')
};

document.querySelectorAll('[data-scroll]').forEach(btn => {
    btn.addEventListener('click', () => {
        const target = document.querySelector(btn.dataset.scroll);
        if (target) target.scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
});

els.fileInput.addEventListener('change', () => {
    const file = els.fileInput.files?.[0];
    if (!file) {
        els.fileMeta.textContent = '클릭해서 파일을 선택하세요';
        return;
    }
    const mb = (file.size / (1024 * 1024)).toFixed(2);
    els.fileMeta.textContent = `${file.name} · ${mb}MB`;
});

els.form.addEventListener('submit', async (event) => {
    event.preventDefault();
    const file = els.fileInput.files?.[0];
    if (!file) {
        alert('분석할 영상을 선택하세요.');
        return;
    }

    const formData = new FormData();
    formData.append('file', file);
    formData.append('frameCount', String(els.frameCount.value || 3));
    formData.append('imageDetail', els.imageDetail.value);
    formData.append('includeHeatmapsInPrompt', String(els.includeHeatmaps.checked));
    if (els.model.value.trim()) {
        formData.append('model', els.model.value.trim());
    }

    setBusy(true, '프레임 추출 및 GPT 분석 중...');
    try {
        const response = await fetch('/api/analyze', {
            method: 'POST',
            body: formData
        });

        if (!response.ok) {
            const message = await readError(response);
            throw new Error(message);
        }

        const report = await response.json();
        state.current = report;
        renderReport(report);
        await loadReports();
        document.getElementById('reportSection').scrollIntoView({ behavior: 'smooth', block: 'start' });
    } catch (error) {
        alert(error.message || '분석에 실패했습니다.');
    } finally {
        setBusy(false, '대기 중');
    }
});

els.refreshReports.addEventListener('click', loadReports);

els.clearReports.addEventListener('click', async () => {
    if (!confirm('최근 분석 기록을 모두 삭제할까요?')) return;
    await fetch('/api/reports', { method: 'DELETE' });
    state.current = null;
    hideReport();
    await loadReports();
});

window.addEventListener('DOMContentLoaded', async () => {
    await loadConfig();
    await loadReports();
});

async function loadConfig() {
    try {
        const res = await fetch('/api/config');
        if (!res.ok) return;
        state.config = await res.json();
        els.frameCount.value = state.config.defaultFrameCount;
        els.frameCount.max = state.config.maxFrameCount;
        els.imageDetail.value = state.config.defaultImageDetail;
        els.model.placeholder = `비워두면 서버 기본 모델 사용 (${state.config.defaultModel})`;
    } catch (_) {
    }
}

async function loadReports() {
    try {
        const res = await fetch('/api/reports');
        if (!res.ok) throw new Error('최근 기록을 불러오지 못했습니다.');
        state.reports = await res.json();
        renderRecentReports();
    } catch (error) {
        els.recentList.innerHTML = `<div class="empty-state">${escapeHtml(error.message)}</div>`;
    }
}

function renderRecentReports() {
    if (!state.reports.length) {
        els.recentList.innerHTML = '<div class="empty-state">저장된 분석 기록이 없습니다.</div>';
        return;
    }

    els.recentList.innerHTML = '';
    state.reports.forEach(report => {
        const item = document.createElement('div');
        item.className = 'recent-item';

        const main = document.createElement('div');
        main.className = 'recent-item-main';
        main.innerHTML = `
            <div class="recent-item-title">${escapeHtml(report.originalFilename)}</div>
            <div class="recent-item-subtitle">
                ${formatDate(report.createdAt)} · ${escapeHtml(toVerdictLabel(report.verdict))}
                · 점수 ${toFixed3(report.aiProbability)} · 신뢰도 ${toFixed3(report.confidence)}
            </div>
            <div class="recent-item-subtitle">${escapeHtml(report.shortSummary || '')}</div>
        `;
        main.addEventListener('click', () => loadSingleReport(report.reportId));

        const badge = document.createElement('div');
        badge.className = 'small-tag';
        badge.textContent = `${report.frameCount} frame${report.frameCount > 1 ? 's' : ''}`;

        const deleteButton = document.createElement('button');
        deleteButton.className = 'btn btn-danger';
        deleteButton.textContent = '삭제';
        deleteButton.addEventListener('click', async (event) => {
            event.stopPropagation();
            if (!confirm('이 리포트를 삭제할까요?')) return;
            await fetch(`/api/reports/${encodeURIComponent(report.reportId)}`, { method: 'DELETE' });
            if (state.current?.reportId === report.reportId) {
                hideReport();
            }
            await loadReports();
        });

        item.appendChild(main);
        item.appendChild(badge);
        item.appendChild(deleteButton);
        els.recentList.appendChild(item);
    });
}

async function loadSingleReport(reportId) {
    setBusy(true, '저장된 리포트를 불러오는 중...');
    try {
        const res = await fetch(`/api/reports/${encodeURIComponent(reportId)}`);
        if (!res.ok) throw new Error(await readError(res));
        const report = await res.json();
        state.current = report;
        renderReport(report);
        document.getElementById('reportSection').scrollIntoView({ behavior: 'smooth', block: 'start' });
    } catch (error) {
        alert(error.message || '리포트를 불러오지 못했습니다.');
    } finally {
        setBusy(false, '대기 중');
    }
}

function renderReport(report) {
    els.reportSection.classList.remove('hidden');
    els.framesSection.classList.remove('hidden');

    const verdictLabel = toVerdictLabel(report.summary.verdict);
    els.summaryHeadline.textContent = verdictHeadline(report.summary.verdict);
    els.summaryCopy.textContent = report.summary.shortSummary || '';
    els.aiProbability.textContent = toFixed3(report.summary.aiProbability);
    els.confidence.textContent = toFixed3(report.summary.confidence);
    els.verdictPill.textContent = verdictLabel;
    els.cautionText.textContent = report.summary.caution || '';

    els.directionVerdict.textContent = verdictLabel;
    els.directionSummary.textContent = report.summary.shortSummary || '';

    renderList(els.analysisProcessList, report.summary.analysisProcess);
    renderList(els.signalsList, report.summary.primarySignals);
    renderList(els.visualEvidenceList, report.summary.visualEvidence);
    renderList(els.realismSignalsList, report.summary.realismSignals);
    els.scoreRationaleText.textContent = report.summary.scoreRationale || '점수 산정 이유가 제공되지 않았습니다.';
    renderList(els.limitationsList, report.summary.limitations);
    renderMeta(report);
    renderFrames(report);
    renderList(els.globalCaveats, report.caveats);
}

function renderMeta(report) {
    const items = [
        ['파일명', report.originalFilename],
        ['업로드 시간', formatDate(report.createdAt)],
        ['처리 상태', '분석 완료'],
        ['판정 점수', toFixed3(report.summary.aiProbability)],
        ['신뢰도', toFixed3(report.summary.confidence)],
        ['판정', toVerdictLabel(report.summary.verdict)],
        ['영상 길이', `${toFixed3(report.video.durationSeconds)}s`],
        ['분석 프레임 수', String(report.frames.length)],
        ['모델', report.model],
        ['detail', report.imageDetail],
        ['해상도', `${report.video.width} × ${report.video.height}`],
        ['처리 시간', `${report.processingMillis}ms`]
    ];

    els.metaGrid.innerHTML = '';
    items.forEach(([label, value]) => {
        const tile = document.createElement('div');
        tile.className = 'meta-tile';
        tile.innerHTML = `
            <div class="label">${escapeHtml(label)}</div>
            <div class="value">${escapeHtml(value)}</div>
        `;
        els.metaGrid.appendChild(tile);
    });
}

function renderFrames(report) {
    els.framesGrid.innerHTML = '';
    report.frames.forEach(frame => {
        const card = document.createElement('div');
        card.className = 'frame-card';

        const cuesList = toListHtml(frame.cues);
        const counterCuesList = toListHtml(frame.counterCues);
        const caveatsList = toListHtml(frame.caveats);
        const verdict = frame.label === 'suspicious' ? '의심' : frame.label === 'normal' ? '정상 쪽' : '유보';

        card.innerHTML = `
            <div class="frame-head">
                <div>
                    <div class="frame-title">Frame ${frame.index}</div>
                    <div class="frame-time">${toFixed3(frame.timestampSeconds)}s · ${escapeHtml(verdict)} · suspicious score ${toFixed3(frame.suspiciousScore)}</div>
                </div>
                <div class="small-tag">${escapeHtml(frame.shortNote || '')}</div>
            </div>

            <div class="frame-images">
                <div class="frame-image-box">
                    <div class="small-label">원본 프레임</div>
                    <img src="${frame.rawImageUrl}" alt="frame ${frame.index}">
                </div>
                <div class="frame-image-box">
                    <div class="small-label">heatmap overlay</div>
                    <img src="${frame.heatmapImageUrl}" alt="heatmap ${frame.index}">
                </div>
            </div>

            <div class="metric-row">
                <div class="metric-tag">temporalResidual ${toFixed4(frame.metrics.temporalResidual)}</div>
                <div class="metric-tag">edgeJump ${toFixed4(frame.metrics.edgeJump)}</div>
                <div class="metric-tag">sharpnessSwing ${toFixed4(frame.metrics.sharpnessSwing)}</div>
                <div class="metric-tag">colorShift ${toFixed4(frame.metrics.colorShift)}</div>
            </div>

            <div class="frame-evidence-grid">
                <div>
                    <div class="small-label">관찰 영역</div>
                    <p>${escapeHtml(frame.observedArea || '대표 프레임 전체')}</p>
                </div>
                <div>
                    <div class="small-label">수치 해석</div>
                    <p>${escapeHtml(frame.metricInterpretation || '보조 수치는 시각 단서와 함께 참고해야 합니다.')}</p>
                </div>
            </div>

            <div class="small-label">직접 관찰 근거</div>
            <div class="tag-row">${cuesList}</div>
            <div class="small-label">반대 근거 / 대안 설명</div>
            <div class="tag-row">${counterCuesList}</div>
            <div class="small-label">주의사항</div>
            <div class="tag-row">${caveatsList}</div>
        `;

        els.framesGrid.appendChild(card);
    });
}

function renderList(container, items) {
    container.innerHTML = '';
    if (!items || !items.length) {
        const li = document.createElement('li');
        li.textContent = '표시할 항목이 없습니다.';
        container.appendChild(li);
        return;
    }
    items.forEach(item => {
        const li = document.createElement('li');
        li.textContent = item;
        container.appendChild(li);
    });
}

function toListHtml(items) {
    if (!items || !items.length) {
        return '<span class="small-tag">근거 없음</span>';
    }
    return items.map(item => `<span class="small-tag">${escapeHtml(item)}</span>`).join('');
}

function setBusy(busy, message) {
    els.submitButton.disabled = busy;
    els.submitButton.textContent = busy ? '분석 중...' : '분석 시작';
    els.jobState.textContent = message;
}

function hideReport() {
    els.reportSection.classList.add('hidden');
    els.framesSection.classList.add('hidden');
    els.directionVerdict.textContent = '판정 유보';
    els.directionSummary.textContent = '대표 프레임 기반의 GPT 판별 결과가 여기 표시됩니다. 먼저 영상을 업로드해 분석을 시작하세요.';
}

async function readError(response) {
    try {
        const data = await response.json();
        return data.detail || data.message || JSON.stringify(data);
    } catch (_) {
        return await response.text();
    }
}

function verdictHeadline(verdict) {
    switch (verdict) {
        case 'AI_SUSPECTED':
            return 'AI 생성/합성 의심이 비교적 높게 나타났습니다.';
        case 'LIKELY_REAL':
            return '실제 촬영 영상일 가능성이 더 높아 보입니다.';
        default:
            return '결론을 단정하기보다 추가 확인이 필요한 상태입니다.';
    }
}

function toVerdictLabel(verdict) {
    switch (verdict) {
        case 'AI_SUSPECTED':
            return 'AI 의심';
        case 'LIKELY_REAL':
            return '실사 가능성 높음';
        default:
            return '판정 유보';
    }
}

function formatDate(value) {
    try {
        return new Intl.DateTimeFormat('ko-KR', {
            dateStyle: 'medium',
            timeStyle: 'medium'
        }).format(new Date(value));
    } catch (_) {
        return value;
    }
}

function escapeHtml(value) {
    return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}

function toFixed3(value) {
    const num = Number(value ?? 0);
    return Number.isFinite(num) ? num.toFixed(3) : '-';
}

function toFixed4(value) {
    const num = Number(value ?? 0);
    return Number.isFinite(num) ? num.toFixed(4) : '-';
}
