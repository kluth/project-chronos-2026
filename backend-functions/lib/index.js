"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.onPrivateAnchorAdded = exports.onWorkTaskAdded = void 0;
const functions = require("firebase-functions");
const admin = require("firebase-admin");
const crdt_1 = require("./crdt");
admin.initializeApp();
const db = admin.firestore();
// Managed Agent: Conflict-Free Resolution Trigger
exports.onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
    const userId = context.params.userId;
    const newTask = snap.data();
    // Fetch all private anchors for this user
    const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
    const anchors = anchorsSnap.docs.map(doc => doc.data());
    // Fetch existing work tasks
    const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
    const tasks = tasksSnap.docs.map(doc => doc.data());
    // Run the CRDT Resolution Logic
    const resolution = (0, crdt_1.resolveSchedule)(anchors, tasks);
    // If the new task is NOT in the validTasks array, it means it was blocked.
    const isTaskValid = resolution.validTasks.some(t => t.id === newTask.id);
    if (!isTaskValid) {
        console.log(`[Chronos AI] WorkTask ${newTask.id} violates private life anchor. Deleting.`);
        await snap.ref.delete();
        // Optionally: emit a telemetry event or notify the enterprise system 
        // via Consumer-Driven Contracts that the slot is unavailable.
    }
    else {
        console.log(`[Chronos AI] WorkTask ${newTask.id} safely scheduled in a valid TimeGap.`);
    }
});
exports.onPrivateAnchorAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onCreate(async (snap, context) => {
    const userId = context.params.userId;
    const newAnchor = snap.data();
    console.log(`[Chronos AI] Private anchor ${newAnchor.id} added. Enforcing boundaries...`);
    // A new private anchor always wins. We must re-evaluate all existing work tasks.
    const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
    const anchors = anchorsSnap.docs.map(doc => doc.data());
    const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
    const tasks = tasksSnap.docs.map(doc => doc.data());
    const resolution = (0, crdt_1.resolveSchedule)(anchors, tasks);
    // Find tasks that are no longer valid and delete/reschedule them
    const validTaskIds = new Set(resolution.validTasks.map(t => t.id));
    const batch = db.batch();
    tasksSnap.docs.forEach(doc => {
        if (!validTaskIds.has(doc.data().id)) {
            console.log(`[Chronos AI] WorkTask ${doc.data().id} preempted by private anchor. Removing.`);
            batch.delete(doc.ref);
        }
    });
    await batch.commit();
});
//# sourceMappingURL=index.js.map