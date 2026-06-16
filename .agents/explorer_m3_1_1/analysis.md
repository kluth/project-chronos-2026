# Sub-Milestone 1 Strategy Report: Core CRDT & Policy Extensions

## Executive Summary
This report proposes the design and implementation strategy for extending Project Chronos's conflict-free backend. We introduce logical vector clocks for causal conflict resolution, implement a dynamic JSON-based scheduling policy engine, and add strict temporal bounds validation on Firestore write triggers.

---

## 1. Feature 18: Vector Clock Synchronization Schema

### Interface Extensions (`src/crdt.ts`)
We will expand the base interface `TimeBlock` to support an optional logical `vectorClock`, mapping replica/actor IDs to logical sequence numbers. Because `PrivateLifeAnchor` and `WorkTask` inherit from `TimeBlock`, they automatically gain this field:

```typescript
export type VectorClock = Record<string, number>;

export interface TimeBlock {
    id: string;
    start: number; // Unix timestamp in ms
    end: number;
    vectorClock?: VectorClock; // Tracks causal history across devices
}

export interface PrivateLifeAnchor extends TimeBlock {
    type: 'PRIVATE';
    description: string;
}

export interface WorkTask extends TimeBlock {
    type: 'WORK';
    jiraId?: string;
}
```

### Clock Comparison & Causal Resolution Algorithm
We define a standard partial order comparison for vector clocks:
1. $V_1 < V_2$ if for all replicas $i$, $V_1[i] \le V_2[i]$ and there exists $j$ where $V_1[j] < V_2[j]$ (missing keys default to 0).
2. If neither clock dominates, they are concurrent, indicating a causal conflict.

We resolve causal conflicts deterministically using the lexical representation of the document object as a tie-breaker.

```typescript
export enum ClockComparison {
    LESS = 'LESS',
    GREATER = 'GREATER',
    EQUAL = 'EQUAL',
    CONCURRENT = 'CONCURRENT'
}

export function compareVectorClocks(vc1?: VectorClock, vc2?: VectorClock): ClockComparison {
    if (!vc1 && !vc2) return ClockComparison.EQUAL;
    if (!vc1) return ClockComparison.LESS;
    if (!vc2) return ClockComparison.GREATER;

    let hasGreater = false;
    let hasLess = false;

    const allKeys = new Set([...Object.keys(vc1), ...Object.keys(vc2)]);
    for (const key of allKeys) {
        const val1 = vc1[key] ?? 0;
        const val2 = vc2[key] ?? 0;
        if (val1 > val2) {
            hasGreater = true;
        } else if (val1 < val2) {
            hasLess = true;
        }
    }

    if (hasGreater && hasLess) return ClockComparison.CONCURRENT;
    if (hasGreater) return ClockComparison.GREATER;
    if (hasLess) return ClockComparison.LESS;
    return ClockComparison.EQUAL;
}

/**
 * Deduplicates and resolves concurrent versions of the same TimeBlock ID.
 */
export function resolveCausalConflicts<T extends TimeBlock>(items: T[]): T[] {
    const grouped = new Map<string, T[]>();
    for (const item of items) {
        const list = grouped.get(item.id) || [];
        list.push(item);
        grouped.set(item.id, list);
    }

    const resolved: T[] = [];
    for (const [_, versions] of grouped.entries()) {
        if (versions.length === 1) {
            resolved.push(versions[0]);
            continue;
        }

        let best = versions[0];
        for (let i = 1; i < versions.length; i++) {
            const next = versions[i];
            const cmp = compareVectorClocks(best.vectorClock, next.vectorClock);
            if (cmp === ClockComparison.LESS) {
                best = next;
            } else if (cmp === ClockComparison.CONCURRENT) {
                // Deterministic value-based tie-breaker
                if (JSON.stringify(next) > JSON.stringify(best)) {
                    best = next;
                }
            }
        }
        resolved.push(best);
    }
    return resolved;
}
```

---

## 2. Feature 19: Dynamic Scheduling Policy Engine

Instead of hardcoding "PrivateLifeAnchor strictly wins", conflict resolution will parse a user's JSON policy loaded from `users/{userId}/policy/current`.

### Schema Specification
```typescript
export interface TimeRangeRule {
    startHour: number;     // Hour of day (0-23)
    endHour: number;       // Hour of day (0-23). Supports overnight ranges (e.g., 22 to 6)
    daysOfWeek?: number[]; // Days: 0 (Sun) to 6 (Sat)
    timezone?: string;     // Timezone name (e.g. 'Europe/Berlin')
}

export interface PolicyRule {
    timeRange?: TimeRangeRule;
    winner: 'PRIVATE' | 'WORK';
}

export interface SchedulingPolicy {
    rules: PolicyRule[];
    defaultWinner: 'PRIVATE' | 'WORK';
}
```

### Policy Evaluation Logic
When an anchor and task overlap, the policy engine determines the winner based on the midpoint of the overlap:

```typescript
export function evaluateConflictWinner(
    anchor: PrivateLifeAnchor,
    task: WorkTask,
    policy: SchedulingPolicy
): 'PRIVATE' | 'WORK' {
    const overlapStart = Math.max(anchor.start, task.start);
    const overlapEnd = Math.min(anchor.end, task.end);
    const midpoint = (overlapStart + overlapEnd) / 2;
    const date = new Date(midpoint);

    for (const rule of policy.rules) {
        if (rule.timeRange && isTimeInRule(date, rule.timeRange)) {
            return rule.winner;
        }
    }
    return policy.defaultWinner;
}

function isTimeInRule(date: Date, range: TimeRangeRule): boolean {
    let hour = date.getUTCHours();
    let day = date.getUTCDay();

    if (range.timezone) {
        try {
            const tzString = date.toLocaleString('en-US', { timeZone: range.timezone });
            const tzDate = new Date(tzString);
            hour = tzDate.getHours();
            day = tzDate.getDay();
        } catch (e) {
            // Fallback to UTC
            hour = date.getUTCHours();
            day = date.getUTCDay();
        }
    }

    let hourMatches = false;
    if (range.startHour <= range.endHour) {
        hourMatches = hour >= range.startHour && hour < range.endHour;
    } else {
        // Overnight span (e.g. 22:00 to 06:00)
        hourMatches = hour >= range.startHour || hour < range.endHour;
    }

    if (!hourMatches) return false;

    if (range.daysOfWeek && range.daysOfWeek.length > 0) {
        return range.daysOfWeek.includes(day);
    }

    return true;
}
```

---

## 3. Feature 31: Strict Temporal Bounds Validation

We must enforce that for any created or updated `TimeBlock`:
1. `start < end`
2. `start > 0` and `end > 0` (bounds are positive numbers representing Unix time).

### Validation Helper (`src/crdt.ts` or `src/index.ts`)
```typescript
export function validateTimeBlock(block: any): boolean {
    if (!block) return false;
    const start = Number(block.start);
    const end = Number(block.end);

    if (isNaN(start) || isNaN(end)) return false;
    if (start <= 0 || end <= 0) return false;
    if (start >= end) return false;

    return true;
}
```

---

## 4. Trigger Integration Design (`src/index.ts`)

We transition the triggers from `onCreate` to `onWrite` (or separate `onCreate`/`onUpdate`) to monitor both creations and updates. When validation fails, the trigger immediately deletes the invalid document.

### Modified Triggers
```typescript
import * as functions from 'firebase-functions';
import * as admin from 'firebase-admin';
import { 
    resolveSchedule, 
    validateTimeBlock, 
    PrivateLifeAnchor, 
    WorkTask, 
    SchedulingPolicy 
} from './crdt';

admin.initializeApp();
const db = admin.firestore();

export const onWorkTaskWrite = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onWrite(async (change, context) => {
        const userId = context.params.userId;

        // Skip delete events
        if (!change.after.exists) return;

        const taskData = change.after.data();

        // 1. Feature 31 Validation
        if (!validateTimeBlock(taskData)) {
            console.warn(`[Chronos AI] WorkTask ${context.params.taskId} failed temporal validation. Deleting.`);
            await change.after.ref.delete();
            return;
        }

        const newTask = taskData as WorkTask;

        // Fetch inputs
        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);

        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? (policySnap.data() as SchedulingPolicy) : undefined;

        // 2. Resolve Schedule
        const resolution = resolveSchedule(anchors, tasks, policy);

        // 3. Preemption logic
        const isTaskValid = resolution.validTasks.some(t => t.id === newTask.id);
        if (!isTaskValid) {
            console.log(`[Chronos AI] WorkTask ${newTask.id} violates policy/anchors. Deleting.`);
            await change.after.ref.delete();
        }
    });

export const onPrivateAnchorWrite = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onWrite(async (change, context) => {
        const userId = context.params.userId;

        if (!change.after.exists) return;

        const anchorData = change.after.data();

        // 1. Feature 31 Validation
        if (!validateTimeBlock(anchorData)) {
            console.warn(`[Chronos AI] PrivateAnchor ${context.params.anchorId} failed temporal validation. Deleting.`);
            await change.after.ref.delete();
            return;
        }

        const newAnchor = anchorData as PrivateLifeAnchor;

        // Fetch inputs
        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);

        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? (policySnap.data() as SchedulingPolicy) : undefined;

        // 2. Resolve Schedule
        const resolution = resolveSchedule(anchors, tasks, policy);

        // 3. Apply Preemptions
        const validTaskIds = new Set(resolution.validTasks.map(t => t.id));
        const batch = db.batch();
        let deletedCount = 0;

        tasksSnap.docs.forEach(doc => {
            if (!validTaskIds.has(doc.data().id)) {
                batch.delete(doc.ref);
                deletedCount++;
            }
        });

        if (deletedCount > 0) {
            await batch.commit();
        }

        // Also check if the new anchor itself was preempted
        const isAnchorValid = resolution.validAnchors.some(a => a.id === newAnchor.id);
        if (!isAnchorValid) {
            console.log(`[Chronos AI] PrivateAnchor ${newAnchor.id} preempted by policy. Deleting.`);
            await change.after.ref.delete();
        }
    });
```
