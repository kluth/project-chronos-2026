# BRIEFING — 2026-06-16T17:36:16Z

## Mission
Investigate the project code in backend-functions and propose a design strategy for Core CRDT & Policy Extensions.

## 🔒 My Identity
- Archetype: teamwork_preview_explorer
- Roles: Read-only exploration agent
- Working directory: /home/matthias/project/project-chronos/.agents/explorer_m3_1_3/
- Original parent: 1a51d808-f04a-45b7-9440-91d776812a3a
- Milestone: Sub-Milestone 1 (Core CRDT & Policy Extensions: Features 18, 19, and 31)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement or modify any source code.
- Report must be written to /home/matthias/project/project-chronos/.agents/explorer_m3_1_3/analysis.md.
- Notify the parent ID 1a51d808-f04a-45b7-9440-91d776812a3a using send_message upon completion.

## Current Parent
- Conversation ID: 1a51d808-f04a-45b7-9440-91d776812a3a
- Updated: 2026-06-16T17:36:16Z

## Investigation State
- **Explored paths**:
  - `/home/matthias/project/project-chronos/backend-functions/src/crdt.ts`
  - `/home/matthias/project/project-chronos/backend-functions/src/index.ts`
  - `/home/matthias/project/project-chronos/backend-functions/tests/crdt.test.ts`
  - `/home/matthias/project/project-chronos/backend-functions/package.json`
- **Key findings**:
  - `TimeBlock`, `PrivateLifeAnchor`, and `WorkTask` are defined in `src/crdt.ts`.
  - Conflict resolution currently runs in `src/crdt.ts#resolveSchedule` and hardcodes `PrivateLifeAnchor` strictly winning.
  - Triggers are defined in `src/index.ts` for `onCreate` events only; no `onUpdate` triggers exist.
  - Baseline test runs successfully using `npm test`.
- **Unexplored areas**: None. All components for Sub-Milestone 1 have been analyzed.

## Key Decisions Made
- Chose `Record<string, number>` as the optional `vectorClock` schema on `TimeBlock`.
- Proposed handling of concurrent clock conflicts via deterministic tie-breaker (lexicographical JSON string comparison) and merging clocks.
- Proposed midpoint evaluation of scheduling conflicts against user policy rules retrieved from Firestore at `users/{userId}/policy/current`.
- Proposed separate `onCreate` and `onUpdate` triggers with guards against recursive update loops to handle validation, vector clock updates, and preemption.

## Artifact Index
- /home/matthias/project/project-chronos/.agents/explorer_m3_1_3/analysis.md — Main analysis report
- /home/matthias/project/project-chronos/.agents/explorer_m3_1_3/handoff.md — Handoff report
