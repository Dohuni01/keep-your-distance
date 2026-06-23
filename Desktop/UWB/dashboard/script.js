/* ═══════════════════════════════════════════════════════════════
   3m Safety Monitoring System — Dashboard Script

   구조:
     1. CONFIG        설정 상수
     2. STATE         애플리케이션 전역 상태
     3. MOCK_DATA     Mock 데이터 초기화 및 업데이트 시뮬레이션
     4. DATA_SOURCE   WebSocket 연결 추상화 레이어 (실제 연결 포인트)
     5. MAP           Leaflet 지도 관리
     6. UI            DOM 업데이트 함수
     7. EVENTS        이벤트 로그 관리
     8. TOAST         알림 시스템
     9. CLOCK         헤더 시계
    10. INIT          초기화 진입점
═══════════════════════════════════════════════════════════════ */

'use strict';

/* ════════════════════════════════════════
   1. CONFIG
════════════════════════════════════════ */
const CONFIG = {
  DANGER_DIST:       3.0,   // m — 위험 진입 임계값
  EXIT_DIST:         3.2,   // m — 이탈 히스테리시스 임계값
  UPDATE_INTERVAL:   5000,  // ms — Mock 데이터 갱신 주기
  MAX_LOG_ENTRIES:   100,   // 이벤트 로그 최대 보관 수
  MAP_CENTER:        [37.5172, 127.0473], // 기본 지도 중심 (서울 강남)
  MAP_ZOOM:          18,

  /* ── WebSocket 설정 (Phase 3 연결 시 여기만 수정) ──
   * WS_URL: 'ws://your-server.com:3000'
   * USE_MOCK: false 로 변경하면 WebSocket 모드로 전환
   */
  USE_MOCK:  true,
  WS_URL:    'ws://localhost:3000',
};

/* ════════════════════════════════════════
   2. STATE
════════════════════════════════════════ */
const state = {
  anchors:      [],   // AnchorObject[]
  tags:         [],   // TagObject[]
  events:       [],   // EventObject[]
  todayEvents:  0,

  connection: {
    server: false,
    mqtt:   false,
  },

  lastUpdate: null,
};

/* ════════════════════════════════════════
   3. MOCK DATA

   실제 WebSocket 연결 전 시뮬레이션용.
   DataSource.start()에서 호출된다.
════════════════════════════════════════ */

/** Anchor 5개, Tag 20개 (앵커당 4명) 초기화 */
function initMockData() {
  const anchorConfigs = [
    { id: 'A001', equipment: '굴착기 #1', lat: 37.51755, lon: 127.04672 },
    { id: 'A002', equipment: '굴착기 #2', lat: 37.51757, lon: 127.04788 },
    { id: 'A003', equipment: '크레인 #1',  lat: 37.51685, lon: 127.04789 },
    { id: 'A004', equipment: '굴착기 #3', lat: 37.51683, lon: 127.04672 },
    { id: 'A005', equipment: '크레인 #2',  lat: 37.51720, lon: 127.04730 },
  ];

  state.anchors = anchorConfigs.map(cfg => ({
    id:          cfg.id,
    equipment:   cfg.equipment,
    lat:         cfg.lat,
    lon:         cfg.lon,
    status:      'online',
    gpsConnected: true,
    tagIds:      [], // 아래에서 채움
  }));

  /* Tag 20개 생성 — 앵커당 4개씩 배정 */
  let tagNum = 1;
  state.anchors.forEach(anchor => {
    for (let i = 0; i < 4; i++) {
      const id = `T${String(tagNum).padStart(3, '0')}`;

      /* 이동 패턴: 사인파 기반 자연스러운 거리 변화 */
      const baseDist  = 3.5 + Math.random() * 7;   // 3.5 ~ 10.5m
      const amplitude = 1.0 + Math.random() * 3.0; // 1 ~ 4m 진폭

      state.tags.push({
        id,
        anchorId:   anchor.id,
        distance:   baseDist,
        filtered:   baseDist,
        inDanger:   false,
        lastUpdate: Date.now(),

        /* 시뮬레이션 내부 변수 */
        _base:      baseDist,
        _amp:       amplitude,
        _phase:     Math.random() * Math.PI * 2,  // 위상 오프셋
        _period:    25 + Math.random() * 35,       // 주기 25~60s
        _angle:     (i / 4) * 2 * Math.PI,        // 앵커 주변 배치 방향 (0°,90°,180°,270°)
      });

      anchor.tagIds.push(id);
      tagNum++;
    }
  });

  /* 시작 시 2개 태그를 즉시 위험 상태로 설정 (대시보드 초기 시각화용) */
  state.tags[0]._phase = Math.PI;        // 진폭 최대 (가장 가까운 위치)
  state.tags[0]._base  = 1.8;
  state.tags[7]._phase = Math.PI * 0.9;
  state.tags[7]._base  = 2.0;

  state.connection.server = true;
  state.connection.mqtt   = true;
}

/** 5초마다 호출 — 거리값을 사인파로 갱신하고 위험 판단 */
function updateMockData() {
  const now = Date.now();

  state.tags.forEach(tag => {
    const prevDanger = tag.inDanger;

    /* 사인파 기반 거리 갱신 */
    tag._phase += (2 * Math.PI * CONFIG.UPDATE_INTERVAL) / (tag._period * 1000);
    const raw = tag._base + tag._amp * Math.sin(tag._phase);
    tag.distance = Math.max(0.3, parseFloat(raw.toFixed(2)));
    tag.filtered = tag.distance; // Mock에서는 raw = filtered
    tag.lastUpdate = now;

    /* 위험 판단 (히스테리시스 적용) */
    if (!tag.inDanger && tag.distance <= CONFIG.DANGER_DIST) {
      tag.inDanger = true;
      if (prevDanger !== tag.inDanger) {
        addEvent('enter', tag);
        showToast(
          `위험 감지`,
          `${tag.id} → ${tag.anchorId}  (${tag.distance.toFixed(2)}m)`,
          'danger'
        );
      }
    } else if (tag.inDanger && tag.distance > CONFIG.EXIT_DIST) {
      tag.inDanger = false;
      if (prevDanger !== tag.inDanger) {
        addEvent('exit', tag);
        showToast(
          `안전 구역 복귀`,
          `${tag.id} → ${tag.anchorId}`,
          'success'
        );
      }
    }
  });

  state.lastUpdate = now;
  renderAll();
}

/* ════════════════════════════════════════
   4. DATA SOURCE

   WebSocket 연결 추상화 레이어.
   USE_MOCK = true  → Mock 시뮬레이션
   USE_MOCK = false → WebSocket 모드

   Phase 3 서버 연결 시 connectWebSocket() 함수만 구현하면 됨.
   외부에서 받은 데이터는 applyServerData(data)로 state에 반영.
════════════════════════════════════════ */
const DataSource = {
  _mockTimer: null,
  _ws:        null,

  start() {
    if (CONFIG.USE_MOCK) {
      this._startMock();
    } else {
      this._connectWebSocket();
    }
  },

  stop() {
    if (this._mockTimer) clearInterval(this._mockTimer);
    if (this._ws)        this._ws.close();
  },

  /* ── Mock 모드 ── */
  _startMock() {
    initMockData();

    /* 첫 렌더는 즉시 실행 */
    updateMockData();

    /* 이후 5초 주기로 갱신 */
    this._mockTimer = setInterval(() => {
      updateMockData();
      updateLastUpdateText();
    }, CONFIG.UPDATE_INTERVAL);

    console.info('[DataSource] Mock 모드 시작');
  },

  /* ── WebSocket 모드 (Phase 3 구현 예정) ──
   *
   * 서버에서 오는 메시지 포맷 예시:
   * {
   *   type: 'range',
   *   anchorId: 'A001',
   *   tagId: 'T001',
   *   distRaw: 2.85,
   *   distFilt: 2.90,
   *   danger: true,
   *   ts: 1234567890
   * }
   * {
   *   type: 'relay',
   *   anchorId: 'A001',
   *   state: true,
   *   ts: 1234567890
   * }
   * {
   *   type: 'status',
   *   anchorId: 'A001',
   *   online: true,
   *   uptime: 3600,
   *   ts: 1234567890
   * }
   */
  _connectWebSocket() {
    console.info('[DataSource] WebSocket 연결 시도:', CONFIG.WS_URL);

    this._ws = new WebSocket(CONFIG.WS_URL);

    this._ws.onopen = () => {
      console.info('[DataSource] WebSocket 연결됨');
      state.connection.server = true;
      state.connection.mqtt   = true;
      renderConnectionStatus();
      showToast('서버 연결됨', CONFIG.WS_URL, 'info');
    };

    this._ws.onmessage = (e) => {
      try {
        const data = JSON.parse(e.data);
        this._applyServerData(data);
      } catch (err) {
        console.error('[DataSource] 메시지 파싱 실패:', err);
      }
    };

    this._ws.onclose = () => {
      console.warn('[DataSource] WebSocket 끊김 — 5초 후 재연결');
      state.connection.server = false;
      state.connection.mqtt   = false;
      renderConnectionStatus();
      setTimeout(() => this._connectWebSocket(), 5000);
    };

    this._ws.onerror = (err) => {
      console.error('[DataSource] WebSocket 오류:', err);
    };
  },

  /** 서버 데이터를 state에 반영 후 렌더 */
  _applyServerData(data) {
    if (data.type === 'range') {
      const tag = state.tags.find(t => t.id === data.tagId);
      if (!tag) return;

      const prevDanger = tag.inDanger;
      tag.distance  = data.distRaw;
      tag.filtered  = data.distFilt;
      tag.inDanger  = data.danger;
      tag.lastUpdate = data.ts;

      if (!prevDanger && tag.inDanger) {
        addEvent('enter', tag);
        showToast('위험 감지', `${tag.id} → ${tag.anchorId}`, 'danger');
      } else if (prevDanger && !tag.inDanger) {
        addEvent('exit', tag);
        showToast('안전 구역 복귀', `${tag.id} → ${tag.anchorId}`, 'success');
      }
    }

    if (data.type === 'status') {
      const anchor = state.anchors.find(a => a.id === data.anchorId);
      if (anchor) anchor.status = data.online ? 'online' : 'offline';
    }

    state.lastUpdate = Date.now();
    renderAll();
    updateLastUpdateText();
  },
};

/* ════════════════════════════════════════
   5. MAP
════════════════════════════════════════ */
let map = null;
const mapMarkers    = {}; // anchorId → L.Marker
const mapCircles    = {}; // anchorId → L.Circle
const mapTagMarkers = {}; // tagId    → L.CircleMarker

/* 위도 37.5° 기준: 1m 당 위경도 변환 계수 */
const LAT_PER_M = 0.000009;    // 1m = 0.000009°위도
const LON_PER_M = 0.0000114;   // 1m = 0.0000114°경도 (cos(37.5°) 보정)

function initMap() {
  map = L.map('map', {
    center: CONFIG.MAP_CENTER,
    zoom:   CONFIG.MAP_ZOOM,
    zoomControl: true,
  });

  /* CartoDB Dark Matter 타일 — 다크 테마에 최적 */
  L.tileLayer(
    'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    {
      attribution: '© OpenStreetMap contributors © CARTO',
      subdomains:  'abcd',
      maxZoom:     20,
    }
  ).addTo(map);

  /* Leaflet 기본 attribution 위치 조정 */
  map.attributionControl.setPosition('bottomleft');
}

/** 태그의 지도 위치 계산 — 앵커 기준 실제 거리·방향 */
function getTagMapPosition(tag) {
  const anchor = getAnchor(tag.anchorId);
  if (!anchor) return null;
  const d = Math.min(tag.distance, 20); // 지도 표시용 20m 상한
  return [
    anchor.lat + d * LAT_PER_M * Math.sin(tag._angle),
    anchor.lon + d * LON_PER_M * Math.cos(tag._angle),
  ];
}

/** anchorId 기준으로 마커와 안전구역 원을 생성/갱신 */
function updateMapMarkers() {
  if (!map) return;

  state.anchors.forEach(anchor => {
    const hasDanger = getAnchorDangerCount(anchor.id) > 0;

    /* 마커 생성 또는 아이콘 갱신 */
    if (!mapMarkers[anchor.id]) {
      /* 최초 생성 */
      const marker = L.marker([anchor.lat, anchor.lon], {
        icon: buildMarkerIcon(anchor.id, hasDanger),
      }).addTo(map);

      marker.bindPopup(() => buildPopupHTML(anchor.id), {
        minWidth: 220,
        className: 'dark-popup',
      });

      mapMarkers[anchor.id] = marker;

      /* 3m 안전 구역 원 */
      const circle = L.circle([anchor.lat, anchor.lon], {
        radius:       3,
        color:        hasDanger ? '#f85149' : '#58a6ff',
        fillColor:    hasDanger ? '#f85149' : '#58a6ff',
        fillOpacity:  0.15,
        weight:       1.5,
        dashArray:    hasDanger ? null : '4 4',
      }).addTo(map);

      mapCircles[anchor.id] = circle;

    } else {
      /* 상태 변경 시 아이콘 갱신 */
      mapMarkers[anchor.id].setIcon(buildMarkerIcon(anchor.id, hasDanger));

      mapCircles[anchor.id].setStyle({
        color:       hasDanger ? '#f85149' : '#58a6ff',
        fillColor:   hasDanger ? '#f85149' : '#58a6ff',
        fillOpacity: hasDanger ? 0.25 : 0.12,
        dashArray:   hasDanger ? null : '4 4',
      });

      /* 열린 팝업 갱신 */
      if (mapMarkers[anchor.id].isPopupOpen()) {
        mapMarkers[anchor.id].setPopupContent(buildPopupHTML(anchor.id));
      }
    }
  });

  /* ── 태그 원형 마커 ── */
  state.tags.forEach(tag => {
    const pos = getTagMapPosition(tag);
    if (!pos) return;

    const color = tag.inDanger ? '#f85149' : '#3fb950';
    const tipContent = `<b>${tag.id}</b><br>${tag.anchorId} &nbsp;·&nbsp; ${tag.distance.toFixed(2)}m`;

    if (!mapTagMarkers[tag.id]) {
      const marker = L.circleMarker(pos, {
        radius:      6,
        color,
        fillColor:   color,
        fillOpacity: 0.9,
        weight:      2,
        pane:        'markerPane', // 앵커 마커보다 위에 표시
      }).addTo(map);

      marker.bindTooltip(tipContent, {
        direction:  'top',
        offset:     [0, -8],
        className:  'tag-tooltip',
      });

      mapTagMarkers[tag.id] = marker;
    } else {
      mapTagMarkers[tag.id].setLatLng(pos);
      mapTagMarkers[tag.id].setStyle({ color, fillColor: color });
      mapTagMarkers[tag.id].setTooltipContent(tipContent);
    }
  });
}

/** 커스텀 divIcon 생성 */
function buildMarkerIcon(anchorId, hasDanger) {
  const cls = hasDanger ? 'danger' : 'normal';
  const pulse = hasDanger
    ? '<div class="danger-pulse-ring"></div>'
    : '';

  return L.divIcon({
    className: '',
    html: `
      <div class="map-marker-wrap">
        ${pulse}
        <div class="map-marker-body ${cls}">
          <i class="fas fa-tower-broadcast"></i>
        </div>
        <div class="map-marker-label">${anchorId}</div>
      </div>
    `,
    iconSize:    [70, 56],
    iconAnchor:  [35, 46],
    popupAnchor: [0, -48],
  });
}

/** 팝업 HTML 빌드 */
function buildPopupHTML(anchorId) {
  const anchor      = getAnchor(anchorId);
  if (!anchor) return '';
  const tags        = getAnchorTags(anchorId);
  const dangerCount = tags.filter(t => t.inDanger).length;
  const cls         = dangerCount > 0 ? 'danger' : 'safe';

  return `
    <div class="popup-inner">
      <div class="popup-header">
        <span class="popup-anchor-id">${anchor.id}</span>
        <span class="popup-status-pill ${anchor.status}">${anchor.status === 'online' ? 'Online' : 'Offline'}</span>
      </div>
      <div class="popup-equipment">${anchor.equipment}</div>
      <hr class="popup-divider">
      <div class="popup-row">
        <span class="popup-label">GPS</span>
        <span class="popup-val">${anchor.lat.toFixed(5)}, ${anchor.lon.toFixed(5)}</span>
      </div>
      <div class="popup-row">
        <span class="popup-label">Workers Nearby</span>
        <span class="popup-val">${tags.length}</span>
      </div>
      <div class="popup-row">
        <span class="popup-label">In Danger</span>
        <span class="popup-val ${cls}">${dangerCount > 0 ? '⚠ ' + dangerCount : '✓ None'}</span>
      </div>
      ${tags.map(t => `
        <div class="popup-row">
          <span class="popup-label">${t.id}</span>
          <span class="popup-val ${t.inDanger ? 'danger' : ''}">${t.distance.toFixed(2)}m</span>
        </div>
      `).join('')}
    </div>
  `;
}

/* ════════════════════════════════════════
   6. UI
════════════════════════════════════════ */

/** 모든 UI 컴포넌트를 한 번에 렌더 */
function renderAll() {
  renderConnectionStatus();
  renderAnchorCards();
  renderTagList();
  renderStats();
  renderDangerList();
  renderEventLog();
  updateMapMarkers();
}

/* ── Tag 목록 ── */
function renderTagList() {
  const container = document.getElementById('tag-list');
  const badge     = document.getElementById('tag-count-badge');
  if (!container) return;

  badge.textContent = state.tags.length;

  /* 정렬: 위험 우선 → 거리 가까운 순 */
  const sorted = [...state.tags].sort((a, b) => {
    if (a.inDanger !== b.inDanger) return a.inDanger ? -1 : 1;
    return a.distance - b.distance;
  });

  container.innerHTML = sorted.map(tag => {
    /* 거리 바: 0~10m 기준, 위험=빨강, 5m 미만=주황, 그 외=초록 */
    const barPct   = Math.min(100, (tag.distance / 10) * 100);
    const barColor = tag.inDanger
      ? '#f85149'
      : tag.distance < 5 ? '#d29922' : '#3fb950';

    return `
      <div class="tag-item ${tag.inDanger ? 'danger' : ''}">
        <div class="tag-item-row">
          <span class="tag-id-label">
            <i class="fas fa-person-walking"></i>${tag.id}
          </span>
          <span class="tag-anchor-label">${tag.anchorId}</span>
          <span class="tag-distance-value">${tag.distance.toFixed(1)}m</span>
          <span class="tag-status-pill ${tag.inDanger ? 'danger' : 'safe'}">
            ${tag.inDanger ? '⚠ DANGER' : 'SAFE'}
          </span>
        </div>
        <div class="tag-dist-bar">
          <div class="tag-dist-bar-fill"
               style="width:${barPct}%;background:${barColor}"></div>
        </div>
      </div>
    `;
  }).join('');
}

/* ── 연결 상태 배지 ── */
function renderConnectionStatus() {
  const { server, mqtt } = state.connection;

  const serverDot  = document.getElementById('server-dot');
  const serverText = document.getElementById('server-status-text');
  const mqttDot    = document.getElementById('mqtt-dot');
  const mqttText   = document.getElementById('mqtt-status-text');
  const serverBadge = document.getElementById('server-badge');
  const mqttBadge   = document.getElementById('mqtt-badge');

  if (server) {
    serverDot.className  = 'status-dot online';
    serverText.textContent = 'Online';
    serverBadge.classList.add('connected');
  } else {
    serverDot.className  = 'status-dot offline';
    serverText.textContent = 'Offline';
    serverBadge.classList.remove('connected');
  }

  if (mqtt) {
    mqttDot.className  = 'status-dot online';
    mqttText.textContent = 'Connected';
    mqttBadge.classList.add('connected');
  } else {
    mqttDot.className  = 'status-dot offline';
    mqttText.textContent = 'Disconnected';
    mqttBadge.classList.remove('connected');
  }
}

/* ── Anchor 카드 목록 ── */
function renderAnchorCards() {
  const container = document.getElementById('anchor-list');
  const badge     = document.getElementById('anchor-count-badge');

  badge.textContent = state.anchors.length;

  container.innerHTML = state.anchors.map(anchor => {
    const tags        = getAnchorTags(anchor.id);
    const dangerTags  = tags.filter(t => t.inDanger);
    const hasDanger   = dangerTags.length > 0;

    /* 작업자 점 표시 (최대 8개) */
    const dots = tags.slice(0, 8).map(t =>
      `<div class="worker-dot ${t.inDanger ? 'danger' : ''}" title="${t.id}: ${t.distance.toFixed(2)}m"></div>`
    ).join('');

    return `
      <div class="anchor-card ${hasDanger ? 'danger' : ''}" onclick="focusAnchor('${anchor.id}')">
        <div class="anchor-card-header">
          <div class="anchor-id">
            <i class="fas fa-tower-broadcast"></i>
            ${anchor.id}
          </div>
          <span class="anchor-status-pill ${anchor.status}">${anchor.status === 'online' ? 'Online' : 'Offline'}</span>
        </div>
        <div class="anchor-equipment">${anchor.equipment}</div>
        <div class="anchor-meta">
          <div class="anchor-meta-row">
            <span class="label">GPS</span>
            <span class="value">${anchor.gpsConnected ? anchor.lat.toFixed(4) + ', ' + anchor.lon.toFixed(4) : 'No Signal'}</span>
          </div>
          <div class="anchor-meta-row">
            <span class="label">Workers Nearby</span>
            <span class="value">${tags.length}</span>
          </div>
        </div>
        <div class="anchor-workers">
          <div class="worker-dots">${dots}</div>
          ${hasDanger
            ? `<span class="danger-tag-label">⚠ ${dangerTags.length} DANGER</span>`
            : ''}
        </div>
      </div>
    `;
  }).join('');
}

/* ── 통계 패널 ── */
function renderStats() {
  const dangerCount = state.tags.filter(t => t.inDanger).length;

  document.getElementById('stat-anchors').textContent  = state.anchors.length;
  document.getElementById('stat-tags').textContent     = state.tags.length;
  document.getElementById('stat-dangers').textContent  = dangerCount;
  document.getElementById('stat-events').textContent   = state.todayEvents;

  /* 위험 카드 강조 */
  const dangerCard = document.getElementById('stat-card-danger');
  if (dangerCount > 0) {
    dangerCard.classList.add('active');
  } else {
    dangerCard.classList.remove('active');
  }
}

/* ── 위험 목록 ── */
function renderDangerList() {
  const container = document.getElementById('danger-list');
  const badge     = document.getElementById('danger-count-badge');
  const noMsg     = document.getElementById('no-danger-msg');

  const dangerTags = state.tags
    .filter(t => t.inDanger)
    .sort((a, b) => a.distance - b.distance); // 가까운 순 정렬

  badge.textContent = dangerTags.length;

  if (dangerTags.length === 0) {
    container.innerHTML = '';
    container.appendChild(noMsg || createNoDangerMsg());
    return;
  }

  /* 빈 상태 메시지 숨기기 */
  if (noMsg) noMsg.style.display = 'none';

  /* 위험 태그 카드만 렌더 (noMsg는 DOM에 그대로 둠) */
  const html = dangerTags.map(tag => {
    const elapsed = Math.floor((Date.now() - tag.lastUpdate) / 1000);
    const elapsedStr = elapsed < 60
      ? `${elapsed}s ago`
      : `${Math.floor(elapsed / 60)}m ago`;

    return `
      <div class="danger-item">
        <div class="danger-item-header">
          <span class="danger-label">
            <i class="fas fa-triangle-exclamation"></i>
            DANGER
          </span>
          <span class="danger-distance">
            ${tag.distance.toFixed(2)}<span>m</span>
          </span>
        </div>
        <div class="danger-info">
          Tag <strong>${tag.id}</strong> →
          <strong>${tag.anchorId}</strong>
        </div>
        <div class="danger-time"><i class="fas fa-clock"></i> ${elapsedStr}</div>
      </div>
    `;
  }).join('');

  /* noMsg를 날리지 않고 위험 카드만 새로 쓴다 */
  const existing = container.querySelectorAll('.danger-item');
  existing.forEach(el => el.remove());

  const tempDiv = document.createElement('div');
  tempDiv.innerHTML = html;
  while (tempDiv.firstChild) {
    container.appendChild(tempDiv.firstChild);
  }
}

/* ── 이벤트 로그 ── */
function renderEventLog() {
  const container = document.getElementById('event-log');
  const noMsg     = document.getElementById('no-events-msg');

  if (state.events.length === 0) {
    if (noMsg) noMsg.style.display = '';
    return;
  }
  if (noMsg) noMsg.style.display = 'none';

  container.innerHTML = state.events
    .slice()          // 복사
    .reverse()        // 최신순
    .map(ev => {
      const timeStr = new Date(ev.ts).toLocaleTimeString('ko-KR', { hour12: false });
      const isEnter = ev.type === 'enter';

      return `
        <div class="event-item">
          <div class="event-icon ${ev.type}">
            <i class="fas ${isEnter ? 'fa-arrow-right' : 'fa-arrow-left'}"></i>
          </div>
          <div class="event-body">
            <div class="event-time">${timeStr}</div>
            <div class="event-msg">
              <strong>${ev.tagId}</strong>
              <span class="${ev.type}-keyword">
                ${isEnter ? ' entered danger zone' : ' left danger zone'}
              </span>
            </div>
            <div class="event-meta">${ev.anchorId}  ·  ${ev.distance.toFixed(2)}m</div>
          </div>
        </div>
      `;
    }).join('');
}

/* ── 마지막 업데이트 텍스트 ── */
function updateLastUpdateText() {
  const el      = document.getElementById('last-update-text');
  const icon    = document.getElementById('update-icon');
  const timeStr = new Date().toLocaleTimeString('ko-KR', { hour12: false });

  el.textContent = `Last updated: ${timeStr}`;

  /* 회전 아이콘 잠깐 표시 */
  icon.classList.add('spin');
  setTimeout(() => icon.classList.remove('spin'), 800);
}

/* ── Anchor 카드 클릭 시 지도 이동 ── */
function focusAnchor(anchorId) {
  const anchor = getAnchor(anchorId);
  if (!anchor || !map) return;

  map.setView([anchor.lat, anchor.lon], Math.max(map.getZoom(), 19), {
    animate: true,
    duration: 0.6,
  });

  if (mapMarkers[anchorId]) {
    mapMarkers[anchorId].openPopup();
  }
}

/* ════════════════════════════════════════
   7. EVENTS
════════════════════════════════════════ */
function addEvent(type, tag) {
  state.events.push({
    id:       Date.now() + Math.random(),
    type,                          // 'enter' | 'exit'
    tagId:    tag.id,
    anchorId: tag.anchorId,
    distance: tag.distance,
    ts:       Date.now(),
  });

  /* 최대 보관 수 초과 시 오래된 것 제거 */
  if (state.events.length > CONFIG.MAX_LOG_ENTRIES) {
    state.events.shift();
  }

  state.todayEvents++;
}

/* ════════════════════════════════════════
   8. TOAST 알림
════════════════════════════════════════ */
function showToast(title, msg, type = 'info') {
  const icons = {
    danger:  'fa-triangle-exclamation',
    success: 'fa-shield-check',
    info:    'fa-circle-info',
  };

  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  toast.innerHTML = `
    <i class="fas ${icons[type] || icons.info}"></i>
    <div class="toast-body">
      <div class="toast-title">${title}</div>
      <div class="toast-msg">${msg}</div>
    </div>
  `;

  const container = document.getElementById('toast-container');
  container.appendChild(toast);

  /* 3.5초 후 자동 제거 */
  setTimeout(() => {
    toast.classList.add('removing');
    toast.addEventListener('animationend', () => toast.remove());
  }, 3500);
}

/* ════════════════════════════════════════
   9. CLOCK
════════════════════════════════════════ */
function startClock() {
  function tick() {
    const el = document.getElementById('current-time');
    if (el) {
      el.textContent = new Date().toLocaleTimeString('ko-KR', { hour12: false });
    }
  }
  tick();
  setInterval(tick, 1000);
}

/* ════════════════════════════════════════
   유틸리티
════════════════════════════════════════ */
function getAnchor(id)      { return state.anchors.find(a => a.id === id); }
function getTag(id)         { return state.tags.find(t => t.id === id); }
function getAnchorTags(id)  { return state.tags.filter(t => t.anchorId === id); }
function getAnchorDangerCount(id) {
  return state.tags.filter(t => t.anchorId === id && t.inDanger).length;
}

/* ════════════════════════════════════════
   10. INIT
════════════════════════════════════════ */
function init() {
  console.info('[Init] 3m Safety Monitoring System 시작');

  /* 시계 시작 */
  startClock();

  /* Leaflet 지도 초기화 */
  initMap();

  /* 이벤트 로그 초기화 버튼 */
  document.getElementById('clear-log-btn').addEventListener('click', () => {
    state.events      = [];
    state.todayEvents = 0;
    renderEventLog();
    renderStats();
    showToast('로그 초기화', '이벤트 로그가 비워졌습니다.', 'info');
  });

  /* 데이터 소스 시작 (Mock or WebSocket) */
  DataSource.start();

  console.info(`[Init] 모드: ${CONFIG.USE_MOCK ? 'Mock' : 'WebSocket'}`);
}

/* DOM 준비 완료 후 실행 */
document.addEventListener('DOMContentLoaded', init);
