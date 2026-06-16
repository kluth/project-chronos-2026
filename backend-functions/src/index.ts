import * as functions from 'firebase-functions';
import * as admin from 'firebase-admin';
import { resolveSchedule, PrivateLifeAnchor, WorkTask } from './crdt';

admin.initializeApp();
const db = admin.firestore();

// Managed Agent: Conflict-Free Resolution Trigger
export const onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newTask = snap.data() as WorkTask;

        // Fetch all private anchors for this user
        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
        
        // Fetch existing work tasks
        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        // Run the CRDT Resolution Logic
        const resolution = resolveSchedule(anchors, tasks);

        // If the new task is NOT in the validTasks array, it means it was blocked.
        const isTaskValid = resolution.validTasks.some(t => t.id === newTask.id);

        if (!isTaskValid) {
            console.log(`[Chronos AI] WorkTask ${newTask.id} violates private life anchor. Deleting.`);
            await snap.ref.delete();
            
            // Optionally: emit a telemetry event or notify the enterprise system 
            // via Consumer-Driven Contracts that the slot is unavailable.
        } else {
            console.log(`[Chronos AI] WorkTask ${newTask.id} safely scheduled in a valid TimeGap.`);
        }
    });

export const onPrivateAnchorAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newAnchor = snap.data() as PrivateLifeAnchor;

        console.log(`[Chronos AI] Private anchor ${newAnchor.id} added. Enforcing boundaries...`);

        const anchorsSnap = await db.collection(`users/${userId}/private_anchors`).get();
        const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);

        const tasksSnap = await db.collection(`users/${userId}/work_tasks`).get();
        const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

        const resolution = resolveSchedule(anchors, tasks);

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

// REST API for Edge Daemon Telemetry Ingestion
export const ingestTelemetry = functions.region('europe-west3').https.onRequest(async (req, res) => {
    if (req.method !== 'POST') {
        res.status(405).send('Method Not Allowed');
        return;
    }
    const data = req.body;
    console.log(`[Edge Ingestion] Received anonymized telemetry:`, data);
    
    // Store telemetry in a time-series collection for the user
    await db.collection('users/user_123/telemetry').add({
        metric: data.metric,
        value: data.val,
        timestamp: admin.firestore.FieldValue.serverTimestamp()
    });

    res.status(200).send({ success: true });
});

// REST API to instantly seed the database and test the CRDT overlap
export const seedDatabase = functions.region('europe-west3').https.onRequest(async (req, res) => {
    const userId = "user_123";
    
    // 1. Add a Private Anchor (Family Festival)
    const anchorRef = db.collection(`users/${userId}/private_anchors`).doc('family_fest');
    await anchorRef.set({
        id: 'family_fest',
        type: 'PRIVATE',
        description: 'Family Festival at Dorfwiese',
        start: 100, // Normalized logical time start
        end: 300    // Normalized logical time end
    });

    // 2. Add an overlapping Work Task (Jira sync)
    const workRef = db.collection(`users/${userId}/work_tasks`).doc('jira_sync');
    await workRef.set({
        id: 'jira_sync',
        type: 'WORK',
        jiraId: 'PROJ-123',
        start: 200, // Overlaps with the festival!
        end: 250
    });

    res.status(200).send({ 
        message: 'Database seeded! Check the Firebase Functions logs to see the WorkTask get automatically deleted by the CRDT logic.',
        frontend: 'Check https://project-chronos-2026.web.app to see the Canvas updated!'
    });
});

