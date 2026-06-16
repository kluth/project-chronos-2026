# Handoff Report: Sub-Milestone 1 Core CRDT & Policy Extensions

## 1. Observation

Direct observations of the codebase:
*   In `backend-functions/src/crdt.ts` (lines 1-15), the data models (`TimeBlock`, `PrivateLifeAnchor`, and `WorkTask`) contain basic attributes (`id`, `start`, `end`, `type`, `description`, `jiraId`), but lack logical clock attributes.
*   In `backend-functions/src/crdt.ts` (lines 21-34), conflict resolution is hardcoded to prioritize private anchors:
    ```typescript
    // CRDT Resolution Rule: PrivateLifeAnchor strictly wins.
    const validAnchors = [...anchors];
    
    // Filter out any work tasks that conflict with ANY private life anchor
    const validTasks = tasks.filter(task => {
        return anchors.every(anchor => noOverlap(anchor, task));
    });
    ```
*   In `backend-functions/src/index.ts`, `onWorkTaskAdded` (lines 9-38) and `onPrivateAnchorAdded` (lines 40-67) handle the creation trigger logic (`onCreate`) but do not monitor update actions (`onUpdate`).
*   In `backend-functions/src/index.ts`, validation checks on incoming time blocks are minimal and do not strictly verify `start < end` or `bounds > 0` before triggering logic.
*   Tests in `backend-functions/tests/crdt.test.ts` pass successfully when running `npm test`.

---

## 2. Logic Chain

From these observations, we conclude:
1.  **Logical Clock Implementation**: Since logical version tracking is absent in data schemas, we must introduce an optional logical `vectorClock?: Record<string, number>` on `TimeBlock` (line 1). This ensures downstream classes `PrivateLifeAnchor` and `WorkTask` inherit it.
2.  **Causal Consistency**: Causal resolution requires comparisons. We formulate a comparison engine (`compareVectorClocks`) to identify which version is newer or older, and a resolver (`resolveConcurrentConflict`) to resolve concurrency by merging vector clocks (element-wise maximum) and selecting the winner deterministically (e.g. lexicographically sorting serialized JSON).
3.  **Conflict Resolution Loop Prevention**: Integrating vector clock comparison in Firestore `onUpdate` triggers will prevent stale writes (e.g. from offline clients syncing out-of-order) and revert them. Guarding updates ensures no infinite cycles occur during reversion.
4.  **Dynamic Scheduling Policy Engine**: Changing conflict resolution dynamically requires loading a JSON policy structure from `users/{userId}/policy/current`. We shift the preemption decision to `resolveSchedule` using `getPriorityAt(midpoint, policy)` where the midpoint of the temporal conflict is matched against timezone-converted scheduling rules.
5.  **Strict Temporal Bounds Validation**: Implementing Feature 31 requires adding assertion checks at the beginning of `onCreate` and `onUpdate` triggers in `src/index.ts` to reject and delete/revert entities that have invalid ranges (`start >= end` or non-positive values).

---

## 3. Caveats

*   **Timezone Formatting**: Node.js `Intl.DateTimeFormat` with options was chosen to avoid importing external timezone packages. It relies on standard browser/Node configurations.
*   **Vector Clock Initialization**: We assume legacy updates without `vectorClock` are treated as default (last-write-wins) rather than failing, to maintain compatibility.
*   **Recursion Prevention**: While Firestore trigger logic does not invoke triggers recursively for document deletions, updates (`change.after.ref.set`) will fire updates. Our design guards this by performing updates only if the resolved document differs from the post-write document.

---

## 4. Conclusion

We propose a robust read-only strategy for Sub-Milestone 1, providing complete code signatures for `src/crdt.ts` and `src/index.ts`, and a set of Unit and Integration tests to run with `npm test`.

---

## 5. Verification Method

To independently verify this implementation when code is written:
1.  **Test Execution**: Run `npm test` inside `backend-functions/` to execute Jest.
2.  **Verify New Cases**: Ensure that the vector clock relation test, conflict resolution test, and policy-driven scheduling test pass successfully.
3.  **Validation Check**: Attempt to write an invalid time block (`start: 500`, `end: 400`) in Firestore emulator, and verify that the trigger deletes the document (`onCreate`) or reverts the document (`onUpdate`).
