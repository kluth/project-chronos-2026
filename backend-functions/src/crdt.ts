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

export function noOverlap(t1: TimeBlock, t2: TimeBlock): boolean {
    return t1.end <= t2.start || t2.end <= t1.start;
}

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
