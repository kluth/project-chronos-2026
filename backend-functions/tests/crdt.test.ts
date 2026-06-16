import { PrivateLifeAnchor, WorkTask, resolveSchedule } from '../src/crdt';

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
});
