---------------- MODULE LifeCentricScheduler ----------------
EXTENDS Naturals, FiniteSets

VARIABLES private_anchors, work_tasks

TypeOK == 
    /\ private_anchors \in SUBSET [start: Nat, end: Nat]
    /\ work_tasks \in SUBSET [start: Nat, end: Nat]

Init == 
    /\ private_anchors = {}
    /\ work_tasks = {}

NoOverlap(t1, t2) ==
    t1.end <= t2.start \/ t2.end <= t1.start

ZeroOverlapGuarantee ==
    \A p \in private_anchors, w \in work_tasks : NoOverlap(p, w)

AddPrivateAnchor(p) ==
    /\ p.start < p.end
    /\ private_anchors' = private_anchors \cup {p}
    /\ work_tasks' = {w \in work_tasks : NoOverlap(p, w)}

AddWorkTask(w) ==
    /\ w.start < w.end
    /\ \A p \in private_anchors : NoOverlap(p, w)
    /\ work_tasks' = work_tasks \cup {w}
    /\ UNCHANGED private_anchors

Next == 
    \/ \E p \in [start: Nat, end: Nat] : AddPrivateAnchor(p)
    \/ \E w \in [start: Nat, end: Nat] : AddWorkTask(w)

Spec == Init /\ [][Next]_<<private_anchors, work_tasks>>

THEOREM Spec => []ZeroOverlapGuarantee
=============================================================
