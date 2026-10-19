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
		// events reported by the graph
		std::string entered{ "RDF_DodgeEntered" };
		std::string left{ "RDF_DodgeLeft" };
		// variables: clip and speed are written by us, isDodging is only read (set by the graph)
		std::string clip{ "RDF_DodgeClip" };
		std::string speed{ "RDF_DodgeSpeed" };
		std::string armBlend{ "RDF_ArmBlend" };  // weight of the arm-stance layer over the dodge clip
		std::string isDodging{ "RDF_IsDodging" };
		std::vector<std::string> windowOpen{ "MCO_DodgeOpen" };
		std::vector<std::string> windowClose{ "MCO_DodgeClose" };
	};

	struct Settings
	{
		Config core;

		// [Input]: raw device scan codes, -1 = unbound
		int   dodgeKeyboard{ 0x2F };  // V
		int   dodgeMouse{ -1 };
		int   dodgeGamepad{ 0x80 };  // right stick click
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
		bool       armStanceWhenSheathed{ false };  // also keep the stance of a sheathed weapon / spell
		bool       sidestepWhenKeepFacing{ true };  // clips 8..15 instead of 0..7 when the actor keeps its facing
		int        enterTimeoutFrames{ 6 };

		// [Motion]
		int yawSign{ 1 };

		// [Compat]
		bool allowFirstPerson{ false };

		// [IFrames]
		bool  iFrames{ true };
		bool  projectileIFrames{ true };  // arrows and spell missiles pass through during i-frames
		float iFrameStart{ 0.05f };
		float iFrameEnd{ 0.35f };

		bool suppressLegacyDodge{ true };
		bool useTrueDirectionalMovement{ true };

		// [Dev]
		bool devLog{ false };

		static const Settings& Get();
		static void            Load();
	};
}
