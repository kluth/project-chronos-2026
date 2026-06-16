"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.noOverlap = noOverlap;
exports.resolveSchedule = resolveSchedule;
function noOverlap(t1, t2) {
    return t1.end <= t2.start || t2.end <= t1.start;
}
function resolveSchedule(anchors, tasks) {
    // CRDT Resolution Rule: PrivateLifeAnchor strictly wins.
    const validAnchors = [...anchors];
    // Filter out any work tasks that conflict with ANY private life anchor
    const validTasks = tasks.filter(task => {
        return anchors.every(anchor => noOverlap(anchor, task));
    });
    return { validAnchors, validTasks };
}
//# sourceMappingURL=crdt.js.map