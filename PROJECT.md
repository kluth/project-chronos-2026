# Project: Chronos Phase 2 Features

## Architecture
Chronos is a life-centric edge tracker. It consists of:
- `chromeos-extension`: Manifest V3 extension tracking active tabs and idle status, pushing to edge daemon.
- `edge-daemon`: C++ POSIX socket server receiving data from extension, applying Laplace Epsilon-Differential Privacy, uploading to Cloud Functions.
- `backend-functions`: Firebase Cloud Functions handling CRDT conflict resolution (private life anchors win over work tasks) and telemetry ingestion.
- `frontend-angular`: Standalone Angular app rendering schedules via HTML5 Canvas.

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Baseline & Feature Roadmap | Establish the 50-feature roadmap | None | DONE |
| 2 | Edge Tracker enhancements | Implement 16 Edge Daemon & Extension features (F35-F50) | M1 | PLANNED |
| 3 | Backend CRDT extensions | Implement 17 Firebase CRDT Backend features (F18-F34) | M2 | PLANNED |
| 4 | Dashboard Visualization | Implement 17 Angular Dashboard features (F1-F17) | M3 | PLANNED |
| 5 | Validation & Integrity Audit | Final build, verification, and audit | M4 | PLANNED |

## Code Layout
- `/chromeos-extension/`: Browser extension files (`background.js`, `manifest.json`).
- `/edge-daemon/`: C++ daemon code (`src/main.cpp`, `src/differential_privacy.cpp`, `src/differential_privacy.h`).
- `/backend-functions/`: Cloud functions typescript code (`src/index.ts`, `src/crdt.ts`, `tests/`).
- `/frontend-angular/`: Angular dashboard code (`src/app/`).

## Interface Contracts
- Chrome extension posts to `http://localhost:8888/ingest` with active tab and idle state.
- Edge daemon posts to Firebase Function endpoint `ingestTelemetry`.
- Firestore structure includes `users/{userId}/private_anchors` and `users/{userId}/work_tasks`.
