# Handoff Report: Core CRDT & Policy Extensions (Sub-Milestone 1)

## 1. Observation
We observed the existing implementation in `/home/matthias/project/project-chronos/backend-functions`:
- **`src/crdt.ts` (lines 1-15)** defines the scheduling entities:
  ```typescript
  export interface TimeBlock {
      id: string;
      start: number; // Unix timestamp for simplicity
      end: number;
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
- **`src/crdt.ts` (lines 21-34)** contains the hardcoded conflict-resolution rule:
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
- **`src/index.ts` (lines 9-67)** implements asynchronous `onCreate` database triggers (`onWorkTaskAdded` and `onPrivateAnchorAdded`) that fetch the user's data and apply preemption by deleting work tasks that fail the resolution check.
- **`tests/crdt.test.ts`** verified three baseline test cases showing successful conflict detection. Running `npm test` output:
  ```
  PASS tests/crdt.test.ts
    Life-First BDD Testing (CRDT Synchronization)
      ✓ must automatically block work assignments overlapping with family festival at Dorfwiese (11 ms)
      ✓ must prioritize sync with Sandra, Ryan, Lara, Melina, Elena, and Emma over work (2 ms)
      ✓ allows work tasks in TimeGaps without overlap (1 ms)
  ```

---

## 2. Logic Chain
- **Feature 18 (Vector Clock Schema)**: Expanding the `TimeBlock` interface to include `vectorClock?: Record<string, number>` allows tracking causal histories. To resolve causal conflicts, we must compare vector clocks when we see multiple versions of the same entity ID. If the clocks are concurrent, we apply a deterministic value-based tie-breaker (lexical comparison of serialized JSON documents) to guarantee convergence.
- **Feature 19 (Dynamic Scheduling Policy Engine)**: Instead of the hardcoded rule, we read user rules from `users/{userId}/policy/current`. When the engine encounters an overlap, it extracts the midpoint of the overlap, parses it against the rules in order (handling timezone conversion and overnight crossovers), and determines which type (`PRIVATE` or `WORK`) wins.
- **Feature 31 (Strict Temporal Bounds Validation)**: We migrate `onCreate` triggers to `onWrite` triggers to intercept updates and creations. We validate that `start < end`, `start > 0`, and `end > 0`. If a block fails, the trigger deletes the invalid document.

---

## 3. Caveats
- **Environment Assumptions**: Timezone resolution relies on standard Node.js support for the IANA timezone database in `toLocaleString`. If run in an environment without full Intl support, timezone conversions may fall back to UTC.
- **Firestore Rule Interactions**: Write triggers run asynchronously after a write succeeds. While they successfully enforce consistency by deleting invalid/preempted entries, they do not prevent temporary invalid writes from hitting the database before deletion.

---

## 4. Conclusion
We have completed a read-only investigation and fully designed the implementations for Features 18, 19, and 31. The resulting design strategies, interfaces, algorithms, and trigger integrations have been compiled in `/home/matthias/project/project-chronos/.agents/explorer_m3_1_1/analysis.md`. The design is fully compatible with the existing package layout.

---

## 5. Verification Method
1. **Compilation & Unit Tests**:
   - Navigate to `/home/matthias/project/project-chronos/backend-functions` and run:
     ```bash
     npm run build
     npm test
     ```
2. **Key Inspectable Targets**:
   - `tests/crdt.test.ts`: Ensure new test suites are added for:
     - Causal conflict resolution (deduplicating concurrent versions of the same event ID using vector clocks).
     - Policy parsing (testing daylight vs overnight hours rules).
     - Temporal validations (submitting negative start times or start >= end).
3. **Trigger Behavior**:
   - Verify that invalid writes to Firestore triggers are cleaned up (i.e. deleted) automatically by writing test mocks for `onWrite`.
