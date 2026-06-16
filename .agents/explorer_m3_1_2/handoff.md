# Handoff Report - Sub-Milestone 1 (Core CRDT & Policy Extensions)

## 1. Observation

During read-only exploration of `/home/matthias/project/project-chronos/backend-functions`, the following files and details were examined:

1. **`src/crdt.ts`** (Lines 1–35): Defines the original `TimeBlock`, `PrivateLifeAnchor`, and `WorkTask` structures:
   ```typescript
   export interface TimeBlock {
       id: string;
       start: number; // Unix timestamp for simplicity
       end: number;
   }
   ```
   And the original hardcoded conflict resolution rule (lines 25–31):
   ```typescript
   export function resolveSchedule(
       anchors: PrivateLifeAnchor[], 
       tasks: WorkTask[]
   ): { validAnchors: PrivateLifeAnchor[], validTasks: WorkTask[] } {
       // CRDT Resolution Rule: PrivateLifeAnchor strictly wins.
       const validAnchors = [...anchors];
       
       // Filter out any work tasks that conflict with ANY private life anchor
       const validTasks = tasks.filter(task => {
           return anchors.every(anchor => noOverlap(anchor, task));
       });

       return { validAnchors, validTasks };
   }
   ```

2. **`src/index.ts`** (Lines 1–118): Defines Firebase triggers for `onCreate` events.
   - `onWorkTaskAdded` (lines 9–38) fetches anchors and tasks and filters the new task if invalid.
   - `onPrivateAnchorAdded` (lines 40–67) fetches all work tasks and deletes any that conflict with private anchors.

3. **`tests/crdt.test.ts`** (Lines 1–78): Contains three test cases testing the overlap resolution:
   - "must automatically block work assignments overlapping with family festival at Dorfwiese"
   - "must prioritize sync with Sandra, Ryan, Lara, Melina, Elena, and Emma over work"
   - "allows work tasks in TimeGaps without overlap"

4. **`package.json`** (Lines 1–25): Specifies testing via Jest (`"test": "jest"`). Running `npm test` inside `/home/matthias/project/project-chronos/backend-functions` returns:
   ```
   PASS tests/crdt.test.ts (11.616 s)
     Life-First BDD Testing (CRDT Synchronization)
       ✓ must automatically block work assignments overlapping with family festival at Dorfwiese (38 ms)
       ✓ must prioritize sync with Sandra, Ryan, Lara, Melina, Elena, and Emma over work (6 ms)
       ✓ allows work tasks in TimeGaps without overlap (6 ms)

   Test Suites: 1 passed, 1 total
   Tests:       3 passed, 3 total
   ```

---

## 2. Logic Chain

1. **Feature 18 (Vector Clock Schema)**: To track causal history, `TimeBlock` must be expanded with `vectorClock?: Record<string, number>`. Conflict resolution needs a vector clock comparison utility (`compareVectorClocks`) to determine if one clock dominates (`GREATER`/`LESS`) or if they are `CONCURRENT`/`EQUAL`.
2. **Feature 19 (Dynamic Scheduling Policy Engine)**: The hardcoded rule in `resolveSchedule` must be replaced by loading a JSON policy from `users/{userId}/policy/current`. This policy should specify priorities (e.g. `{ PRIVATE: 2, WORK: 1 }`). We pass this policy to `resolveSchedule`. If vector clocks are concurrent or missing, we fall back to comparing type priorities from the policy, using a stable lexicographical tie-breaker on block IDs when priorities are equal.
3. **Feature 31 (Strict Temporal Bounds Validation)**: We need to validate that `start < end` and `bounds > 0` (meaning `start > 0` and `end > 0`). This validation must occur on both `onCreate` and `onUpdate`. Thus, we must create `onWorkTaskUpdated` and `onPrivateAnchorUpdated` triggers in `src/index.ts`. If an update fails validation, we revert the Firestore document to its previous state (`change.before`). If a creation fails validation, we delete the document.

---

## 3. Caveats

- **Policy Complexity**: The policy parsing assumes a flat priorities structure (e.g., `{ priorities: Record<string, number> }`). If more complex scheduling rules are added, parsing logic will need extending.
- **Overlap Models**: The report details two conflict models:
  - *Bipartite Overlap Resolution* (Recommended for compatibility): Allows overlapping anchors/tasks within the same category to coexist (matching original behavior), only resolving conflicts *across* categories (PRIVATE vs WORK).
  - *Absolute Greedy Overlap Resolution*: Disallows any overlapping blocks entirely, regardless of type.
- **Emulator State**: Development/emulation setup was not run or deployed on live Firestore instances since the explorer agent role is strictly read-only.

---

## 4. Conclusion

The design strategy defined in `/home/matthias/project/project-chronos/.agents/explorer_m3_1_2/analysis.md` provides a complete, robust blueprint for implementing Features 18, 19, and 31. By extending type definitions, introducing causal ordering, supporting user-defined policies, and adding update triggers, the application will enforce boundaries dynamically while preserving historical causal changes.

---

## 5. Verification Method

- **Build Code**: Run `npm run build` in `/home/matthias/project/project-chronos/backend-functions` to verify it compiles.
- **Run Tests**: Execute `npm test` in the backend functions directory.
- **Verify Test Suite**: Verify that the new tests (detailed in `analysis.md` Section 5) check vector clock ordering, dynamic policy priority fallback, and strict temporal boundary validation.
