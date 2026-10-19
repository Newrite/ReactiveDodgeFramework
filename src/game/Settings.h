#pragma once

#include "core/Config.h"

#include <string>
#include <vector>

namespace rdf::game
{
	struct GraphNames
	{
		// events sent to the graph
		std::string start{ "RDF_DodgeStart" };
		std::string chain{ "RDF_DodgeChain" };
		std::string exit{ "RDF_DodgeExit" };
		// events reported by the graph
		std::string entered{ "RDF_DodgeEntered" };
		std::string left{ "RDF_DodgeLeft" };
		std::string clipEnd{ "RDF_DodgeClipEnd" };
		// variables: clip and speed are written by us, isDodging is only read (set by the graph)
		std::string clip{ "RDF_DodgeClip" };
		std::string speed{ "RDF_DodgeSpeed" };
		std::string isDodging{ "RDF_IsDodging" };
		std::vector<std::string> windowOpen{ "MCO_DodgeOpen" };
		std::vector<std::string> windowClose{ "MCO_DodgeClose" };
	};

	struct Settings
	{
		Config core;

		// [Input]: raw device scan codes, -1 = unbound
		int   dodgeKeyboard{ 0x38 };  // Left Alt
		int   dodgeMouse{ -1 };
		int   dodgeGamepad{ 0x80 };  // right stick click
		float powerAttackHold{ 0.30f };

		// [Cost]
		float staminaCost{ 20.0f };
		float minStamina{ 10.0f };

		// [Graph]
		GraphNames graph;
		float      animSpeed{ 1.0f };
		float      clipLength{ 0.667f };  // seconds of the dodge clip at speed 1
		bool       sidestepWhenKeepFacing{ true };  // clips 8..15 instead of 0..7 when the actor keeps its facing
		int        enterTimeoutFrames{ 6 };

		// [Motion]
		int yawSign{ 1 };

		// [Compat]
		bool allowFirstPerson{ false };

		// [IFrames]
		bool  iFrames{ true };
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
