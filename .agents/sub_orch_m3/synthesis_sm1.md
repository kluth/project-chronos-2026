# Synthesized Implementation Plan: Sub-Milestone 1 (Core CRDT & Policy Extensions)

## 1. File: `backend-functions/src/crdt.ts`

### Interface updates:
```typescript
export interface TimeBlock {
    id: string;
    start: number; // Unix timestamp for simplicity
    end: number;
    vectorClock?: Record<string, number>;
}

export interface PrivateLifeAnchor extends TimeBlock {
    type: 'PRIVATE';
    description: string;
}

export interface WorkTask extends TimeBlock {
    type: 'WORK';
    jiraId?: string;
}

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
```

### Logical Vector Clocks utilities:
```typescript
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
```

### Dynamic Policy checking utilities:
```typescript
export function getPriorityAt(timestamp: number, policy: SchedulingPolicy): 'PRIVATE' | 'WORK' {
    const timezone = policy.timezone || 'UTC';
    
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
```

### Overlap and Resolution update:
```typescript
export function noOverlap(t1: TimeBlock, t2: TimeBlock): boolean {
    return t1.end <= t2.start || t2.end <= t1.start;
}

export function resolveSchedule(
    anchors: PrivateLifeAnchor[], 
    tasks: WorkTask[],
    policy?: SchedulingPolicy
): { validAnchors: PrivateLifeAnchor[], validTasks: WorkTask[] } {
    const uniqueAnchors = deduplicateByVectorClock(anchors);
    const uniqueTasks = deduplicateByVectorClock(tasks);

    if (!policy) {
        const validAnchors = [...uniqueAnchors];
        const validTasks = uniqueTasks.filter(task => {
            return uniqueAnchors.every(anchor => noOverlap(anchor, task));
        });
        return { validAnchors, validTasks };
    }

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

## 2. File: `backend-functions/src/index.ts`

Ensure triggers do reconciliation correctly:
```typescript
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

async function reconcileSchedule(userId: string) {
    const policySnap = await db.doc(`users/${userId}/policy/current`).get();
    const policy = policySnap.exists ? (policySnap.data() as SchedulingPolicy) : undefined;

    const [anchorsSnap, tasksSnap] = await Promise.all([
        db.collection(`users/${userId}/private_anchors`).get(),
        db.collection(`users/${userId}/work_tasks`).get()
    ]);

    const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
    const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

    const { validAnchors, validTasks } = resolveSchedule(anchors, tasks, policy);

    const validAnchorIds = new Set(validAnchors.map(a => a.id));
    const validTaskIds = new Set(validTasks.map(t => t.id));

    const batch = db.batch();
    let hasDeletes = false;

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

// Validation logic for Feature 31
function validateTemporalBounds(block: any): boolean {
    if (!block || typeof block !== 'object') return false;
    const { start, end } = block;
    if (typeof start !== 'number' || typeof end !== 'number') return false;
    return start > 0 && end > 0 && start < end;
}

export const onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newTask = snap.data() as WorkTask;

        if (!validateTemporalBounds(newTask)) {
            console.warn(`[Chronos AI] WorkTask ${newTask.id} has invalid bounds. Deleting.`);
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

        if (!validateTemporalBounds(after)) {
            console.warn(`[Chronos AI] WorkTask ${after.id} has invalid bounds on update. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
                const resolved = resolveConcurrentConflict(after, before);
                if (JSON.stringify(resolved) !== JSON.stringify(after)) {
                    await change.after.ref.set(resolved);
                    return;
                }
            }
        }

        await reconcileSchedule(userId);
    });

export const onPrivateAnchorAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newAnchor = snap.data() as PrivateLifeAnchor;

        if (!validateTemporalBounds(newAnchor)) {
            console.warn(`[Chronos AI] PrivateAnchor ${newAnchor.id} has invalid bounds. Deleting.`);
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

        if (!validateTemporalBounds(after)) {
            console.warn(`[Chronos AI] PrivateAnchor ${after.id} has invalid bounds on update. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
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
