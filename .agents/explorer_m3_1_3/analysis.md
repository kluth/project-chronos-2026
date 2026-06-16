# Analysis Report: Core CRDT & Policy Extensions (Sub-Milestone 1)

This report outlines the proposed design and implementation strategy for Feature 18, Feature 19, and Feature 31 of Project Chronos. These features extend the baseline CRDT synchronization rules to support vector clocks, user-configured scheduling policies, and strict temporal validation triggers.

---

## 1. Feature 18: Vector Clock Synchronization Schema

### Architectural Design
To support causal consistency across multiple client devices, we expand scheduling entities to track vector clocks (`Record<string, number>`). 
*   **Logical Time Trackers**: Each write actor (e.g. extension, dashboard, local daemon) represents its logical updates with an incremental counter keyed by its device/replica ID.
*   **Entity Schema Expansion**: Since `PrivateLifeAnchor` and `WorkTask` extend `TimeBlock` in `src/crdt.ts`, adding `vectorClock?: Record<string, number>` to `TimeBlock` makes it available for both.
*   **Causal Relationships**: Given clocks $V_1$ and $V_2$:
    *   $V_1$ dominates $V_2$ ($V_1 > V_2$) if all keys in $V_1$ are $\ge V_2$ and at least one is strictly $>$.
    *   $V_1$ and $V_2$ are concurrent ($V_1 \parallel V_2$) if neither dominates the other.
*   **Causal Conflict Resolution**:
    *   *Deduplication*: When multiple versions of the same entity exist (e.g., in a replication log or input array), we deduplicate by ID, choosing the causally newer version.
    *   *Deterministic Tie-Breaker*: For concurrent clocks ($V_1 \parallel V_2$), we merge the clocks by taking the element-wise maximum ($\max(V_1[k], V_2[k])$) and resolve the state using a lexicographical string comparison of their stringified JSON representation as a tie-breaker.
    *   *Trigger Reversion*: If a client writes a causally older update to Firestore, the `onUpdate` trigger detects this and reverts the document to the newer version in the database.

---

## 2. Feature 19: Dynamic Scheduling Policy Engine

### Architectural Design
Instead of hardcoding that `PrivateLifeAnchor` strictly wins, the system will read a `SchedulingPolicy` JSON document from `users/{userId}/policy/current` and resolve conflicts dynamically.

*   **JSON Policy Schema**:
    ```json
    {
      "timezone": "Europe/Berlin",
      "defaultPriority": "PRIVATE",
      "rules": [
        {
          "name": "Work hours priority",
          "days": [1, 2, 3, 4, 5],
          "startHour": 9,
          "startMinute": 0,
          "endHour": 17,
          "endMinute": 0,
          "priority": "WORK"
        }
      ]
    }
    ```
*   **Conflict Evaluation**:
    When a `PrivateLifeAnchor` and a `WorkTask` overlap, they conflict over the interval:
    $$T_{start} = \max(\text{anchor.start}, \text{task.start})$$
    $$T_{end} = \min(\text{anchor.end}, \text{task.end})$$
    The policy engine evaluates the conflict priority at the midpoint of this overlap:
    $$T_{mid} = \frac{T_{start} + T_{end}}{2}$$
*   **Timezone Conversion**: To check if $T_{mid}$ falls into a specific rule window, the engine extracts the local hour, minute, and weekday in the policy's `timezone` using Node's locale-independent `Intl.DateTimeFormat.formatToParts`.
*   **Preemption Symmetry**:
    *   If the resolved priority is `PRIVATE`, the `WorkTask` is preempted and deleted from Firestore.
    *   If the resolved priority is `WORK`, the `PrivateLifeAnchor` is preempted and deleted from Firestore.

---

## 3. Feature 31: Strict Temporal Bounds Validation

### Architectural Design
To guarantee schedule consistency, strict temporal validation is enforced in post-write triggers. If a document fails validation, the write is immediately corrected.

*   **Validation Rules**:
    1.  `start < end`: The block must have a duration strictly greater than 0.
    2.  `start > 0` and `end > 0`: The logical timestamps must be positive numbers.
*   **Trigger Enforcement**:
    *   `onCreate`: If the new document violates validation, delete it from Firestore immediately.
    *   `onUpdate`: If the updated document violates validation, restore the database to the `before` version.

---

## 4. Proposed Code Changes

### A. Modifications in `src/crdt.ts`

```typescript
// Proposed src/crdt.ts
export interface TimeBlock {
    id: string;
    start: number; // Unix timestamp for simplicity
    end: number;
    vectorClock?: Record<string, number>; // Feature 18: Vector Clock support
}

export interface PrivateLifeAnchor extends TimeBlock {
    type: 'PRIVATE';
    description: string;
}

export interface WorkTask extends TimeBlock {
    type: 'WORK';
    jiraId?: string;
}

// Feature 19: Scheduling Policy Interfaces
export interface PolicyRule {
    days?: number[];         // 0 = Sunday, 1 = Monday, etc.
    startHour?: number;      // 0-23
    startMinute?: number;    // 0-59
    endHour?: number;        // 0-23
    endMinute?: number;      // 0-59
    priority: 'PRIVATE' | 'WORK';
}

export interface SchedulingPolicy {
    timezone?: string;
    defaultPriority: 'PRIVATE' | 'WORK';
    rules: PolicyRule[];
}

export function noOverlap(t1: TimeBlock, t2: TimeBlock): boolean {
    return t1.end <= t2.start || t2.end <= t1.start;
}

// Feature 18: Compare two vector clocks
export function compareVectorClocks(
    v1: Record<string, number> | undefined,
    v2: Record<string, number> | undefined
): 'newer' | 'older' | 'concurrent' | 'equal' {
    const clk1 = v1 || {};
    const clk2 = v2 || {};
    
    const keys = new Set([...Object.keys(clk1), ...Object.keys(clk2)]);
    let greater = false;
    let lesser = false;
    
    for (const k of keys) {
        const val1 = clk1[k] || 0;
        const val2 = clk2[k] || 0;
        if (val1 > val2) greater = true;
        if (val1 < val2) lesser = true;
    }
    
    if (greater && lesser) return 'concurrent';
    if (greater) return 'newer';
    if (lesser) return 'older';
    return 'equal';
}

// Feature 18: Resolve concurrent updates using max clocks + JSON tie-breaker
export function resolveConcurrentConflict<T extends TimeBlock>(a: T, b: T): T {
    const mergedClock: Record<string, number> = {};
    const allKeys = new Set([...Object.keys(a.vectorClock || {}), ...Object.keys(b.vectorClock || {})]);
    for (const k of allKeys) {
        const valA = (a.vectorClock || {})[k] || 0;
        const valB = (b.vectorClock || {})[k] || 0;
        mergedClock[k] = Math.max(valA, valB);
    }
    
    const strA = JSON.stringify(a);
    const strB = JSON.stringify(b);
    const winner = strA >= strB ? a : b;
    
    return {
        ...winner,
        vectorClock: mergedClock
    };
}

// Feature 18: Filter arrays selecting the causally newest versions
export function deduplicateByVectorClock<T extends TimeBlock>(items: T[]): T[] {
    const map = new Map<string, T>();
    for (const item of items) {
        const existing = map.get(item.id);
        if (!existing) {
            map.set(item.id, item);
        } else {
            const comparison = compareVectorClocks(item.vectorClock, existing.vectorClock);
            if (comparison === 'newer') {
                map.set(item.id, item);
            } else if (comparison === 'concurrent') {
                const resolved = resolveConcurrentConflict(item, existing);
                map.set(item.id, resolved);
            }
        }
    }
    return Array.from(map.values());
}

// Feature 19: Retrieve priority at a specific timestamp in user's timezone
export function getPriorityAt(timestamp: number, policy: SchedulingPolicy): 'PRIVATE' | 'WORK' {
    const timezone = policy.timezone || 'UTC';
    
    // Parse time components robustly and locale-independently
    const parts = new Intl.DateTimeFormat('en-US', {
        timeZone: timezone,
        hour: 'numeric',
        minute: 'numeric',
        hour12: false,
        weekday: 'short'
    }).formatToParts(new Date(timestamp));

    let localHour = 0;
    let localMinute = 0;
    let weekdayStr = 'Sun';

    for (const part of parts) {
        if (part.type === 'hour') localHour = parseInt(part.value, 10);
        if (part.type === 'minute') localMinute = parseInt(part.value, 10);
        if (part.type === 'weekday') weekdayStr = part.value;
    }

    const localMinutes = localHour * 60 + localMinute;
    const weekdays = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    const localDay = weekdays.indexOf(weekdayStr);

    for (const rule of policy.rules) {
        if (rule.days && !rule.days.includes(localDay)) {
            continue;
        }
        const startMin = (rule.startHour ?? 0) * 60 + (rule.startMinute ?? 0);
        const endMin = (rule.endHour ?? 24) * 60 + (rule.endMinute ?? 0);
        
        if (localMinutes >= startMin && localMinutes <= endMin) {
            return rule.priority;
        }
    }
    
    return policy.defaultPriority;
}

// Feature 18, 19: Schedule conflict resolution using vector clocks & policy rules
export function resolveSchedule(
    anchors: PrivateLifeAnchor[], 
    tasks: WorkTask[],
    policy?: SchedulingPolicy
): { validAnchors: PrivateLifeAnchor[], validTasks: WorkTask[] } {
    // 1. Causal deduplication
    const uniqueAnchors = deduplicateByVectorClock(anchors);
    const uniqueTasks = deduplicateByVectorClock(tasks);

    if (!policy) {
        // Fallback: PrivateLifeAnchor strictly wins
        const validAnchors = [...uniqueAnchors];
        const validTasks = uniqueTasks.filter(task => {
            return uniqueAnchors.every(anchor => noOverlap(anchor, task));
        });
        return { validAnchors, validTasks };
    }

    // 2. Policy-driven preemption evaluation
    const preemptedAnchors = new Set<string>();
    const preemptedTasks = new Set<string>();

    for (const anchor of uniqueAnchors) {
        for (const task of uniqueTasks) {
            if (!noOverlap(anchor, task)) {
                const overlapStart = Math.max(anchor.start, task.start);
                const overlapEnd = Math.min(anchor.end, task.end);
                const midPoint = (overlapStart + overlapEnd) / 2;
                
                const priority = getPriorityAt(midPoint, policy);
                if (priority === 'PRIVATE') {
                    preemptedTasks.add(task.id);
                } else {
                    preemptedAnchors.add(anchor.id);
                }
            }
        }
    }

    const validAnchors = uniqueAnchors.filter(a => !preemptedAnchors.has(a.id));
    const validTasks = uniqueTasks.filter(t => !preemptedTasks.has(t.id));

    return { validAnchors, validTasks };
}
```

---

### B. Modifications in `src/index.ts`

```typescript
// Proposed src/index.ts additions/updates
import * as functions from 'firebase-functions';
import * as admin from 'firebase-admin';
import { 
    resolveSchedule, 
    PrivateLifeAnchor, 
    WorkTask, 
    compareVectorClocks, 
    resolveConcurrentConflict,
    SchedulingPolicy 
} from './crdt';

admin.initializeApp();
const db = admin.firestore();

// Shared scheduler execution engine
async function reconcileSchedule(userId: string) {
    // Fetch policy
    const policySnap = await db.doc(`users/${userId}/policy/current`).get();
    const policy = policySnap.exists ? (policySnap.data() as SchedulingPolicy) : undefined;

    // Fetch anchors and tasks
    const [anchorsSnap, tasksSnap] = await Promise.all([
        db.collection(`users/${userId}/private_anchors`).get(),
        db.collection(`users/${userId}/work_tasks`).get()
    ]);

    const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
    const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

    // Run resolution
    const { validAnchors, validTasks } = resolveSchedule(anchors, tasks, policy);

    const validAnchorIds = new Set(validAnchors.map(a => a.id));
    const validTaskIds = new Set(validTasks.map(t => t.id));

    const batch = db.batch();
    let hasDeletes = false;

    // Symmetrically delete preempted documents
    anchorsSnap.docs.forEach(doc => {
        if (!validAnchorIds.has(doc.id)) {
            console.log(`[Chronos AI] PrivateLifeAnchor ${doc.id} preempted. Removing.`);
            batch.delete(doc.ref);
            hasDeletes = true;
        }
    });

    tasksSnap.docs.forEach(doc => {
        if (!validTaskIds.has(doc.id)) {
            console.log(`[Chronos AI] WorkTask ${doc.id} preempted. Removing.`);
            batch.delete(doc.ref);
            hasDeletes = true;
        }
    });

    if (hasDeletes) {
        await batch.commit();
    }
}

// TRIGGERS FOR WORK_TASKS

export const onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newTask = snap.data() as WorkTask;

        // Feature 31: Strict Temporal Validation
        if (newTask.start >= newTask.end || newTask.start <= 0 || newTask.end <= 0) {
            console.error(`[Chronos AI] Validation failed for WorkTask ${newTask.id}. Deleting.`);
            await snap.ref.delete();
            return;
        }

        await reconcileSchedule(userId);
    });

export const onWorkTaskUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const before = change.before.data() as WorkTask;
        const after = change.after.data() as WorkTask;

        // Feature 31: Strict Temporal Validation
        if (after.start >= after.end || after.start <= 0 || after.end <= 0) {
            console.error(`[Chronos AI] Validation failed for WorkTask ${after.id}. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        // Feature 18: Vector Clock Causal Check
        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                console.log(`[Chronos AI] WorkTask ${after.id} update is causally older. Reverting.`);
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
                console.log(`[Chronos AI] WorkTask ${after.id} update is concurrent. Resolving.`);
                const resolved = resolveConcurrentConflict(after, before);
                if (JSON.stringify(resolved) !== JSON.stringify(after)) {
                    await change.after.ref.set(resolved);
                    return;
                }
            }
        }

        await reconcileSchedule(userId);
    });

// TRIGGERS FOR PRIVATE_ANCHORS

export const onPrivateAnchorAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newAnchor = snap.data() as PrivateLifeAnchor;

        // Feature 31: Strict Temporal Validation
        if (newAnchor.start >= newAnchor.end || newAnchor.start <= 0 || newAnchor.end <= 0) {
            console.error(`[Chronos AI] Validation failed for PrivateLifeAnchor ${newAnchor.id}. Deleting.`);
            await snap.ref.delete();
            return;
        }

        await reconcileSchedule(userId);
    });

export const onPrivateAnchorUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const before = change.before.data() as PrivateLifeAnchor;
        const after = change.after.data() as PrivateLifeAnchor;

        // Feature 31: Strict Temporal Validation
        if (after.start >= after.end || after.start <= 0 || after.end <= 0) {
            console.error(`[Chronos AI] Validation failed for PrivateLifeAnchor ${after.id}. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        // Feature 18: Vector Clock Causal Check
        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                console.log(`[Chronos AI] PrivateLifeAnchor ${after.id} update is causally older. Reverting.`);
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
                console.log(`[Chronos AI] PrivateLifeAnchor ${after.id} update is concurrent. Resolving.`);
                const resolved = resolveConcurrentConflict(after, before);
                if (JSON.stringify(resolved) !== JSON.stringify(after)) {
                    await change.after.ref.set(resolved);
                    return;
                }
            }
        }

        await reconcileSchedule(userId);
    });
```

---

## 5. Verification Plan (Jest Test Proposals)

The following tests are proposed for integration into `tests/crdt.test.ts` to verify the CRDT, validation, and policy engine operations:

```typescript
// Proposed test additions for tests/crdt.test.ts
import { 
    PrivateLifeAnchor, 
    WorkTask, 
    SchedulingPolicy, 
    compareVectorClocks, 
    resolveConcurrentConflict, 
    resolveSchedule 
} from '../src/crdt';

describe('Feature 18: Vector Clock Causal Consistency', () => {
    it('should correctly classify vector clock relations', () => {
        expect(compareVectorClocks({ a: 1 }, {})).toBe('newer');
        expect(compareVectorClocks({}, { a: 1 })).toBe('older');
        expect(compareVectorClocks({ a: 1 }, { a: 1 })).toBe('equal');
        expect(compareVectorClocks({ a: 1, b: 0 }, { a: 0, b: 1 })).toBe('concurrent');
    });

    it('should resolve concurrent updates using element-wise max clocks and deterministic winner selection', () => {
        const v1: WorkTask = {
            id: 'work-1',
            type: 'WORK',
            start: 100,
            end: 200,
            vectorClock: { a: 1, b: 0 }
        };
        const v2: WorkTask = {
            id: 'work-1',
            type: 'WORK',
            start: 150,
            end: 250,
            vectorClock: { a: 0, b: 1 }
        };

        const resolved = resolveConcurrentConflict(v1, v2);
        expect(resolved.vectorClock).toEqual({ a: 1, b: 1 });
        // The winner should be based on string comparison of serialized JSON
        const expectedWinner = JSON.stringify(v1) >= JSON.stringify(v2) ? v1 : v2;
        expect(resolved.start).toBe(expectedWinner.start);
    });
});

describe('Feature 19: Dynamic Scheduling Policy Engine', () => {
    const policy: SchedulingPolicy = {
        timezone: 'UTC',
        defaultPriority: 'PRIVATE',
        rules: [
            {
                name: 'Working hours priority',
                days: [1, 2, 3, 4, 5], // Mon-Fri
                startHour: 9,
                startMinute: 0,
                endHour: 17,
                endMinute: 0,
                priority: 'WORK'
            }
        ]
    };

    it('prioritizes WORK during weekday work hours', () => {
        const privateAnchor: PrivateLifeAnchor = {
            id: 'private-1',
            type: 'PRIVATE',
            description: 'Midday Break',
            start: new Date('2026-06-15T12:00:00Z').getTime(), // Mon 12:00
            end: new Date('2026-06-15T13:00:00Z').getTime()
        };
        const workTask: WorkTask = {
            id: 'work-1',
            type: 'WORK',
            start: new Date('2026-06-15T12:30:00Z').getTime(), // Overlaps
            end: new Date('2026-06-15T13:30:00Z').getTime()
        };

        const result = resolveSchedule([privateAnchor], [workTask], policy);
        expect(result.validTasks).toHaveLength(1);
        expect(result.validTasks[0].id).toBe('work-1');
        expect(result.validAnchors).toHaveLength(0); // Private anchor is preempted
    });

    it('prioritizes PRIVATE during weekends', () => {
        const privateAnchor: PrivateLifeAnchor = {
            id: 'private-1',
            type: 'PRIVATE',
            description: 'Weekend Social',
            start: new Date('2026-06-20T12:00:00Z').getTime(), // Sat 12:00
            end: new Date('2026-06-20T13:00:00Z').getTime()
        };
        const workTask: WorkTask = {
            id: 'work-1',
            type: 'WORK',
            start: new Date('2026-06-20T12:30:00Z').getTime(), // Overlaps
            end: new Date('2026-06-20T13:30:00Z').getTime()
        };

        const result = resolveSchedule([privateAnchor], [workTask], policy);
        expect(result.validTasks).toHaveLength(0); // Work task is preempted
        expect(result.validAnchors).toHaveLength(1);
        expect(result.validAnchors[0].id).toBe('private-1');
    });
});
```
