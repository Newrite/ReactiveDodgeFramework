#pragma once

#include "core/Config.h"

#include <optional>
#include <string>
#include <vector>

namespace rdf::game
{
	struct GraphNames
	{
		// events sent to the graph
		std::string start{ "RDF_DodgeStart" };    // root wildcard into dodge state A
		std::string startB{ "RDF_DodgeStartB" };  // root wildcard into dodge state B
		std::string chain{ "RDF_DodgeChain" };
		std::string exit{ "RDF_DodgeExit" };
		// the dodge inside 1hm_behavior (BEHAVIOR_INTEGRATION.md 7): tried first while 1hm runs (third person); a dodge
		// there leaves straight into the attack / block / bash the graph is sent, like 1HM_Ready_State
		bool        oneHand{ true };
		std::string start1H{ "RDF_DodgeStart1H" };
		std::string startB1H{ "RDF_DodgeStartB1H" };
		// graph events other mods send directly, held back while the dodge may not be left: "event" or "event|cancelEvent"
		std::vector<std::string> holdEvents{ "blockStart|blockStop" };
		// events reported by the graph
		std::string entered{ "RDF_DodgeEntered" };
		std::string left{ "RDF_DodgeLeft" };
		// variables: clip and speed are written by us, isDodging is only read (set by the graph)
		std::string clip{ "RDF_DodgeClip" };
		std::string speed{ "RDF_DodgeSpeed" };
		std::string armBlend{ "RDF_ArmBlend" };  // weight of the arm-stance layer over the dodge clip
		std::string isDodging{ "RDF_IsDodging" };
		std::string chainIndex{ "RDF_ChainIndex" };  // which dodge of a chain: 0 first, 1 second, ...; 0 when none runs
		// the attack playing came out of a dodge (1 attack, 2 power attack; 0 otherwise) and that dodge's clip index:
		// for OAR conditions on attack clips (a dodge attack)
		std::string attackFromDodge{ "RDF_AttackFromDodge" };
		std::string attackFromDodgeClip{ "RDF_AttackFromDodgeClip" };
		std::vector<std::string> windowOpen{ "MCO_DodgeOpen" };
		std::vector<std::string> windowClose{ "MCO_DodgeClose" };
	};

	struct Settings
	{
		Config core;

		// [Input]: raw device scan codes, -1 = unbound
		int   dodgeKeyboard{ RE::BSKeyboardDevice::Keys::kV };
		int   dodgeMouse{ -1 };
		int   dodgeGamepad{ RE::BSWin32GamepadDevice::Keys::kRightThumb };  // XInput mask
		float powerAttackHold{ 0.30f };

		// [Cost]
		// Resource a dodge costs; nullopt = free.
		std::optional<RE::ActorValue> costResource{ RE::ActorValue::kStamina };
		float                         cost{ 20.0f };
		float                         minimum{ 10.0f };  // refuse below this much of costResource
		// After a dodge hands over into running, the engine re-accelerates from below the dodge's exit
		// speed; its speed may not drop under the exit speed decaying at this rate (u/s^2, 0 = off).
		float carryDecay{ 1200.0f };

		// [Graph]
		GraphNames graph;
		float      animSpeed{ 1.0f };
		float      clipLength{ 0.667f };  // seconds of the dodge clip at speed 1
		// Share of the current weapon stance on the arms during a dodge (0..1): steps keep the guard,
		// rolls let the clip move the arms (a body turning over with arms frozen in a stance looks wrong).
		float      armStanceStep{ 1.0f };
		float      armStanceRoll{ 0.0f };
		// First person plays Bethesda's 1P dodge clips (0.9 s) on torso, head and camera; this is the
		// share of the weapon-ready pose on the arms (1 = the grip stays on the weapon).
		float      armStanceFirstPerson{ 1.0f };
		float      clipLengthFirstPerson{ 0.9f };
		float      firstPersonLookScale{ 1.0f };  // mouse turn during a 1P dodge, x the engine's own rate
		bool       armStanceWhenSheathed{ false };  // also keep the stance of a sheathed weapon / spell
		bool       sidestepWhenKeepFacing{ true };  // clips 8..15 instead of 0..7 when the actor keeps its facing
		int        enterTimeoutFrames{ 6 };

		// [Motion]
		int yawSign{ 1 };

		// [Compat]
		bool allowFirstPerson{ true };

		// [Jump]
		bool blockJumpDuringDodge{ true };  // a jump may leave a dodge only once its exit window is open

		// [Stagger]
		bool  dodgeFromStagger{ false };  // a dodge may cut a stagger short (meant to be unlocked later)
		float staggerDodgeDelay{ 0.0f };  // seconds of stagger before that dodge is allowed

		// [IFrames]
		bool  iFrames{ true };
		bool  projectileIFrames{ true };  // every projectile type passes through during i-frames
		bool  explosionIFrames{ true };   // explosions skip the player during i-frames
		float iFrameStart{ 0.05f };
		float iFrameEnd{ 0.35f };

		bool suppressLegacyDodge{ true };
		bool useTrueDirectionalMovement{ true };
		bool usePrecision{ true };  // register i-frames with Precision's pre-hit callback

		// [Steering] (game side): read the move input against the camera yaw the dodge started with,
		// so turning the camera mid-dodge does not steer it (a locked target still does).
		bool inputFromStartCamera{ false };

		// [Input] sIdleReplayChains: idle mods' selection chains for an attack held back by a dodge, re-chosen when it is
		// replayed. Each: "Plugin.esp|0xID|0xID|...", the idles in the order the mod tries them.
		std::vector<std::string> idleReplayChains{ "SCSI-ACTbfco-Main.esp|0x944|0x8BE|0x8C0|0x945|0x946|0x947|0x948|0x8C5" };

		// [AttackCancel]: when a dodge may cancel the actor's own attack (beyond sAttackStates, see ActionArbiter.h).
		struct AttackCancelSettings
		{
			// Attack frameworks: a recovery event opens the recovery until a recovery-end event or the next attack;
			// a recovery variable (bool graph variable) counts while true.
			std::vector<std::string> recoveryEvents{ "MCO_Recovery", "BFCO_DIY_recovery" };
			std::vector<std::string> recoveryEndEvents{ "MCO_InputBuffer", "Bfco_AttackStartFX" };
			std::vector<std::string> recoveryVariables{ "MCO_IsInRecovery" };
			// Graph events of a new attack starting (besides the vanilla attack state passing through Draw):
			// the attack's recovery and clip windows start over.
			std::vector<std::string> attackStartEvents{ "MCO_AttackInitiate", "MCO_PowerAttackInitiate", "Bfco_AttackStartFX" };
			// Keywords ("Plugin.esp|0xFormID" or an EditorID) on the actor, its worn items or its active magic effects:
			// a dodge cancels its normal / power attacks at any moment.
			std::string anyTimeKeyword;
			std::string anyTimePowerKeyword;
			// [Rules] sNoDodgeKeyword: an actor with this keyword (same sources) cannot dodge at all.
			std::string noDodgeKeyword;
		} attackCancel;

		// [Perfect]: a hit the i-frames dodge inside this part of the dodge (seconds from its start) is a perfect
		// dodge. The window only counts where i-frames are active.
		struct PerfectSettings
		{
			bool        enabled{ true };
			float       windowStart{ 0.0f };
			float       windowEnd{ 0.15f };
			// slow motion (player only), real seconds
			float       slowMotion{ 1.0f };          // world speed during it; 1 = no slow motion
			float       slowMotionPlayer{ 1.0f };    // the player's speed during it; 1 = unaffected, = slowMotion: slowed alike
			float       slowMotionDuration{ 0.4f };  // full slow motion, between the ramps
			float       slowMotionEaseIn{ 0.05f };
			float       slowMotionEaseOut{ 0.2f };
			float       slowMotionCooldown{ 0.0f };  // after one ends, before the next may start
			std::string playerEvent{ "RDF_PerfectDodge" };     // graph event on the player
			std::string attackerEvent{ "RDF_PerfectDodged" };  // graph event on the attacker
			std::string modEvent{ "RDF_PerfectDodge" };        // SKSE mod event (Papyrus RegisterForModEvent)
		} perfect;

		// [Dev]
		bool devLog{ false };

		// Main thread. Get() is the live tuning everything reads.
		static const Settings& Get();
		static void            Load();
		// Replace the live tuning (main thread); bumps Generation() so cached copies (the core) refresh.
		static void            Commit(const Settings& s);
		static std::uint32_t   Generation();
		// A copy for another thread (the menu renders on the render thread).
		static Settings        Snapshot();
	};
}
