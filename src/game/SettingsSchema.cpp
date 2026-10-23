#include "SettingsSchema.h"

#include "core/IniWriter.h"

#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace rdf::game::schema
{
	namespace
	{
		constexpr auto kIniPath = "Data/SKSE/Plugins/ReactiveDodgeFramework.ini";

#define RDF_F(member) +[](Settings& s) -> float* { return &s.member; }
#define RDF_I(member) +[](Settings& s) -> int* { return &s.member; }
#define RDF_B(member) +[](Settings& s) -> bool* { return &s.member; }
#define RDF_S(member) +[](Settings& s) -> std::string* { return &s.member; }

		Field Float(Page page, const char* section, const char* key, float* (*f)(Settings&), float min, float max, const char* format)
		{
			Field x{ page, section, key, Kind::Float, min, max, format };
			x.f = f;
			return x;
		}

		Field Int(Page page, const char* section, const char* key, int* (*i)(Settings&), int min, int max)
		{
			Field x{ page, section, key, Kind::Int, static_cast<float>(min), static_cast<float>(max), "%d" };
			x.i = i;
			return x;
		}

		Field Bool(Page page, const char* section, const char* key, bool* (*b)(Settings&))
		{
			Field x{ page, section, key, Kind::Bool };
			x.b = b;
			return x;
		}

		Field Form(Page page, const char* section, const char* key, std::string* (*str)(Settings&), RE::FormType type)
		{
			Field x{ page, section, key, Kind::Form };
			x.str = str;
			x.formType = type;
			return x;
		}

		Field Named(Field f, const char* textKey)
		{
			f.textKey = textKey;
			return f;
		}

		Field Custom(Field f)
		{
			f.custom = true;
			return f;
		}

		constexpr std::array<const char*, 3> kOrientations{ "Auto", "FaceMotion", "KeepFacing" };
		constexpr std::array<const char*, 3> kResources{ "None", "Stamina", "Magicka" };

		Field Orientation()
		{
			Field x{ Page::Steering, "Steering", "sOrientation", Kind::Choice };
			x.getChoice = [](const Settings& s) { return static_cast<int>(s.core.steering.orientation); };
			x.setChoice = [](Settings& s, int v) { s.core.steering.orientation = static_cast<OrientationMode>(v); };
			x.choices = kOrientations;
			return x;
		}

		Field Resource()
		{
			Field x{ Page::Rules, "Cost", "sResource", Kind::Choice };
			x.getChoice = [](const Settings& s) {
				if (!s.costResource) {
					return 0;
				}
				return *s.costResource == RE::ActorValue::kMagicka ? 2 : 1;
			};
			x.setChoice = [](Settings& s, int v) {
				s.costResource = v == 0 ? std::nullopt :
				                 v == 2 ? std::optional<RE::ActorValue>(RE::ActorValue::kMagicka) :
				                          std::optional<RE::ActorValue>(RE::ActorValue::kStamina);
			};
			x.choices = kResources;
			return x;
		}

		// Ranges are the menu's slider ranges; the ini loader's own clamps are wider or equal.
		const std::vector<Field>& Table()
		{
			static const std::vector<Field> fields{
				// --- input: the dodge keys are drawn as one FLICK hotkey widget (Menu.cpp)
				Custom(Int(Page::Input, "Input", "iDodgeKeyboard", RDF_I(dodgeKeyboard), -1, 0xFF)),
				Custom(Int(Page::Input, "Input", "iDodgeMouse", RDF_I(dodgeMouse), -1, 0xFF)),
				Custom(Int(Page::Input, "Input", "iDodgeGamepad", RDF_I(dodgeGamepad), -1, 0xFFFF)),
				Float(Page::Input, "Input", "fBufferWindow", RDF_F(core.input.bufferWindow), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Input, "Input", "fDebounce", RDF_F(core.input.debounce), 0.0f, 0.3f, "%.2f s"),
				Bool(Page::Input, "Input", "bHoldPressesDuringDodge", RDF_B(core.input.holdDuringDodge)),
				Float(Page::Input, "Input", "fPowerAttackHold", RDF_F(powerAttackHold), 0.05f, 1.0f, "%.2f s"),
				// --- timing and windows
				Float(Page::Timing, "Timing", "fStartup", RDF_F(core.timing.startup), 0.0f, 0.5f, "%.3f s"),
				Float(Page::Timing, "Timing", "fActive", RDF_F(core.timing.active), 0.0f, 1.0f, "%.3f s"),
				Float(Page::Timing, "Timing", "fRecovery", RDF_F(core.timing.recovery), 0.0f, 1.0f, "%.3f s"),
				Float(Page::Timing, "Timing", "fExitBlend", RDF_F(core.timing.exitBlend), 0.02f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fActionExitBlend", RDF_F(core.timing.actionExitBlend), 0.02f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fAttackExitBlend", RDF_F(core.timing.attackExitBlend), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fChainFrom", RDF_F(core.policy.chainFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fAttackFrom", RDF_F(core.policy.attackFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fBlockFrom", RDF_F(core.policy.blockFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fExitFrom", RDF_F(core.policy.exitFrom), 0.0f, 1.5f, "%.2f s"),
				Int(Page::Timing, "Timing", "iMaxChain", RDF_I(core.policy.maxChain), 0, 10),
				Float(Page::Timing, "IFrames", "fIFrameStart", RDF_F(iFrameStart), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Timing, "IFrames", "fIFrameEnd", RDF_F(iFrameEnd), 0.0f, 1.5f, "%.2f s"),
				// --- motion
				Float(Page::Motion, "Motion", "fDistance", RDF_F(core.motion.distance), 50.0f, 1000.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fRunSpeed", RDF_F(core.motion.runSpeed), 0.0f, 800.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fMaxCarrySpeed", RDF_F(core.motion.maxCarrySpeed), 0.0f, 1000.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fCarryDecay", RDF_F(carryDecay), 0.0f, 5000.0f, "%.0f"),
				Bool(Page::Motion, "Motion", "bBackstepWithoutInput", RDF_B(core.motion.backstepWithoutInput)),
				// --- steering
				Float(Page::Steering, "Steering", "fDirectionCommitTime", RDF_F(core.steering.commitTime), 0.0f, 0.3f, "%.2f s"),
				Float(Page::Steering, "Steering", "fMaxDeviation", RDF_F(core.steering.maxDeviation), 0.0f, 180.0f, "%.0f deg"),
				Float(Page::Steering, "Steering", "fInputCone", RDF_F(core.steering.inputCone), 0.0f, 180.0f, "%.0f deg"),
				Bool(Page::Steering, "Steering", "bInputFromStartCamera", RDF_B(inputFromStartCamera)),
				Bool(Page::Steering, "Steering", "bHoldFacing", RDF_B(core.steering.holdFacing)),
				Float(Page::Steering, "Steering", "fOrientationWarpMax", RDF_F(core.steering.orientationWarpMax), 0.0f, 90.0f, "%.1f deg"),
				Orientation(),
				Float(Page::Steering, "Steering", "fFaceMotionMaxAngle", RDF_F(core.steering.faceMotionMaxAngle), 0.0f, 180.0f, "%.0f deg"),
				Float(Page::Steering, "Steering", "fRateStartup", RDF_F(core.steering.rateStartup), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fRateActive", RDF_F(core.steering.rateActive), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fRateRecovery", RDF_F(core.steering.rateRecovery), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fStartupYawRate", RDF_F(core.steering.startupYawRate), 1.0f, 10000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fFaceYawRate", RDF_F(core.steering.faceYawRate), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fDeadzone", RDF_F(core.steering.deadzone), 0.0f, 0.9f, "%.2f"),
				Float(Page::Steering, "Steering", "fResponseExponent", RDF_F(core.steering.responseExponent), 0.1f, 5.0f, "%.2f"),
				// --- animation
				Float(Page::Animation, "Graph", "fAnimSpeed", RDF_F(animSpeed), 0.1f, 3.0f, "%.2f"),
				Float(Page::Animation, "Graph", "fClipLength", RDF_F(clipLength), 0.1f, 3.0f, "%.3f s"),
				Float(Page::Animation, "Graph", "fArmStance", RDF_F(armStance), 0.0f, 1.0f, "%.2f"),
				Bool(Page::Animation, "Graph", "bArmStanceWhenSheathed", RDF_B(armStanceWhenSheathed)),
				Float(Page::Animation, "Graph", "fArmStanceFirstPerson", RDF_F(armStanceFirstPerson), 0.0f, 1.0f, "%.2f"),
				Float(Page::Animation, "Graph", "fClipLengthFirstPerson", RDF_F(clipLengthFirstPerson), 0.1f, 3.0f, "%.2f s"),
				Float(Page::Animation, "Graph", "fFirstPersonLookScale", RDF_F(firstPersonLookScale), -2.0f, 2.0f, "%.2f"),
				// --- rules and cost
				Resource(),
				Float(Page::Rules, "Cost", "fCost", RDF_F(cost), 0.0f, 200.0f, "%.0f"),
				Float(Page::Rules, "Cost", "fMinimum", RDF_F(minimum), 0.0f, 200.0f, "%.0f"),
				Bool(Page::Rules, "Perfect", "bPerfectDodge", RDF_B(perfect.enabled)),
				Float(Page::Rules, "Perfect", "fPerfectWindowStart", RDF_F(perfect.windowStart), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectWindowEnd", RDF_F(perfect.windowEnd), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotion", RDF_F(perfect.slowMotion), 0.05f, 1.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionPlayer", RDF_F(perfect.slowMotionPlayer), 0.05f, 1.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionDuration", RDF_F(perfect.slowMotionDuration), 0.0f, 2.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionEaseIn", RDF_F(perfect.slowMotionEaseIn), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionEaseOut", RDF_F(perfect.slowMotionEaseOut), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionCooldown", RDF_F(perfect.slowMotionCooldown), 0.0f, 10.0f, "%.1f s"),
				Form(Page::Rules, "Rules", "sNoDodgeKeyword", RDF_S(attackCancel.noDodgeKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "AttackCancel", "bDodgeInRecovery", RDF_B(core.policy.cancelInRecovery)),
				Form(Page::Rules, "AttackCancel", "sAnyTimeKeyword", RDF_S(attackCancel.anyTimeKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "AttackCancel", "sAnyTimePowerKeyword", RDF_S(attackCancel.anyTimePowerKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "Jump", "bBlockDuringDodge", RDF_B(blockJumpDuringDodge)),
				Bool(Page::Rules, "Stagger", "bDodgeFromStagger", RDF_B(dodgeFromStagger)),
				Float(Page::Rules, "Stagger", "fDodgeDelay", RDF_F(staggerDodgeDelay), -1.0f, 3.0f, "%.2f s"),
				Float(Page::Rules, "Stagger", "fEscapeCost", RDF_F(stagger.escapeCost), 0.0f, 100.0f, "%.0f"),
				Form(Page::Rules, "Stagger", "sNoEscapeKeyword", RDF_S(stagger.noEscapeKeyword), RE::FormType::Keyword),
				Named(Form(Page::Rules, "Stagger", "sAnyTimeKeyword", RDF_S(stagger.anyTimeKeyword), RE::FormType::Keyword), "Stagger_sAnyTimeKeyword"),
				Form(Page::Rules, "Stagger", "sDelayKeyword", RDF_S(stagger.delayKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Stagger", "sEscapeCostKeyword", RDF_S(stagger.escapeCostKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Casting", "sDrawDuringDodgeKeyword", RDF_S(casting.drawKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Casting", "sCastDuringDodgeKeyword", RDF_S(casting.castKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "CastOn", "bCastOn", RDF_B(castOn)),
				// --- debug
				Bool(Page::Debug, "Dev", "bDevLog", RDF_B(devLog)),
			};
			return fields;
		}

#undef RDF_F
#undef RDF_I
#undef RDF_B
#undef RDF_S

		std::string ValueText(const Field& f, const Settings& s)
		{
			auto& m = const_cast<Settings&>(s);  // the accessors take a mutable Settings; nothing is written
			switch (f.kind) {
			case Kind::Float:
				{
					const float v = *f.f(m);
					return std::format("{}", std::round(v * 10000.0f) / 10000.0f);
				}
			case Kind::Int: return std::format("{}", *f.i(m));
			case Kind::Bool: return *f.b(m) ? "1" : "0";
			case Kind::Choice:
				{
					const int v = f.getChoice(s);
					return v >= 0 && v < static_cast<int>(f.choices.size()) ? f.choices[v] : f.choices[0];
				}
			case Kind::Form: return *f.str(m);
			}
			return {};
		}
	}

	std::span<const Field> Fields()
	{
		return Table();
	}

	void CopyPage(Page page, const Settings& from, Settings& to)
	{
		auto& src = const_cast<Settings&>(from);
		for (const auto& f : Table()) {
			if (f.page != page) {
				continue;
			}
			switch (f.kind) {
			case Kind::Float: *f.f(to) = *f.f(src); break;
			case Kind::Int: *f.i(to) = *f.i(src); break;
			case Kind::Bool: *f.b(to) = *f.b(src); break;
			case Kind::Choice: f.setChoice(to, f.getChoice(from)); break;
			case Kind::Form: *f.str(to) = *f.str(src); break;
			}
		}
	}

	bool SaveToIni(const Settings& s, std::string* error)
	{
		std::string text;
		{
			std::ifstream in(kIniPath, std::ios::binary);
			if (in) {
				std::ostringstream ss;
				ss << in.rdbuf();
				text = ss.str();
			}
		}
		std::vector<ini::Entry> entries;
		for (const auto& f : Table()) {
			entries.push_back({ f.section, f.key, ValueText(f, s) });
		}
		const auto    out = ini::Update(text, entries);
		std::ofstream file(kIniPath, std::ios::binary | std::ios::trunc);
		if (!file) {
			if (error) {
				*error = std::format("cannot write {}", kIniPath);
			}
			return false;
		}
		file << out;
		return static_cast<bool>(file);
	}
}
