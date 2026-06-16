import { 
    PrivateLifeAnchor, 
    WorkTask, 
    resolveSchedule,
    compareVectorClocks,
    resolveConcurrentConflict,
    deduplicateByVectorClock,
    getPriorityAt,
    SchedulingPolicy
} from '../src/crdt';
import { validateTemporalBounds } from '../src/index';

describe('Life-First BDD Testing (CRDT Synchronization)', () => {
    it('must automatically block work assignments overlapping with family festival at Dorfwiese', () => {
        // Sept 12, 2026 10:00 to 18:00
        const dorfwieseFestival: PrivateLifeAnchor = {
            id: 'private-1',
            type: 'PRIVATE',
            description: 'Family Festival at the Dorfwiese',
            start: new Date('2026-09-12T10:00:00Z').getTime(),
            end: new Date('2026-09-12T18:00:00Z').getTime()
        };

        // Jira Ticket scheduled for 14:00 to 15:00 on the same day
        const jiraTask: WorkTask = {
            id: 'work-1',
            type: 'WORK',
            jiraId: 'PROJ-123',
            start: new Date('2026-09-12T14:00:00Z').getTime(),
            end: new Date('2026-09-12T15:00:00Z').getTime()
        };

        const result = resolveSchedule([dorfwieseFestival], [jiraTask]);

        // Assertions
        expect(result.validAnchors).toHaveLength(1);
        expect(result.validAnchors[0].id).toBe('private-1');
        
        // The work task MUST be blocked
        expect(result.validTasks).toHaveLength(0);
    });

    it('must prioritize sync with Sandra, Ryan, Lara, Melina, Elena, and Emma over work', () => {
        const familySync: PrivateLifeAnchor = {
            id: 'private-2',
            type: 'PRIVATE',
            description: 'Dedicated family synchronization with Sandra, Ryan, Lara, Melina, Elena, and Emma',
            start: new Date('2026-06-20T18:00:00Z').getTime(),
            end: new Date('2026-06-20T20:00:00Z').getTime()
        };

        const workSync: WorkTask = {
            id: 'work-2',
            type: 'WORK',
            jiraId: 'MEETING-456',
            start: new Date('2026-06-20T19:00:00Z').getTime(),
            end: new Date('2026-06-20T19:30:00Z').getTime()
        };

        const result = resolveSchedule([familySync], [workSync]);

        expect(result.validTasks).toHaveLength(0);
        expect(result.validAnchors[0].id).toBe('private-2');
    });

    it('allows work tasks in TimeGaps without overlap', () => {
        const vacation: PrivateLifeAnchor = {
            id: 'private-3',
            type: 'PRIVATE',
            description: 'Vacation to Sweden',
            start: new Date('2026-07-01T00:00:00Z').getTime(),
            end: new Date('2026-07-14T23:59:59Z').getTime()
        };

        const workBeforeVacation: WorkTask = {
            id: 'work-3',
            type: 'WORK',
            start: new Date('2026-06-30T10:00:00Z').getTime(),
            end: new Date('2026-06-30T12:00:00Z').getTime()
        };

        const result = resolveSchedule([vacation], [workBeforeVacation]);

        expect(result.validTasks).toHaveLength(1);
        expect(result.validTasks[0].id).toBe('work-3');
    });

    describe('Feature 18: Vector Clock Synchronization Schema', () => {
        it('should correctly compare vector clocks', () => {
            expect(compareVectorClocks({ A: 1 }, { A: 1 })).toBe('equal');
            expect(compareVectorClocks({ A: 2 }, { A: 1 })).toBe('newer');
            expect(compareVectorClocks({ A: 1 }, { A: 2 })).toBe('older');
            expect(compareVectorClocks({ A: 1, B: 2 }, { A: 2, B: 1 })).toBe('concurrent');
            expect(compareVectorClocks(undefined, { A: 1 })).toBe('older');
            expect(compareVectorClocks({ A: 1 }, undefined)).toBe('newer');
            expect(compareVectorClocks(undefined, undefined)).toBe('equal');
        });

        it('should resolve concurrent conflicts using deterministic tie-breaking', () => {
            const blockA: WorkTask = {
                id: 'work-conflict',
                type: 'WORK',
                start: 1000,
                end: 2000,
                vectorClock: { dev1: 1, dev2: 0 }
            };
            const blockB: WorkTask = {
                id: 'work-conflict',
                type: 'WORK',
                start: 1500,
                end: 2500,
                vectorClock: { dev1: 0, dev2: 1 }
            };

            const resolved = resolveConcurrentConflict(blockA, blockB);
            
            expect(resolved.vectorClock).toEqual({ dev1: 1, dev2: 1 });
            
            const strA = JSON.stringify(blockA);
            const strB = JSON.stringify(blockB);
            const expectedWinner = strA >= strB ? blockA : blockB;
            
            expect(resolved.id).toBe('work-conflict');
            expect(resolved.start).toBe(expectedWinner.start);
            expect(resolved.end).toBe(expectedWinner.end);
        });

        it('should deduplicate items keeping the newest or resolving conflicts', () => {
            const task1: WorkTask = {
                id: 'task-1',
                type: 'WORK',
                start: 1000,
                end: 2000,
                vectorClock: { dev1: 1 }
            };
            const task2: WorkTask = {
                id: 'task-1',
                type: 'WORK',
                start: 1000,
                end: 2050,
                vectorClock: { dev1: 2 }
            };
            const task3: WorkTask = {
                id: 'task-1',
                type: 'WORK',
                start: 1010,
                end: 2060,
                vectorClock: { dev1: 2, dev2: 1 }
            };

            const dedup1 = deduplicateByVectorClock([task1, task2]);
            expect(dedup1).toHaveLength(1);
            expect(dedup1[0].end).toBe(2050);

            const dedup2 = deduplicateByVectorClock([task2, task3]);
            expect(dedup2).toHaveLength(1);
            expect(dedup2[0].vectorClock).toEqual({ dev1: 2, dev2: 1 });
        });
    });

    describe('Feature 19: Dynamic Scheduling Policy Engine', () => {
        const policy: SchedulingPolicy = {
            timezone: 'America/New_York',
            defaultPriority: 'PRIVATE',
            rules: [
                {
                    days: [1, 2, 3, 4, 5],
                    startHour: 9,
                    startMinute: 0,
                    endHour: 17,
                    endMinute: 0,
                    priority: 'WORK'
                }
            ]
        };

        it('should get priority based on policy rules and timezone', () => {
            const earlyTime = new Date('2026-06-17T10:00:00Z').getTime(); // 6:00 AM NY time (Wed)
            expect(getPriorityAt(earlyTime, policy)).toBe('PRIVATE');

            const workTime = new Date('2026-06-17T15:00:00Z').getTime(); // 11:00 AM NY time (Wed)
            expect(getPriorityAt(workTime, policy)).toBe('WORK');

            const weekendTime = new Date('2026-06-20T15:00:00Z').getTime(); // 11:00 AM NY time (Sat)
            expect(getPriorityAt(weekendTime, policy)).toBe('PRIVATE');
        });

        it('should resolve schedule using policy to preempt lower priority blocks', () => {
            const anchor: PrivateLifeAnchor = {
                id: 'anchor-overlap',
                type: 'PRIVATE',
                description: 'Private Lunch',
                start: new Date('2026-06-17T14:30:00Z').getTime(),
                end: new Date('2026-06-17T15:30:00Z').getTime()
            };

            const task: WorkTask = {
                id: 'task-overlap',
                type: 'WORK',
                start: new Date('2026-06-17T14:45:00Z').getTime(),
                end: new Date('2026-06-17T15:15:00Z').getTime()
            };

            const resNoPolicy = resolveSchedule([anchor], [task]);
            expect(resNoPolicy.validAnchors).toHaveLength(1);
            expect(resNoPolicy.validTasks).toHaveLength(0);

            const resWithPolicy = resolveSchedule([anchor], [task], policy);
            expect(resWithPolicy.validAnchors).toHaveLength(1);
            expect(resWithPolicy.validTasks).toHaveLength(0);
            expect(resWithPolicy.validAnchors[0].id).toBe('anchor-overlap');
        });
    });

    describe('Feature 31: Strict Temporal Bounds Validation', () => {
        it('should validate temporal bounds correctly', () => {
            expect(validateTemporalBounds({ start: 1000, end: 2000 })).toBe(true);
            expect(validateTemporalBounds({ start: 2000, end: 1000 })).toBe(false);
            expect(validateTemporalBounds({ start: -5, end: 1000 })).toBe(false);
            expect(validateTemporalBounds({ start: 0, end: 1000 })).toBe(false);
            expect(validateTemporalBounds({ end: 1000 })).toBe(false);
            expect(validateTemporalBounds({ start: '1000', end: 2000 })).toBe(false);
            expect(validateTemporalBounds(null)).toBe(false);
            expect(validateTemporalBounds(undefined)).toBe(false);
            expect(validateTemporalBounds({})).toBe(false);
        });
    });
});
