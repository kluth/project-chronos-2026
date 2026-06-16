# Implementation Strategy & Design Analysis: Sub-Milestone 1

This document outlines the proposed design and implementation strategy for **Sub-Milestone 1 (Core CRDT & Policy Extensions: Features 18, 19, and 31)** in `/home/matthias/project/project-chronos/backend-functions`.

---

## 1. Architectural Design Overview

The task involves expanding the existing scheduling/conflict resolution system to support:
1. **Feature 18 (Vector Clock Schema)**: Track causal history and resolve causal conflicts using vector clocks, falling back to other resolution mechanisms when concurrent or absent.
2. **Feature 19 (Dynamic Scheduling Policy Engine)**: Support custom, user-specified priorities in Firestore (`users/{userId}/policy/current`) instead of the hardcoded private-wins rule.
3. **Feature 31 (Strict Temporal Bounds Validation)**: Validate temporal sanity on both document creation and updates, preventing invalid spans from being written.

We propose a unified, pure-functional conflict resolution algorithm inside `src/crdt.ts` combined with Firestore trigger enhancements inside `src/index.ts` to fetch policy documents, run validation, and execute/revert/delete schedule states.

---

## 2. Proposed Interface Extensions (`src/crdt.ts`)

We expand `TimeBlock` to include optional vector clocks and define relation metadata for clock comparison.

### Vector Clock & Policy Types
```typescript
export type VectorClock = Record<string, number>;

export enum ClockRelation {
    LESS = -1,
    EQUAL = 0,
    GREATER = 1,
    CONCURRENT = 2
}

export interface TimeBlock {
    id: string;
    start: number; // Unix timestamp for simplicity
    end: number;
    vectorClock?: VectorClock;
}

export interface PrivateLifeAnchor extends TimeBlock {
    type: 'PRIVATE';
    description: string;
}

export interface WorkTask extends TimeBlock {
    type: 'WORK';
    jiraId?: string;
}

export interface SchedulingPolicy {
    priorities?: Record<string, number>; // e.g. { "PRIVATE": 2, "WORK": 1 }
}
```

---

## 3. Conflict Resolution Algorithm Design (`src/crdt.ts`)

### 3.1 Vector Clock Comparison
A vector clock $V_1$ dominates $V_2$ ($V_1 > V_2$) if for all nodes $k$, $V_1[k] \ge V_2[k]$, and there exists some node $j$ such that $V_1[j] > V_2[j]$. If neither dominates and they are not equal, they are concurrent.

```typescript
export function compareVectorClocks(vc1?: VectorClock, vc2?: VectorClock): ClockRelation {
    if (!vc1 && !vc2) return ClockRelation.EQUAL;
    if (!vc1) return ClockRelation.LESS;     // vc2 is newer
    if (!vc2) return ClockRelation.GREATER;  // vc1 is newer

    let less = false;
    let greater = false;

    const keys = new Set([...Object.keys(vc1), ...Object.keys(vc2)]);
    for (const key of keys) {
        const val1 = vc1[key] ?? 0;
        const val2 = vc2[key] ?? 0;
        if (val1 < val2) {
            less = true;
        } else if (val1 > val2) {
            greater = true;
        }
    }

    if (less && greater) return ClockRelation.CONCURRENT;
    if (less) return ClockRelation.LESS;
    if (greater) return ClockRelation.GREATER;
    return ClockRelation.EQUAL;
}
```

### 3.2 Dynamic Conflict Rule Winner Determination
We define a deterministic strict total order `wins(t1, t2)` to decide which of two overlapping blocks wins:
1. **Causal Dominance**: Check if one vector clock is strictly newer than the other.
2. **Dynamic Policy**: If clocks are concurrent/equal, fall back to policy priorities.
3. **Lexicographical ID**: If priorities are equal, resolve using a stable tie-breaker (ID comparison).

```typescript
export function wins(t1: TimeBlock, t2: TimeBlock, policy?: SchedulingPolicy): boolean {
    // 1. Check Vector Clocks
    const relation = compareVectorClocks(t1.vectorClock, t2.vectorClock);
    if (relation === ClockRelation.GREATER) return true;
    if (relation === ClockRelation.LESS) return false;

    // 2. Fall back to Policy priorities
    const type1 = (t1 as any).type || 'WORK';
    const type2 = (t2 as any).type || 'WORK';

    const p1 = policy?.priorities?.[type1] ?? (type1 === 'PRIVATE' ? 2 : 1);
    const p2 = policy?.priorities?.[type2] ?? (type2 === 'PRIVATE' ? 2 : 1);

    if (p1 !== p2) {
        return p1 > p2;
    }

    // 3. Fall back to deterministic stable tie-breaker
    return t1.id > t2.id;
}
```

### 3.3 Overlap Filter Options
We propose two alternative approaches to resolving multi-block overlap conflicts.

#### Option A: Bipartite Overlap Resolution (Recommended)
This option preserves overlaps within the same category (e.g. two overlapping private events or two overlapping work events are both allowed), but resolves overlaps *between* work and private categories using vector clocks and policy rules. This preserves backward compatibility.

```typescript
export function resolveSchedule(
    anchors: PrivateLifeAnchor[], 
    tasks: WorkTask[],
    policy?: SchedulingPolicy
): { validAnchors: PrivateLifeAnchor[], validTasks: WorkTask[] } {
    const allBlocks = [...anchors, ...tasks];

    // Sort all blocks in descending order of priority (wins = true comes first)
    const sorted = allBlocks.sort((t1, t2) => (wins(t1, t2, policy) ? -1 : 1));

    const kept: TimeBlock[] = [];
    for (const block of sorted) {
        const hasConflictingOverlap = kept.some(k => {
            const differentType = (k as any).type !== (block as any).type;
            return differentType && !noOverlap(k, block);
        });
        if (!hasConflictingOverlap) {
            kept.push(block);
        }
    }

    const validAnchors = kept.filter((b): b is PrivateLifeAnchor => (b as any).type === 'PRIVATE');
    const validTasks = kept.filter((b): b is WorkTask => (b as any).type === 'WORK');

    return { validAnchors, validTasks };
}
```

#### Option B: Absolute Greedy Overlap Resolution
No two overlapping intervals of *any* type can coexist. In this option, the higher-priority block completely invalidates any overlapping block, regardless of type.

```typescript
export function resolveSchedule(
    anchors: PrivateLifeAnchor[], 
    tasks: WorkTask[],
    policy?: SchedulingPolicy
): { validAnchors: PrivateLifeAnchor[], validTasks: WorkTask[] } {
    const allBlocks = [...anchors, ...tasks];

    const sorted = allBlocks.sort((t1, t2) => (wins(t1, t2, policy) ? -1 : 1));

    const kept: TimeBlock[] = [];
    for (const block of sorted) {
        const hasOverlap = kept.some(k => !noOverlap(k, block));
        if (!hasOverlap) {
            kept.push(block);
        }
    }

    const validAnchors = kept.filter((b): b is PrivateLifeAnchor => (b as any).type === 'PRIVATE');
    const validTasks = kept.filter((b): b is WorkTask => (b as any).type === 'WORK');

    return { validAnchors, validTasks };
}
```

---

## 4. Strict Validation & Trigger Handling (`src/index.ts`)

### 4.1 Temporal Validation Helper
```typescript
export function validateTemporalBounds(block: any): boolean {
    if (!block || typeof block !== 'object') return false;
    const { start, end } = block;
    if (typeof start !== 'number' || typeof end !== 'number') return false;
    return start > 0 && end > 0 && start < end;
}
```

### 4.2 Extended Triggers with onCreate and onUpdate
We implement validation and conflict resolution across both document creation and document updates. If validation fails on update, we revert the document to its original state.

#### 1. Work Tasks triggers
```typescript
export const onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newTask = snap.data() as WorkTask;

        if (!validateTemporalBounds(newTask)) {
            console.warn(`[Chronos AI] WorkTask ${newTask.id} has invalid temporal bounds. Deleting.`);
            await snap.ref.delete();
            return;
        }

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? policySnap.data() : undefined;

        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
        
        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const resolution = resolveSchedule(anchors, tasks, policy);
        const isTaskValid = resolution.validTasks.some(t => t.id === newTask.id);

        if (!isTaskValid) {
            console.log(`[Chronos AI] WorkTask ${newTask.id} violates schedule. Deleting.`);
            await snap.ref.delete();
        }
    });

export const onWorkTaskUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const oldTask = change.before.data() as WorkTask;
        const newTask = change.after.data() as WorkTask;

        if (!validateTemporalBounds(newTask)) {
            console.warn(`[Chronos AI] Updated WorkTask ${newTask.id} has invalid bounds. Reverting.`);
            await change.after.ref.set(oldTask);
            return;
        }

        if (
            oldTask.start === newTask.start &&
            oldTask.end === newTask.end &&
            JSON.stringify(oldTask.vectorClock) === JSON.stringify(newTask.vectorClock)
        ) {
            return;
        }

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? policySnap.data() : undefined;

        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
        
        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const resolution = resolveSchedule(anchors, tasks, policy);
        const isTaskValid = resolution.validTasks.some(t => t.id === newTask.id);

        if (!isTaskValid) {
            console.log(`[Chronos AI] Updated WorkTask ${newTask.id} is invalid. Reverting.`);
            await change.after.ref.set(oldTask);
        }
    });
```

#### 2. Private Anchors triggers
```typescript
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

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? policySnap.data() : undefined;

        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);

        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const resolution = resolveSchedule(anchors, tasks, policy);
        const isAnchorValid = resolution.validAnchors.some(a => a.id === newAnchor.id);

        if (!isAnchorValid) {
            console.log(`[Chronos AI] PrivateAnchor ${newAnchor.id} preempted. Deleting.`);
            await snap.ref.delete();
            return;
        }

        // Apply preemption to other items
        const validTaskIds = new Set(resolution.validTasks.map(t => t.id));
        const validAnchorIds = new Set(resolution.validAnchors.map(a => a.id));

        const batch = db.batch();
        tasksSnap.docs.forEach(doc => {
            if (!validTaskIds.has(doc.data().id)) {
                console.log(`[Chronos AI] WorkTask ${doc.data().id} preempted. Removing.`);
                batch.delete(doc.ref);
            }
        });
        anchorsSnap.docs.forEach(doc => {
            if (!validAnchorIds.has(doc.data().id)) {
                console.log(`[Chronos AI] PrivateAnchor ${doc.data().id} preempted. Removing.`);
                batch.delete(doc.ref);
            }
        });

        await batch.commit();
    });

export const onPrivateAnchorUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const oldAnchor = change.before.data() as PrivateLifeAnchor;
        const newAnchor = change.after.data() as PrivateLifeAnchor;

        if (!validateTemporalBounds(newAnchor)) {
            console.warn(`[Chronos AI] Updated PrivateAnchor ${newAnchor.id} has invalid bounds. Reverting.`);
            await change.after.ref.set(oldAnchor);
            return;
        }

        if (
            oldAnchor.start === newAnchor.start &&
            oldAnchor.end === newAnchor.end &&
            JSON.stringify(oldAnchor.vectorClock) === JSON.stringify(newAnchor.vectorClock)
        ) {
            return;
        }

        const policySnap = await db.doc(`users/${userId}/policy/current`).get();
        const policy = policySnap.exists ? policySnap.data() : undefined;

        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);

        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const resolution = resolveSchedule(anchors, tasks, policy);
        const isAnchorValid = resolution.validAnchors.some(a => a.id === newAnchor.id);

        if (!isAnchorValid) {
            console.log(`[Chronos AI] Updated PrivateAnchor ${newAnchor.id} preempted. Reverting.`);
            await change.after.ref.set(oldAnchor);
            return;
        }

        const validTaskIds = new Set(resolution.validTasks.map(t => t.id));
        const validAnchorIds = new Set(resolution.validAnchors.map(a => a.id));

        const batch = db.batch();
        tasksSnap.docs.forEach(doc => {
            if (!validTaskIds.has(doc.data().id)) {
                console.log(`[Chronos AI] WorkTask ${doc.data().id} preempted. Removing.`);
                batch.delete(doc.ref);
            }
        });
        anchorsSnap.docs.forEach(doc => {
            if (!validAnchorIds.has(doc.data().id)) {
                console.log(`[Chronos AI] PrivateAnchor ${doc.data().id} preempted. Removing.`);
                batch.delete(doc.ref);
            }
        });

        await batch.commit();
    });
```

---

## 5. Test Design (`tests/crdt.test.ts` / new test file)

We propose adding the following BDD unit tests to verify conflict resolution under the new schemas.

```typescript
describe('Feature 18: Vector Clock Causal Conflict Resolution', () => {
    it('resolves overlapping blocks where one vector clock strictly dominates', () => {
        const oldAnchor: PrivateLifeAnchor = {
            id: 'anchor-1',
            type: 'PRIVATE',
            description: 'Family Outing',
            start: 100,
            end: 200,
            vectorClock: { clientA: 1 }
        };

        const newWorkTask: WorkTask = {
            id: 'task-1',
            type: 'WORK',
            start: 150,
            end: 250,
            vectorClock: { clientA: 2 } // Causally newer
        };

        const result = resolveSchedule([oldAnchor], [newWorkTask]);

        // Work task wins because it is causally newer, anchor is discarded
        expect(result.validTasks).toHaveLength(1);
        expect(result.validAnchors).toHaveLength(0);
    });

    it('retains the private life anchor if it is causally newer than the work task', () => {
        const newAnchor: PrivateLifeAnchor = {
            id: 'anchor-2',
            type: 'PRIVATE',
            description: 'Lunch',
            start: 100,
            end: 200,
            vectorClock: { clientA: 2 } // Causally newer
        };

        const oldWorkTask: WorkTask = {
            id: 'task-2',
            type: 'WORK',
            start: 150,
            end: 250,
            vectorClock: { clientA: 1 }
        };

        const result = resolveSchedule([newAnchor], [oldWorkTask]);

        expect(result.validTasks).toHaveLength(0);
        expect(result.validAnchors).toHaveLength(1);
    });
});

describe('Feature 19: Dynamic Scheduling Policy Engine', () => {
    it('resolves concurrent conflicts according to dynamic policy (WORK wins over PRIVATE)', () => {
        const anchor: PrivateLifeAnchor = {
            id: 'anchor-3',
            type: 'PRIVATE',
            description: 'Yoga',
            start: 100,
            end: 200,
            vectorClock: { clientA: 1 }
        };

        const task: WorkTask = {
            id: 'task-3',
            type: 'WORK',
            start: 150,
            end: 250,
            vectorClock: { clientB: 1 } // Concurrent
        };

        const workWinsPolicy = {
            priorities: { WORK: 2, PRIVATE: 1 }
        };

        const result = resolveSchedule([anchor], [task], workWinsPolicy);

        expect(result.validTasks).toHaveLength(1);
        expect(result.validAnchors).toHaveLength(0);
    });

    it('falls back to default priorities when policy is undefined or equal', () => {
        const anchor: PrivateLifeAnchor = {
            id: 'anchor-4',
            type: 'PRIVATE',
            description: 'Dinner',
            start: 100,
            end: 200
        };

        const task: WorkTask = {
            id: 'task-4',
            type: 'WORK',
            start: 150,
            end: 250
        };

        const result = resolveSchedule([anchor], [task]); // default: PRIVATE wins

        expect(result.validTasks).toHaveLength(0);
        expect(result.validAnchors).toHaveLength(1);
    });
});

describe('Feature 31: Strict Temporal Bounds Validation', () => {
    it('returns true for valid timeblocks', () => {
        expect(validateTemporalBounds({ start: 100, end: 200 })).toBe(true);
    });

    it('returns false when start >= end', () => {
        expect(validateTemporalBounds({ start: 200, end: 200 })).toBe(false);
        expect(validateTemporalBounds({ start: 250, end: 200 })).toBe(false);
    });

    it('returns false for negative or zero bounds', () => {
        expect(validateTemporalBounds({ start: -10, end: 50 })).toBe(false);
        expect(validateTemporalBounds({ start: 10, end: -5 })).toBe(false);
    });
});
```
