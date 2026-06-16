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

export function getPriorityAt(timestamp: number, policy: SchedulingPolicy): 'PRIVATE' | 'WORK' {
    const timezone = policy.timezone || 'UTC';
    
    const parts = new Intl.DateTimeFormat('en-US', {
        timeZone: timezone,
        hour: 'numeric',
        minute: 'numeric',
        hourCycle: 'h23',
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
                // PRIVATE LIFE HAS UNCOMPROMISING PRIORITY
                preemptedTasks.add(task.id);
            }
        }
    }

    const validAnchors = uniqueAnchors.filter(a => !preemptedAnchors.has(a.id));
    const validTasks = uniqueTasks.filter(t => !preemptedTasks.has(t.id));

    return { validAnchors, validTasks };
}
