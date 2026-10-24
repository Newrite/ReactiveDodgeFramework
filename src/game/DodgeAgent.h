#pragma once

#include "CastOn.h"
#include "GraphCoordinator.h"
#include "core/ReactiveCore.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace rdf::game
{
	// What drives an agent's dodge this tick. The player's controller fills it from the keys / stick, the camera and
	// TDM; a dodge requested for an NPC through the API fills it from the NPC itself.
	struct AgentInput
	{
		Vec2        move{};               // world move input, length 0..1 (none for an NPC: its dodge is not steered)
		bool        lockOn{ false };      // keep facing a target (player: TDM target lock; NPC: has a combat target)
		bool        freeRotation{ true };  // may turn to the path (player: TDM 360; NPC: yes)
		bool        cameraIsYaw{ false };  // first person: the actor's yaw is the view
		float       cameraYaw{ 0.0f };     // where a kept facing is handed back to at the end
		float       lookYawRate{ 0.0f };   // first-person mouse turn, rad/s per lookInputVec.x
		bool        firstPerson{ false };  // first-person clips and stance
		bool        trackActiveGraph{ false };  // the actor has several graphs (the player): count the active one
		const char* modeName{ "" };        // diagnostics: who owns the facing
	};

	// The side effects that belong to the driver, not to the dodge (all main thread).
	class AgentDriver
	{
	public:
		virtual ~AgentDriver() = default;
		virtual void OnDodgeStarted(RE::Actor*, bool /*ownsYaw*/) {}
		virtual void OnDodgeStopped(RE::Actor*) {}
		// Perform an attack / block decision the dodge was cancelled into. False = it did not happen (retried).
		virtual bool PerformAction(RE::Actor*, const Decision&) { return false; }
		// Right before an attack the dodge was cancelled into: true = the block is held, so the attack is raised as a bash
		// (wantBlocking set: the idle tree turns an attack into bashStart, a power attack into bashPowerStart).
		virtual bool PrepareBash(RE::Actor*) { return false; }
		virtual void OnPerfectDodge(RE::Actor*, float /*actionTime*/) {}
	};

	// One actor's dodge: the core, the graph handshake, clip and stance choice, motion, cost, i-frames, perfect dodge.
	// The same for the player and for NPCs. Main thread.
	class DodgeAgent
	{
	public:
		DodgeAgent(RE::Actor* actor, AgentDriver* driver);
		~DodgeAgent();

		// `feed` runs after the frame began, to push this tick's presses into the core.
		void Tick(RE::Actor* actor, float dt, const AgentInput& in, const std::function<void(ReactiveCore&)>& feed);
		void Reset();
		// Abandons whatever runs (actor unloaded / gone) and frees the per-actor engine state.
		void Release(RE::Actor* actor);

		ReactiveCore&       Core() { return *core_; }
		const ReactiveCore& Core() const { return *core_; }
		bool                AwaitingEnter() const { return awaitingEnter_; }
		bool                Busy() const;  // dodging, entering, or still carrying its speed into running
		std::uint32_t       FormID() const { return formID_; }

	private:
		struct Retry
		{
			Decision decision;
			bool     pending{ false };
		};

		void              RefreshConfig();
		void              Refresh3D(RE::Actor* actor);
		void              MeasureMotion(RE::Actor* actor, float dt);
		FrameInput        BuildFrame(RE::Actor* actor, float dt, const AgentInput& in);
		void              HandleGraphEvents(RE::Actor* actor);
		void              ReadAttack(RE::Actor* actor, std::uint8_t attackState, EngineView& view);
		void              Execute(RE::Actor* actor, const Decision& d, const AgentInput& in);
		graph::DodgeEntry EntryFor(RE::Actor* actor, bool chain) const;
		void              TryStart(RE::Actor* actor, const Decision& d, const AgentInput& in);
		void              CancelInto(RE::Actor* actor, const Decision& d);
		bool              CancelInPlace(RE::Actor* actor, const Decision& d);
		void              GateGraphEvents(RE::Actor* actor);
		void              HoldReleases(RE::Actor* actor, float dt);
		void              TriggerCastOn(RE::Actor* actor, caston::Trigger trigger, RE::Actor* attacker);
		void              DrainCastOnAnnotations(RE::Actor* actor);
		void              ApplyClipMotion(RE::Actor* actor);
		// [Cost] what pays a dodge of `actor` now: sResource, or the resource keywords' choice (both: half each).
		struct CostResources
		{
			std::array<RE::ActorValue, 2> values{};
			int                           count{ 0 };
		};
		CostResources     ResourcesOf(RE::Actor* actor) const;
		float             CostOf(RE::Actor* actor, int chainIndex, bool escape) const;
		bool              CostAllowed(RE::Actor* actor, float cost) const;
		void              Charge(RE::Actor* actor, float cost);
		void              Refund(RE::Actor* actor, float amount);
		bool              PerfectKeywordHeld(RE::Actor* actor) const;
		graph::Layer      CastingLayer(RE::Actor* actor, std::uint8_t attackState) const;
		bool              StaggerEscape(RE::Actor* actor);
		void              RetryDeferred(RE::Actor* actor);
		void              VerifyAttack(RE::Actor* actor, float dt, std::uint8_t attackState);
		void              TrackAttackFromDodge(RE::Actor* actor);
		void              ClearAttackFromDodge(RE::Actor* actor);
		void              RollbackStart(RE::Actor* actor);
		void              EndByGraph(const char* why);
		void              AbandonGraph(RE::Actor* actor, const char* why);
		void              ReleaseOrphanedState(RE::Actor* actor, float dt);
		void              HandleDodgedHits(RE::Actor* actor);
		void              StepAndPublish(RE::Actor* actor, float dt, const AgentInput& in);
		void              OnStopped(RE::Actor* actor);
		void              Trace(RE::Actor* actor);
		void              LogRejection(const Decision& d);
		void              LogCost(RE::Actor* actor);
		static int        ClipIndex(float angleDeg, bool keepFacing, bool neutral);

		std::unique_ptr<ReactiveCore> core_;
		AgentDriver*                  driver_;
		std::uint32_t                 formID_{ 0 };
		std::uint32_t                 seenGeneration_{ 0 };

		// graph handshake
		bool  awaitingEnter_{ false };
		int   enterFrames_{ 0 };
		bool  exitSent_{ true };
		bool  wasRunning_{ false };
		int   prevDepth_{ 0 };
		Retry retry_;
		// an attack the dodge was cancelled into, until the actor's attack state shows it
		struct PendingAttack
		{
			Decision decision;
			float    age{ 0.0f };
			float    lastTry{ 0.0f };
			int      attempts{ 0 };  // 0 = not performed yet (the graph is still in the dodge)
		};
		std::optional<PendingAttack> pendingAttack_;
		// RDF_AttackFromDodge is set for the attack the dodge became
		struct FromDodge
		{
			bool          active{ false };   // the variable is set
			bool          started{ false };  // the attack shows (IsAttacking)
			std::uint32_t starts{ 0 };       // attack starts seen at the cancel (before that attack)
		};
		FromDodge fromDodge_;
		int       lastClip_{ 0 };  // RDF_DodgeClip of the current / last dodge

		// bookkeeping
		RE::NiAVObject* last3D_{ nullptr };
		float           charged_{ 0.0f };
		RejectReason    lastReject_{ RejectReason::None };
		CommandKind     lastRejectCommand_{ CommandKind::Dodge };
		float           orphanTime_{ 0.0f };      // graph in a dodge state we do not own, seconds
		float           staggerAge_{ 0.0f };      // seconds the actor has been staggered
		float           clipLength_{ 0.667f };    // the dodge clip's length: fClipLength*, or its RDF_ClipLength
		float           clipSpeed_{ 1.0f };       // its playback multiplier: fAnimSpeed, or its RDF_Speed
		bool            stanceKept_{ false };
		CostResources   chargedFrom_{};  // what charged_ was taken from (refunds go back there)
		std::uint64_t   flashedPress_{ 0 };  // the refused dodge press whose meters already flashed
		// this dodge's keyword amounts, read as it starts: window changes (s) and whether it may be perfect
		float           iframeDelta_{ 0.0f };
		float           perfectDelta_{ 0.0f };
		bool            perfectAllowed_{ true };     // the dodge keeps a weapon stance on the arms (RDF_ArmStance applies)
		bool            escapeStagger_{ false };  // this tick a dodge may cut the stagger short
		const char*     lastStaggerRule_{ nullptr };  // dev log: the [Stagger] rule last reported
		bool            perfectFired_{ false };   // the current dodge already counted a perfect dodge
		std::uint8_t    prevAttackState_{ 0 };    // vanilla attack state last tick (a new attack passes into Draw)
		std::string     lastAttackLog_;           // dev log: the attack-cancel inputs last printed
		int             tailFrames_{ 0 };         // dev trace keeps running this many ticks after a dodge
		bool            inStateB_{ true };        // dodge state used last; the first dodge goes to A
		graph::Layer    layer_{ graph::Layer::Root };  // where the current / last dodge plays
		bool            oneHandBroken_{ false };  // a 1hm start never entered: root dodges until the 3D reloads
		bool            layerBroken_{ false };    // ... a bow / magic start never entered: no layer dodges until then
		bool            releasesHeld_{ false };   // [Casting] release actions wait for this dodge's end
		caston::Scheduler castOn_;                // [CastOn] the rules this actor's dodges trigger
		int             lastChainIndex_{ 0 };     // ... the facts of the current dodge, for a PerfectDodge trigger
		bool            lastFromStagger_{ false };
		bool            lastFirstPerson_{ false };
		float           releaseRetry_{ 0.0f };    // ... and refused ones are tried again for this long after it, seconds
		bool            moveHeld_{ false };
		bool            intoAttack_{ false };     // the current dodge was cancelled into an attack (runs on when moving)
		float           carry_{ 0.0f };           // post-dodge speed floor for the controls-driven engine, u/s

		// measured motion (previous frame's displacement, seen at the start of this tick)
		bool  havePos_{ false };
		Vec2  lastPos_{};
		float measuredSpeed_{ 0.0f };
		float measuredDistance_{ 0.0f };
		float measuredHeading_{ 0.0f };

		// what we asked the hook for: this tick's request, and last tick's (which is what the
		// displacement measured at the start of this tick should match)
		float requestedDistance_{ 0.0f };
		float requestedHeading_{ 0.0f };
		float prevRequestedDistance_{ 0.0f };
		float prevRequestedHeading_{ 0.0f };
	};
}
