#include "UpdateHook.h"

#include "Diagnostics.h"
#include "ActionCapture.h"
#include "DodgeAgent.h"
#include "Effects.h"
#include "EngineState.h"
#include "GraphCoordinator.h"
#include "GraphGate.h"
#include "InputCollector.h"
#include "JumpGate.h"
#include "LockOn.h"
#include "MotionHook.h"
#include "PerfectGhost.h"
#include "Settings.h"
#include "SlowMotion.h"
#include "core/AiRequests.h"
#include <atomic>

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// wingdi.h defines GetObject as a macro, which breaks BGSDefaultObjectManager::GetObject.
#undef GetObject

namespace rdf::game::update
{
	namespace
	{
		bool PlayAction(RE::Actor* actor, RE::DEFAULT_OBJECT object)
		{
			auto* manager = RE::BGSDefaultObjectManager::GetSingleton();
			auto* action = manager ? manager->GetObject<RE::BGSAction>(object) : nullptr;
			if (!action) {
				return false;
			}
			const std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
			data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
			data->action = action;
			return data->Process();
		}

		bool IsBlocking(RE::Actor* actor)
		{
			bool blocking = false;
			actor->GetGraphVariableBool("IsBlocking"sv, blocking);
			return blocking;
		}

		// The game's left button pressed during the dodge was held back while wantBlocking was 0, so AttackBlockHandler took it
		// for a left attack (attackType kLeft, 0x1407096B0) and, still held, would send LeftPowerAttack once past the power
		// delay: a power bash out of a plain block. Our block replaces that press. (PlayerControls+0x1B8, attackType +0x28:
		// checked in IDA, the constructor 0x1407073B0 and the handler's own accesses.)
		void ClearStaleLeftPress()
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			auto* handler = controls ? controls->attackBlockHandler : nullptr;
			if (handler && handler->attackType == RE::AttackBlockHandler::AttackType::kLeft) {
				handler->attackType = RE::AttackBlockHandler::AttackType::kRight;   // 0: no attack pending
			}
		}

		// Raised block without an action record: wantBlocking + the vanilla graph event.
		bool StartBlock(RE::Actor* actor)
		{
			ClearStaleLeftPress();
			actor->AsActorState()->actorState2.wantBlocking = 1;
			if (gate::Send(actor, "blockStart"sv)) {
				return true;
			}
			actor->AsActorState()->actorState2.wantBlocking = 0;
			return false;
		}

		std::uint32_t HandleOf(RE::Actor* actor)
		{
			return actor ? actor->GetHandle().native_handle() : 0;
		}

		// RequestDodge may come from any thread (Papyrus runs on its own); the main thread drains it.
		struct Request
		{
			std::uint32_t handle;
			float         angleDeg;
		};
		std::mutex           g_requestLock;
		std::vector<Request> g_requests;

		AiRequests g_ai;
		std::atomic<bool> g_apiReady{ false };
		double RequestTime()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// A requested dodge's direction: the angle is relative to where the actor faces, +right.
		Vec2 RequestDir(RE::Actor* actor, float angleDeg)
		{
			return FromHeading(WrapAngle(actor->data.angle.z + angleDeg * kDegToRad));
		}

		// The player: keys / stick, camera, TDM, first person, block holding, jump gate, slow motion.
		class Player final : public AgentDriver
		{
		public:
			void Configure(RE::PlayerCharacter* player) { agent_ = std::make_unique<DodgeAgent>(player, this); }
			void Reset();
			void Tick(RE::PlayerCharacter* player, float dt, const std::vector<float>& requested);
			bool Ready() const { return agent_ != nullptr; }

			void OnDodgeStarted(RE::Actor* actor, bool ownsYaw) override;
			void OnDodgeStopped(RE::Actor*) override { lockon::ReleaseYaw(); }
			bool PerformAction(RE::Actor* actor, const Decision& d) override;
			bool PrepareBash(RE::Actor* actor) override;
			void OnPerfectDodge(RE::Actor*, float) override;

		private:
			AgentInput BuildInput(RE::PlayerCharacter* player);
			void       DrainInput(ReactiveCore& core, Vec2 move);
			void       MaintainBlock(RE::PlayerCharacter* player);
			void       TracePose(RE::PlayerCharacter* player) const;

			std::unique_ptr<DodgeAgent> agent_;
			float       startCameraYaw_{ 0.0f };  // camera yaw when the current dodge started
			bool        freeRotation_{ false };   // TDM 360, sampled while no dodge runs
			const char* modeName_{ "" };
			bool        blockHolding_{ false };   // a block we started out of a dodge, kept while held
			int         blockAge_{ 0 };           // ticks since that block started
		};

		// Actors other than the player dodge only when asked to (RequestDodge). Their agent lives while the dodge
		// does and is dropped once it is over, or when the actor is gone. No driver: an NPC's dodge has no
		// attack / block cancels and no player-side effects.
		class Npcs
		{
		public:
			void Request(RE::Actor* actor, float angleDeg);
			void Request(RE::Actor* actor, const AiRequests::Request& request);
			void Tick(float dt);
			void Clear();

		private:
			struct Entry
			{
				std::unique_ptr<DodgeAgent> agent;
				std::vector<float>          pending;  // requested angles, fed on the next tick
				std::optional<AiRequests::Request> immediate;
				ReactiveDodgeAPI::Ticket confirming{ 0 };
				std::uint64_t startsBefore{ 0 };
				int                         idleTicks{ 0 };
			};
			static AgentInput BuildInput(RE::Actor* actor);

			std::unordered_map<std::uint32_t, Entry> agents_;
		};

		Player g_player;
		Npcs   g_npcs;

		void Player::Reset()
		{
			if (agent_) {
				agent_->Reset();
			}
			lockon::ReleaseYaw();
			slowmo::Stop();
			blockHolding_ = false;
		}

		AgentInput Player::BuildInput(RE::PlayerCharacter* player)
		{
			const auto& settings = Settings::Get();
			const bool  running = agent_->Core().DodgeRunning();
			AgentInput  in;
			in.lockOn = lockon::LockedOn(player);
			// [Steering] bInputFromStartCamera: while a dodge runs (not locked on) the stick is read against
			// the camera yaw it started with, so the camera cannot steer it.
			const bool frozenBasis = settings.inputFromStartCamera && running && !in.lockOn;
			in.move = input::WorldMoveInput(frozenBasis ? std::optional<float>(startCameraYaw_) : std::nullopt);
			// In first person the camera owns the yaw: the dodge never turns the player there.
			const auto* camera = RE::PlayerCamera::GetSingleton();
			in.firstPerson = camera && camera->IsInFirstPerson();
			// TDM's reported mode is only meaningful outside our dodge: its movement-input flag comes from the
			// controls-driven movement handler, which does not run while the dodge drives the motion, so in
			// "Vanilla style" it reports 360 mid-dodge. A chain keeps the mode the sequence started with.
			if (!running) {
				freeRotation_ = lockon::FreeRotation();
				modeName_ = lockon::ModeName();
			}
			in.freeRotation = freeRotation_;
			in.cameraIsYaw = in.firstPerson;
			in.cameraYaw = input::CameraYaw();
			in.lookYawRate = in.firstPerson ? engine::LookTurnRate(player) * settings.firstPersonLookScale : 0.0f;
			in.trackActiveGraph = true;
			in.modeName = modeName_;
			return in;
		}

		void Player::Tick(RE::PlayerCharacter* player, float dt, const std::vector<float>& requested)
		{
			const AgentInput in = BuildInput(player);
			agent_->Tick(player, dt, in, [&](ReactiveCore& core) {
				DrainInput(core, in.move);
				for (const float angle : requested) {
					core.Press(CommandKind::Dodge, RequestDir(player, angle));
					DevLog("player: dodge requested through the API at {:.1f} deg", angle);
				}
				if (input::BlockHeld()) {
					core.KeepAlive(CommandKind::Block);
				}
				MaintainBlock(player);
			});
			const auto& core = agent_->Core();
			jump::SetLocked(Settings::Get().blockJumpDuringDodge &&
			                (agent_->AwaitingEnter() || (core.DodgeRunning() && !core.JumpWindowOpen())));
			slowmo::Update();
			if (Settings::Get().devLog && agent_->Busy()) {
				TracePose(player);
			}
		}

		void Player::DrainInput(ReactiveCore& core, Vec2 move)
		{
			input::Drain([&](const input::Press& press) {
				if (press.promote) {
					core.PromoteAttack();
					return;
				}
				const bool recorded = core.Press(press.kind, move) != 0;
				DevLog("press kind={} {}", static_cast<int>(press.kind), recorded ? "buffered" : "debounced");
			});
		}

		void Player::OnDodgeStarted(RE::Actor*, bool ownsYaw)
		{
			startCameraYaw_ = input::CameraYaw();
			if (ownsYaw) {
				lockon::AcquireYaw();  // TDM would otherwise turn the player back toward its own target yaw
			}
			// Dev only (DevLog's arguments are evaluated even with the log off). The player singleton, not a cast of
			// `actor`: As<PlayerCharacter>() returned null in a 1P crash (2026-10-22).
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (Settings::Get().devLog && camera && camera->IsInFirstPerson()) {
				DevLog("1P dodge: mouse turn rate {:.3f} rad/s x {:.2f}", engine::LookTurnRate(RE::PlayerCharacter::GetSingleton()),
					Settings::Get().firstPersonLookScale);
			}
		}

		bool Player::PerformAction(RE::Actor* actor, const Decision& d)
		{
			switch (d.kind) {
			case DecisionKind::PowerAttack:
				return PlayAction(actor, RE::DEFAULT_OBJECT::kActionRightPowerAttack);
			case DecisionKind::Block:
				blockHolding_ = StartBlock(actor);
				blockAge_ = 0;
				return blockHolding_;
			default:
				return PlayAction(actor, RE::DEFAULT_OBJECT::kActionRightAttack);
			}
		}

		// Block held (the game's block button, or a mod that raises wantBlocking itself such as Input Support) as the dodge lets
		// an attack go: that attack is a bash. A tap or a power attack decides bash or power bash, as without a dodge.
		bool Player::PrepareBash(RE::Actor* actor)
		{
			// A bow / crossbow bashes with the left button itself (an attack, replayed as such): no block to raise.
			const auto* right = actor->GetEquippedObject(false);
			const auto* weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
			if (weap && (weap->IsBow() || weap->IsCrossbow())) {
				return false;
			}
			auto&      state = actor->AsActorState()->actorState2;
			const bool key = input::BlockHeld();
			if (!key && !state.wantBlocking) {
				return false;
			}
			state.wantBlocking = 1;
			ClearStaleLeftPress();
			if (key) {   // our block afterwards: kept while the button is down (a mod's own block is the mod's)
				blockHolding_ = true;
				blockAge_ = 0;
			}
			DevLog("{}", "block held: the attack out of the dodge is a bash");
			return true;
		}

		// The game's block handler never saw a button-down that came during the dodge, so it would drop
		// our block on its next update. Keep the block up while the button stays down, end it on release.
		void Player::MaintainBlock(RE::PlayerCharacter* player)
		{
			if (!blockHolding_) {
				return;
			}
			// The graph reports IsBlocking a tick or two after blockStart; only then can "not blocking"
			// mean the game ended the block (stagger, bash, ...).
			if (++blockAge_ > 3 && !IsBlocking(player)) {
				blockHolding_ = false;
				return;
			}
			auto& state = player->AsActorState()->actorState2;
			if (input::BlockHeld()) {
				state.wantBlocking = 1;
				return;
			}
			state.wantBlocking = 0;
			gate::Send(player, "blockStop"sv);
			blockHolding_ = false;
			DevLog("{}", "block released");
		}

		void Player::OnPerfectDodge(RE::Actor*, float)
		{
			slowmo::Start();
		}

		// Dev: bone positions in the actor's own frame (last rendered pose), to see a one-frame pose pop.
		void Player::TracePose(RE::PlayerCharacter* player) const
		{
			auto* root = player->Get3D(false);
			if (!root) {
				return;
			}
			static const std::array<RE::BSFixedString, 3> kBones{ "NPC Pelvis [Pelv]", "NPC Head [Head]", "NPC R Hand [RHnd]" };
			std::array<RE::NiPoint3, kBones.size()> local{};
			const auto toLocal = root->world.rotate.Transpose();
			for (std::size_t i = 0; i < kBones.size(); ++i) {
				if (const auto* bone = root->GetObjectByName(kBones[i])) {
					local[i] = toLocal * (bone->world.translate - root->world.translate);
				}
			}
			logger::info("trace pose pelvis=({:.1f},{:.1f},{:.1f}) head=({:.1f},{:.1f},{:.1f}) rhand=({:.1f},{:.1f},{:.1f})",
				local[0].x, local[0].y, local[0].z, local[1].x, local[1].y, local[1].z, local[2].x, local[2].y, local[2].z);
		}

		// An NPC faces its combat target through the dodge (a side dodge around the enemy); without one it turns
		// to the path. The kept facing is handed back toward the target.
		AgentInput Npcs::BuildInput(RE::Actor* actor)
		{
			AgentInput in;
			in.cameraYaw = actor->data.angle.z;
			if (const auto target = actor->GetActorRuntimeData().currentCombatTarget.get()) {
				const auto from = actor->GetPosition();
				const auto to = target->GetPosition();
				const Vec2 toTarget{ to.x - from.x, to.y - from.y };
				if (Length(toTarget) > 1.0f) {
					in.lockOn = true;
					in.cameraYaw = HeadingOf(toTarget);
				}
			}
			in.freeRotation = true;
			in.modeName = in.lockOn ? "NPC: facing its combat target" : "NPC: no combat target";
			return in;
		}

		void Npcs::Request(RE::Actor* actor, float angleDeg)
		{
			auto [it, added] = agents_.try_emplace(HandleOf(actor));
			if (added) {
				it->second.agent = std::make_unique<DodgeAgent>(actor, nullptr);
				DevLog("{:08X}: dodge agent created", actor->GetFormID());
			}
			if (it->second.immediate || it->second.confirming) { return; } // v2 owns this start attempt
			it->second.pending.push_back(angleDeg);
			it->second.idleTicks = 0;
		}

		void Npcs::Request(RE::Actor* actor, const AiRequests::Request& request)
		{
			auto [it, added] = agents_.try_emplace(HandleOf(actor));
			if (added) { it->second.agent = std::make_unique<DodgeAgent>(actor, nullptr); }
			auto& entry = it->second;
			if (entry.agent->Busy() || !entry.pending.empty() || entry.immediate || entry.confirming ||
				entry.agent->Core().Buffer().Peek(CommandKind::Dodge, entry.agent->Core().Now())) {
				g_ai.Resolve(request.ticket, AiRequests::Status::Rejected, AiRequests::Reject::Rules, RequestTime());
				return;
			}
			entry.immediate = request;
			entry.idleTicks = 0;
		}

		void Npcs::Tick(float dt)
		{
			for (auto it = agents_.begin(); it != agents_.end();) {
				auto&      e = it->second;
				const auto ref = RE::TESObjectREFR::LookupByHandle(it->first);
				auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor || !actor->Is3DLoaded() || actor->IsDead()) {
					DevLog("{:08X}: dodge agent dropped (actor gone, unloaded or dead)", e.agent->FormID());
					g_ai.Resolve(e.confirming ? e.confirming : e.immediate ? e.immediate->ticket : 0,
						AiRequests::Status::Rejected, AiRequests::Reject::Unavailable, RequestTime());
					e.agent->Release(actor);
					g_ai.Publish(it->first, AiRequests::State::Idle);
					it = agents_.erase(it);
					continue;
				}
				const auto pending = std::exchange(e.pending, {});
				const auto immediate = std::exchange(e.immediate, {});
				std::uint64_t oneShotSeq = 0;
				if (immediate) {
					e.confirming = immediate->ticket;
					e.startsBefore = e.agent->ConfirmedStarts();
				}
				e.agent->Tick(actor, dt, BuildInput(actor), [&](ReactiveCore& core) {
					if (immediate) {
						core.LimitNextDodgeDistance(immediate->maxDistance);
						oneShotSeq = core.Press(CommandKind::Dodge, immediate->direction);
					}
					for (const float angle : pending) {
						core.Press(CommandKind::Dodge, RequestDir(actor, angle));
						DevLog("{:08X}: dodge requested at {:.1f} deg", actor->GetFormID(), angle);
					}
				});
				e.agent->Core().ClearNextDistanceLimit();
				e.agent->Core().DiscardPress(oneShotSeq); // an AI dodge never waits for a later cancel/resource window
				if (e.confirming) {
					if (e.agent->ConfirmedStarts() != e.startsBefore) {
						g_ai.Resolve(e.confirming, AiRequests::Status::Started, AiRequests::Reject::None, RequestTime());
						e.confirming = 0;
					} else if (!e.agent->AwaitingEnter()) {
						g_ai.Resolve(e.confirming, AiRequests::Status::Rejected,
							immediate ? AiRequests::Reject::Rules : AiRequests::Reject::Graph, RequestTime());
						e.confirming = 0;
					}
				}
				const auto& core = e.agent->Core();
				g_ai.Publish(it->first, e.agent->AwaitingEnter() ? AiRequests::State::Starting :
					core.DodgeRunning() ? (core.Exiting() ? AiRequests::State::Exiting : AiRequests::State::Dodging) : AiRequests::State::Idle);

				// A request the core refused (the actor was busy) stays buffered for its window, so the agent waits
				// that long before it is dropped.
				e.idleTicks = e.agent->Busy() ? 0 : e.idleTicks + 1;
				if (e.idleTicks > 60 && !graph::GraphIsDodging(actor)) {
					DevLog("{:08X}: dodge agent released (idle)", actor->GetFormID());
					e.agent->Release(actor);
					it = agents_.erase(it);
					continue;
				}
				++it;
			}
		}

		void Npcs::Clear()
		{
			// Load / new game: the actors may not exist any more; the global resets free the rest.
			agents_.clear();
		}

		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* player, float delta)
			{
				Tick(player, delta);
				func(player, delta);
			}

			static void Tick(RE::PlayerCharacter* player, float dt)
			{
				std::vector<Request> requests;
				{
					std::scoped_lock lock(g_requestLock);
					requests.swap(g_requests);
				}
				auto* ui = RE::UI::GetSingleton();
				if (!g_player.Ready() || !(dt > 0.0f) || !ui || ui->GameIsPaused()) {
					input::DiscardQueued();
					return;
				}
				std::vector<float> forPlayer;
				const auto         playerHandle = HandleOf(player);
				for (const auto& r : requests) {
					if (r.handle == playerHandle) {
						forPlayer.push_back(r.angleDeg);
						continue;
					}
					const auto ref = RE::TESObjectREFR::LookupByHandle(r.handle);
					auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
					if (actor && actor->Is3DLoaded() && !actor->IsDead()) {
						g_npcs.Request(actor, r.angleDeg);
					}
				}
				for (const auto& request : g_ai.Drain(RequestTime())) {
					if (!g_ai.Claim(request.ticket, RequestTime())) { continue; }
					const auto ref = RE::TESObjectREFR::LookupByHandle(request.actor);
					auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
					if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted()) {
						g_ai.Resolve(request.ticket, AiRequests::Status::Rejected, AiRequests::Reject::Unavailable, RequestTime());
						continue;
					}
					g_npcs.Request(actor, request);
				}

				// Under a magic slowdown (ours or the Slow Time shout) the engine runs the player's own updates, movement
				// included, faster than the world by this factor; the player's dodge clock follows them.
				g_player.Tick(player, dt * slowmo::PlayerDtScale(), forPlayer);
				g_npcs.Tick(dt);
				caston::UpdateChannels(dt);
				ghost::Update(dt);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Configure()
	{
		g_player.Configure(RE::PlayerCharacter::GetSingleton());
		g_apiReady.store(true, std::memory_order_release);
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = vtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
		logger::info("PlayerCharacter::Update hooked");
	}

	void Reset()
	{
		{
			std::scoped_lock lock(g_requestLock);
			g_requests.clear();
		}
		g_ai.Reset();
		g_npcs.Clear();
		g_player.Reset();
		motion::Reset();
		capture::Clear();
		graph::ClearAll();
		effects::ClearInvulnerable();
		caston::ClearChannels();
	}

	bool RequestDodge(RE::Actor* actor, float angleDeg)
	{
		if (!actor || !std::isfinite(angleDeg)) {
			return false;
		}
		const auto handle = HandleOf(actor);
		if (!handle) {
			return false;
		}
		std::scoped_lock lock(g_requestLock);
		g_requests.push_back({ handle, angleDeg });
		return true;
	}
	ReactiveDodgeAPI::Ticket RequestWorld(RE::Actor* actor, float x, float y, float maxDistance)
	{
		if (!g_apiReady.load(std::memory_order_acquire) || !actor || actor->IsPlayerRef()) { return 0; }
		return g_ai.Submit(HandleOf(actor), { x, y }, RequestTime(), maxDistance);
	}
	ReactiveDodgeAPI::Result PollRequest(ReactiveDodgeAPI::Ticket ticket, bool consume)
	{
		return g_ai.Poll(ticket, consume, RequestTime());
	}
	ReactiveDodgeAPI::ActorState ActorStatus(RE::Actor* actor) { return g_ai.GetState(HandleOf(actor)); }
	bool CancelRequest(ReactiveDodgeAPI::Ticket ticket) { return g_ai.Cancel(ticket, RequestTime()); }

}
