"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.compareVectorClocks = compareVectorClocks;
exports.resolveConcurrentConflict = resolveConcurrentConflict;
exports.deduplicateByVectorClock = deduplicateByVectorClock;
exports.getPriorityAt = getPriorityAt;
exports.noOverlap = noOverlap;
exports.resolveSchedule = resolveSchedule;
function compareVectorClocks(v1, v2) {
    const clk1 = v1 || {};
    const clk2 = v2 || {};
    const keys = new Set([...Object.keys(clk1), ...Object.keys(clk2)]);
    let greater = false;
    let lesser = false;
    for (const k of keys) {
        const val1 = clk1[k] || 0;
        const val2 = clk2[k] || 0;
        if (val1 > val2)
            greater = true;
        if (val1 < val2)
            lesser = true;
    }
    if (greater && lesser)
        return 'concurrent';
    if (greater)
        return 'newer';
    if (lesser)
        return 'older';
    return 'equal';
}
function resolveConcurrentConflict(a, b) {
    const mergedClock = {};
    const allKeys = new Set([...Object.keys(a.vectorClock || {}), ...Object.keys(b.vectorClock || {})]);
    for (const k of allKeys) {
        const valA = (a.vectorClock || {})[k] || 0;
        const valB = (b.vectorClock || {})[k] || 0;
        mergedClock[k] = Math.max(valA, valB);
    }
    const strA = JSON.stringify(a);
    const strB = JSON.stringify(b);
    const winner = strA >= strB ? a : b;
    return Object.assign(Object.assign({}, winner), { vectorClock: mergedClock });
}
function deduplicateByVectorClock(items) {
    const map = new Map();
    for (const item of items) {
        const existing = map.get(item.id);
        if (!existing) {
            map.set(item.id, item);
        }
        else {
            const comparison = compareVectorClocks(item.vectorClock, existing.vectorClock);
            if (comparison === 'newer') {
                map.set(item.id, item);
            }
            else if (comparison === 'concurrent') {
                const resolved = resolveConcurrentConflict(item, existing);
                map.set(item.id, resolved);
            }
        }
    }
    return Array.from(map.values());
}
function getPriorityAt(timestamp, policy) {
    var _a, _b, _c, _d;
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
        if (part.type === 'hour')
            localHour = parseInt(part.value, 10);
        if (part.type === 'minute')
            localMinute = parseInt(part.value, 10);
        if (part.type === 'weekday')
            weekdayStr = part.value;
    }
    const localMinutes = localHour * 60 + localMinute;
    const weekdays = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    const localDay = weekdays.indexOf(weekdayStr);
    for (const rule of policy.rules) {
        if (rule.days && !rule.days.includes(localDay)) {
            continue;
        }
        const startMin = ((_a = rule.startHour) !== null && _a !== void 0 ? _a : 0) * 60 + ((_b = rule.startMinute) !== null && _b !== void 0 ? _b : 0);
        const endMin = ((_c = rule.endHour) !== null && _c !== void 0 ? _c : 24) * 60 + ((_d = rule.endMinute) !== null && _d !== void 0 ? _d : 0);
        if (localMinutes >= startMin && localMinutes <= endMin) {
            return rule.priority;
        }
    }
    return policy.defaultPriority;
}
function noOverlap(t1, t2) {
    return t1.end <= t2.start || t2.end <= t1.start;
}
function resolveSchedule(anchors, tasks, policy) {
    const uniqueAnchors = deduplicateByVectorClock(anchors);
    const uniqueTasks = deduplicateByVectorClock(tasks);
    if (!policy) {
        const validAnchors = [...uniqueAnchors];
        const validTasks = uniqueTasks.filter(task => {
            return uniqueAnchors.every(anchor => noOverlap(anchor, task));
        });
        return { validAnchors, validTasks };
    }
    const preemptedAnchors = new Set();
    const preemptedTasks = new Set();
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
//# sourceMappingURL=crdt.js.map