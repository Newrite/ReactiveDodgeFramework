Scriptname ReactiveDodge Hidden

; Reactive Dodge Framework.
; Makes akActor dodge at afAngle degrees relative to where it faces: 0 forward, 90 right, 180 back, -90 left.
; The dodge is the player's (clips, windows, chains, cost, i-frames, perfect dodge); the decision is the caller's.
; Returns false if the request could not be queued. The dodge itself starts on the next frame if the actor can.
; Console: cgf "ReactiveDodge.Dodge" <ref> 90
bool Function Dodge(Actor akActor, float afAngle = 180.0) global native
