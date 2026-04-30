# VeriVid Desk

Spring Boot + GPT Vision 기반의 **영상 AI 생성 판별 데스크**입니다.

기능:
- 영상 업로드
- 프레임 자동 샘플링
- 인접 프레임 차이 heatmap 생성
- GPT Responses API로 프레임 분석
- 판정 / 근거 / 주의사항 / 프레임별 코멘트 출력
- 최근 리포트 저장 / 조회 / 삭제

## 1) 빠른 실행

### Docker 사용
```bash
cp .env.example .env
# .env에 OPENAI_API_KEY 입력
docker compose up --build
```

브라우저:
```text
http://localhost:8080
```

### 로컬 Maven 실행
Java 21, Maven 3.9+ 필요
```bash
export OPENAI_API_KEY=sk-...
mvn spring-boot:run
```

## 2) 비용 절약 기본값

기본값은 다음처럼 잡아두었습니다.
- 프레임 수: `3`
- 이미지 detail: `low`
- 기본 모델: `gpt-5.4-mini`

UI에서 아래 항목을 조절할 수 있습니다.
- GPT에 넘길 프레임 수
- image detail (`low`, `auto`, `high`)
- heatmap도 같이 GPT에 보낼지 여부
- 모델 override

## 3) API

### 분석
`POST /api/analyze`

multipart form-data:
- `file`: 업로드 영상
- `frameCount`: 정수, 기본 3
- `imageDetail`: `low|auto|high`, 기본 low
- `includeHeatmapsInPrompt`: `true|false`, 기본 false
- `model`: optional, 모델 override

### 최근 리포트
- `GET /api/reports`
- `GET /api/reports/{id}`
- `DELETE /api/reports/{id}`
- `DELETE /api/reports`

### 서버 기본 설정
- `GET /api/config`

## 4) 저장 구조

실행 후 `storage/reports/{reportId}` 아래에 저장됩니다.

예시:
```text
storage/
  reports/
    rpt-20260403-101500-a1b2c3/
      report.json
      prompt.txt
      openai-request.json
      openai-response.json
      input/
        original.mp4
      raw/
        frame-01.jpg
      heatmap/
        frame-01-heatmap.jpg
```

## 5) 판정 철학

이 앱은 **법적 증거용 감정 시스템**이 아니라,
대표 프레임들을 기반으로 한 **설명 가능한 리스크 평가 도구**입니다.

즉:
- `AI_SUSPECTED`: AI 생성 의심
- `LIKELY_REAL`: 실제 촬영 가능성 높음
- `INCONCLUSIVE`: 판정 유보

발표용 설명은 [`docs/gpt-video-analysis-explanation.md`](docs/gpt-video-analysis-explanation.md)에 정리되어 있습니다.

## 6) 팁

- 비용이 신경 쓰이면: 프레임 3장 + detail low
- 인물 영상이면: heatmap은 기본적으로 GPT에 보내지 않는 편이 저렴
- 애매한 결과가 많다면: 프레임 5장 / detail auto로 올려보세요

## 7) 주의

- beauty filter, denoise, 과도한 색보정, 저조도 촬영은 AI처럼 보일 수 있습니다.
- 일부 최신 AI 영상은 몇 장의 대표 프레임만으로는 구분이 어렵습니다.
- heatmap은 **프레임 간 변화량 시각화**이지, 그 자체가 AI 증거는 아닙니다.
