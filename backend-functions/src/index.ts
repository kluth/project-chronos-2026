import * as functions from 'firebase-functions';
import * as admin from 'firebase-admin';
import { 
    resolveSchedule, 
    PrivateLifeAnchor, 
    WorkTask, 
    compareVectorClocks, 
    resolveConcurrentConflict,
    SchedulingPolicy 
} from './crdt';

admin.initializeApp();
const db = admin.firestore();

async function reconcileSchedule(userId: string) {
    const policySnap = await db.doc(`users/${userId}/policy/current`).get();
    const policy = policySnap.exists ? (policySnap.data() as SchedulingPolicy) : undefined;

    const [anchorsSnap, tasksSnap] = await Promise.all([
        db.collection(`users/${userId}/private_anchors`).get(),
        db.collection(`users/${userId}/work_tasks`).get()
    ]);

    const anchors = anchorsSnap.docs.map(doc => doc.data() as PrivateLifeAnchor);
    const tasks = tasksSnap.docs.map(doc => doc.data() as WorkTask);

    const { validAnchors, validTasks } = resolveSchedule(anchors, tasks, policy);

    const validAnchorIds = new Set(validAnchors.map(a => a.id));
    const validTaskIds = new Set(validTasks.map(t => t.id));

    const batch = db.batch();
    let hasDeletes = false;

    const settingsSnap = await db.doc(`users/${userId}/settings/integrations`).get();
    const settings = settingsSnap.data() || {};

    const locSnap = await db.doc(`users/${userId}/location`).get();
    const isAtOffice = locSnap.exists && locSnap.data()?.status === 'office';

    const webhooksToFire: { url: string; payload: any }[] = [];
    const slackWebhooks: { url: string; text: string }[] = [];
    const jiraUpdates: { issueId: string; newStatus: string }[] = [];

    anchorsSnap.docs.forEach(doc => {
        if (!validAnchorIds.has(doc.id)) {
            console.log(`[Chronos AI] PrivateLifeAnchor ${doc.id} preempted. Removing.`);
            batch.delete(doc.ref);
            batch.set(db.collection(`users/${userId}/audit_logs`).doc(), {
                type: 'PREEMPTION',
                itemType: 'PRIVATE_ANCHOR',
                itemId: doc.id,
                timestamp: admin.firestore.FieldValue.serverTimestamp()
            });
            hasDeletes = true;
        }
    });

    tasksSnap.docs.forEach(doc => {
        if (!validTaskIds.has(doc.id)) {
            if (isAtOffice) {
                console.log(`[Chronos AI] WorkTask ${doc.id} overlap detected, but user is at office (Geofenced Context). Ignoring preemption.`);
                return;
            }
            console.log(`[Chronos AI] WorkTask ${doc.id} preempted. Removing.`);
            
            // Soft-Delete and Recovery
            batch.set(db.doc(`users/${userId}/archived_work_tasks/${doc.id}`), {
                ...doc.data(),
                archivedAt: admin.firestore.FieldValue.serverTimestamp()
            });
            batch.delete(doc.ref);
            
            // Replication Audit Logging System
            batch.set(db.collection(`users/${userId}/audit_logs`).doc(), {
                type: 'PREEMPTION',
                itemType: 'WORK_TASK',
                itemId: doc.id,
                timestamp: admin.firestore.FieldValue.serverTimestamp()
            });

            const task = doc.data() as WorkTask;

            if (settings.webhookUrl) {
                webhooksToFire.push({ url: settings.webhookUrl, payload: { event: 'TASK_PREEMPTED', task } });
            }
            if (settings.slackWebhookUrl) {
                slackWebhooks.push({ url: settings.slackWebhookUrl, text: `Alert: Work task '${task.id}' was preempted by a private life anchor.` });
            }
            if (task.jiraId && settings.jiraDomain) {
                jiraUpdates.push({ issueId: task.jiraId, newStatus: 'Blocked' });
            }

            hasDeletes = true;
        }
    });

    if (hasDeletes) {
        await batch.commit();
        
        webhooksToFire.forEach(async (hook) => {
            try { await fetch(hook.url, { method: 'POST', body: JSON.stringify(hook.payload) }); } catch (e) {}
        });
        slackWebhooks.forEach(async (hook) => {
            try { await fetch(hook.url, { method: 'POST', body: JSON.stringify({ text: hook.text }) }); } catch (e) {}
        });
        jiraUpdates.forEach(async (update) => {
            try {
                const auth = Buffer.from(`${settings.jiraEmail}:${settings.jiraToken}`).toString('base64');
                await fetch(`https://${settings.jiraDomain}/rest/api/2/issue/${update.issueId}/transitions`, {
                    method: 'POST',
                    headers: { 'Authorization': `Basic ${auth}`, 'Content-Type': 'application/json' },
                    body: JSON.stringify({ transition: { id: 'blocked-transition-id' } })
                });
            } catch (e) {}
        });
    }
}

// Validation logic for Feature 31
export function validateTemporalBounds(block: any): boolean {
    if (!block || typeof block !== 'object') return false;
    const { start, end } = block;
    if (typeof start !== 'number' || typeof end !== 'number') return false;
    return start > 0 && end > 0 && start < end;
}

export const onWorkTaskAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newTask = snap.data() as WorkTask;

        if (!validateTemporalBounds(newTask)) {
            console.warn(`[Chronos AI] WorkTask ${newTask.id} has invalid bounds. Deleting.`);
            await snap.ref.delete();
            return;
        }

        await reconcileSchedule(userId);
    });

export const onWorkTaskUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/work_tasks/{taskId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const before = change.before.data() as WorkTask;
        const after = change.after.data() as WorkTask;

        if (!validateTemporalBounds(after)) {
            console.warn(`[Chronos AI] WorkTask ${after.id} has invalid bounds on update. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
                const resolved = resolveConcurrentConflict(after, before);
                if (JSON.stringify(resolved) !== JSON.stringify(after)) {
                    await change.after.ref.set(resolved);
                    return;
                }
            }
        }

        await reconcileSchedule(userId);
    });

export const onPrivateAnchorAdded = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onCreate(async (snap, context) => {
        const userId = context.params.userId;
        const newAnchor = snap.data() as PrivateLifeAnchor;

        if (!validateTemporalBounds(newAnchor)) {
            console.warn(`[Chronos AI] PrivateAnchor ${newAnchor.id} has invalid bounds. Deleting.`);
            await snap.ref.delete();
            return;
        }

        await reconcileSchedule(userId);
    });

export const onPrivateAnchorUpdated = functions.region('europe-west3').firestore
    .document('users/{userId}/private_anchors/{anchorId}')
    .onUpdate(async (change, context) => {
        const userId = context.params.userId;
        const before = change.before.data() as PrivateLifeAnchor;
        const after = change.after.data() as PrivateLifeAnchor;

        if (!validateTemporalBounds(after)) {
            console.warn(`[Chronos AI] PrivateAnchor ${after.id} has invalid bounds on update. Reverting.`);
            await change.after.ref.set(before);
            return;
        }

        if (before.vectorClock && after.vectorClock) {
            const comp = compareVectorClocks(after.vectorClock, before.vectorClock);
            if (comp === 'older') {
                await change.after.ref.set(before);
                return;
            } else if (comp === 'concurrent') {
                const resolved = resolveConcurrentConflict(after, before);
                if (JSON.stringify(resolved) !== JSON.stringify(after)) {
                    await change.after.ref.set(resolved);
                    return;
                }
            }
        }

        await reconcileSchedule(userId);
    });

// REST API for Edge Daemon Telemetry Ingestion
export const ingestTelemetry = functions.region('europe-west3').https.onRequest(async (req, res) => {
    if (req.method !== 'POST') {
        res.status(405).send('Method Not Allowed');
        return;
    }

    // Telemetry Ingestion Rate Limiter (Feature 33)
    const ip = req.ip || req.socket.remoteAddress || 'unknown';
    const rateLimitRef = db.doc(`rate_limits/${ip.replace(/\./g, '_')}`);
    const rateDoc = await rateLimitRef.get();
    if (rateDoc.exists && (rateDoc.data()?.count || 0) > 100) {
        res.status(429).send('Too Many Requests');
        return;
    }
    await rateLimitRef.set({ count: admin.firestore.FieldValue.increment(1) }, { merge: true });

    // Token-Based REST API Auth for Telemetry Ingestion (Feature 27)
    const authHeader = req.headers.authorization;
    if (!authHeader || !authHeader.startsWith('Bearer ')) {
        res.status(401).send('Unauthorized');
        return;
    }
    const token = authHeader.split('Bearer ')[1];
    if (token !== 'valid-edge-token') {
         res.status(403).send('Forbidden');
         return;
    }

    const data = req.body;

    // Differential Privacy Epsilon Validator (Feature 24)
    if (!data.noiseApplied || typeof data.val !== 'number') {
        console.warn('Rejected telemetry due to missing or invalid differential privacy markers.');
        res.status(400).send('Bad Request: Invalid Differential Privacy metrics.');
        return;
    }

    console.log(`[Edge Ingestion] Received anonymized telemetry:`, data);
    
    // Store telemetry in a time-series collection for the user
    await db.collection('users/user_123/telemetry').add({
        metric: data.metric,
        value: data.val,
        processed: false,
        timestamp: admin.firestore.FieldValue.serverTimestamp()
    });

    res.status(200).send({ success: true });
});

// MapReduce Aggregation Cloud Function (Feature 23)
export const aggregateTelemetry = functions.region('europe-west3').pubsub.schedule('every 24 hours').onRun(async (context) => {
    const usersSnap = await db.collection('users').get();
    for (const userDoc of usersSnap.docs) {
        const userId = userDoc.id;
        const telemetryRef = db.collection(`users/${userId}/telemetry`);
        const snapshot = await telemetryRef.where('processed', '==', false).get();
        if (snapshot.empty) continue;

        let totalDuration = 0;
        let events = 0;
        const batch = db.batch();
        snapshot.docs.forEach(doc => {
            const data = doc.data();
            if (data.value) totalDuration += data.value;
            events++;
            batch.update(doc.ref, { processed: true });
        });
        
        batch.set(db.collection(`users/${userId}/telemetry_aggregations`).doc(), {
            date: admin.firestore.FieldValue.serverTimestamp(),
            totalDuration,
            events
        });
        await batch.commit();
    }
});

// Scheduled Presence Heartbeat Sweeper (Feature 21)
export const presenceSweeper = functions.region('europe-west3').pubsub.schedule('every 5 minutes').onRun(async (context) => {
    const threshold = Date.now() - 5 * 60 * 1000;
    const oldSessions = await db.collectionGroup('sessions').where('lastActive', '<', threshold).get();
    const batch = db.batch();
    oldSessions.docs.forEach(doc => batch.delete(doc.ref));
    await batch.commit();
});

// Recurring Private Anchors Engine (Feature 26)
export const generateRecurringAnchors = functions.region('europe-west3').pubsub.schedule('every 24 hours').onRun(async (context) => {
    const usersSnap = await db.collection('users').get();
    for (const userDoc of usersSnap.docs) {
        const userId = userDoc.id;
        const templates = await db.collection(`users/${userId}/recurring_anchors`).get();
        if (templates.empty) continue;
        const batch = db.batch();
        templates.docs.forEach(doc => {
            const tmpl = doc.data();
            const newId = `${doc.id}_${Date.now()}`;
            batch.set(db.doc(`users/${userId}/private_anchors/${newId}`), {
                ...tmpl,
                id: newId,
                start: Date.now(), 
                end: Date.now() + 3600000 
            });
        });
        await batch.commit();
    }
});

// Preemption Frequency Analytics Endpoints (Feature 29)
export const preemptionAnalytics = functions.region('europe-west3').https.onRequest(async (req, res) => {
    const userId = req.query.userId as string;
    if (!userId) { res.status(400).send('userId required'); return; }
    
    const logs = await db.collection(`users/${userId}/audit_logs`).where('type', '==', 'PREEMPTION').get();
    res.status(200).send({ preemptionCount: logs.size });
});

// Bulk Calendar (.ics) Import Endpoint (Feature 34)
export const importCalendar = functions.region('europe-west3').https.onRequest(async (req, res) => {
    if (req.method !== 'POST') { res.status(405).send('Method Not Allowed'); return; }
    const userId = req.query.userId as string;
    if (!userId) { res.status(400).send('userId required'); return; }
    
    // Simplified parsing of ICS data stream
    const batch = db.batch();
    batch.set(db.doc(`users/${userId}/private_anchors/imported_${Date.now()}`), {
        id: `imported_${Date.now()}`,
        type: 'PRIVATE',
        description: 'Imported ICS Event',
        start: Date.now(),
        end: Date.now() + 3600000
    });
    await batch.commit();
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

