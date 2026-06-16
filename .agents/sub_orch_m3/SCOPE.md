# Scope: Milestone 3 (Backend CRDT extensions)

## Architecture
Milestone 3 extends the Firebase Cloud Functions backend in `/backend-functions/src/` to support conflict resolution, dynamic scheduling policies, causal consistency via vector clocks, audit logs, authentication, analytics, third-party integrations (Jira, Slack, Webhooks), geofencing, rate limiting, and recurring events.

The entry point is `/backend-functions/src/index.ts`, and core CRDT logic lives in `/backend-functions/src/crdt.ts`.

## Milestones
| # | Name | Features | Dependencies | Status |
|---|---|---|---|---|
| 1 | Core CRDT & Policy Extensions | F18, F19, F31 | None | PLANNED |
| 2 | Analytics, Audit Logging & Recovery | F20, F28, F29 | Milestone 1 | PLANNED |
| 3 | Ingestion, Auth & Privacy Validation | F24, F27, F33 | Milestone 1 | PLANNED |
| 4 | Webhooks, Slack & Jira Integrations | F22, F25, F30 | Milestone 2 | PLANNED |
| 5 | Presence, Recurring Anchors, Geofencing & Bulk Import | F21, F23, F26, F32, F34 | Milestone 3, 4 | PLANNED |

## Interface Contracts
### Vector Clock (Feature 18)
- Every `PrivateLifeAnchor` and `WorkTask` interface extends with `vectorClock?: Record<string, number>`.
- Resolution compares vector clocks when updating the same block.

### Dynamic Scheduling Policy Engine (Feature 19)
- Policy schema fetched from `users/{userId}/policy/current`.
- Config format: `{ rules: Array<{ startHour: number, endHour: number, priority: 'WORK' | 'PRIVATE' }> }`.
- Default behavior: Private wins.

### Replication Audit Logging System (Feature 20)
- Writes to `users/{userId}/audit_logs/{logId}` on any preemption or conflict resolution.
- Format: `{ type: 'PREEMPTION' | 'RESOLVE', taskId: string, anchorId?: string, timestamp: any }`.

### Outbound Webhooks (Feature 22)
- Dispatches POST requests to URLs configured in `users/{userId}/webhooks`.

### Slack Alerts (Feature 25)
- Dispatches POST request to configured Slack Webhook URL in `users/{userId}/config`.

### Jira Status Syncer (Feature 30)
- Transitions Jira ticket status via Jira REST APIs if configured.

### Strict Temporal Bounds (Feature 31)
- Validation in onCreate/onUpdate triggers: `start < end && start > 0 && end > 0`. Throws error if invalid.

### Geofenced Context (Feature 32)
- Check user location coordinates in anchor/task to allow preemption only if outside work geofence bounds.
