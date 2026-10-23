#include "VanillaConditions.h"

#include <algorithm>
#include <mutex>

namespace rdf::game::vanilla
{
	namespace
	{
		using T = RE::SCRIPT_PARAM_TYPE;
		using F = RE::FormType;

		// What a parameter type is written as. Forms name their type where the CK takes one type only.
		void Classify(Param& p)
		{
			const auto form = [&](F type) {
				p.arg = Arg::Form;
				p.formType = type;
			};
			switch (static_cast<T>(p.type)) {
			case T::kChar:
			case T::kInt:
			case T::kStage:
			case T::kCrimeType:
			case T::kFormType:
			case T::kAlignment:
			case T::kCritStage:
			case T::kRelationshipRank:
			case T::kWardState:
			case T::kFurnitureAnimType:
			case T::kFurnitureEntryType: p.arg = Arg::Integer; break;
			case T::kFloat: p.arg = Arg::Float; break;
			case T::kAxis: p.arg = Arg::Axis; break;
			case T::kSex: p.arg = Arg::Sex; break;
			case T::kCastingSource: p.arg = Arg::CastingSource; break;
			case T::kActorValue: p.arg = Arg::ActorValue; break;
			case T::kObjectRef:
			case T::kActor:
			case T::kContainerRef:
			case T::kMapMarker: p.arg = Arg::Reference; break;
			case T::kSpellItem: form(F::Spell); break;
			case T::kCell: form(F::Cell); break;
			case T::kSound: form(F::Sound); break;
			case T::kTopic: form(F::Dialogue); break;
			case T::kQuest: form(F::Quest); break;
			case T::kRace: form(F::Race); break;
			case T::kClass: form(F::Class); break;
			case T::kFaction: form(F::Faction); break;
			case T::kGlobal: form(F::Global); break;
			case T::kActorBase:
			case T::kNPC: form(F::NPC); break;
			case T::kPackage: form(F::Package); break;
			case T::kCombatStyle: form(F::CombatStyle); break;
			case T::kMagicEffect: form(F::MagicEffect); break;
			case T::kWeather: form(F::Weather); break;
			case T::kShaderEffect: form(F::EffectShader); break;
			case T::kFormList: form(F::FormList); break;
			case T::kPerk: form(F::Perk); break;
			case T::kImagespaceMod: form(F::ImageAdapter); break;
			case T::kImagespace: form(F::ImageSpace); break;
			case T::kVoiceType: form(F::VoiceType); break;
			case T::kEncounterZone: form(F::EncounterZone); break;
			case T::kIdleForm: form(F::Idle); break;
			case T::kMessage: form(F::Message); break;
			case T::kEquipType: form(F::EquipSlot); break;
			case T::kMusic: form(F::MusicType); break;
			case T::kKeyword: form(F::Keyword); break;
			case T::kRefType: form(F::LocationRefType); break;
			case T::kLocation: form(F::Location); break;
			case T::kShout: form(F::Shout); break;
			case T::kWordOfPower: form(F::WordOfPower); break;
			case T::kBGSScene: form(F::Scene); break;
			case T::kAssociationType: form(F::AssociationType); break;
			case T::kRegion: form(F::Region); break;
			case T::kInventoryObject:
			case T::kMagicItem:
			case T::kFurnitureOrFormList:
			case T::kObject:
			case T::kWorldOrList:
			case T::kOwner:
			case T::kInvObjectOrFormList:
			case T::kObjectOrFormList:
			case T::kForm:
			case T::kKnowableForm: form(F::None); break;
			default: p.arg = Arg::Unsupported; break;
			}
		}

		std::vector<Function> Build()
		{
			std::vector<Function> out;
			auto*                 commands = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
			if (!commands) {
				return out;
			}
			const auto count = static_cast<std::uint16_t>(RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd);
			for (std::uint16_t i = 0; i < count; ++i) {
				const auto& c = commands[i];
				if (!c.conditionFunction || !c.functionName || !*c.functionName) {
					continue;
				}
				Function f;
				f.index = i;
				f.name = c.functionName;
				f.help = c.helpString ? c.helpString : "";
				f.reference = c.referenceFunction;
				f.function = c.conditionFunction;
				for (std::uint16_t k = 0; k < c.numParams && c.params; ++k) {
					Param p;
					p.name = c.params[k].paramName ? c.params[k].paramName : "";
					p.type = static_cast<std::uint32_t>(c.params[k].paramType.get());
					p.optional = c.params[k].optional;
					Classify(p);
					f.supported = f.supported && (p.arg != Arg::Unsupported || p.optional);
					f.params.push_back(std::move(p));
				}
				f.supported = f.supported && f.params.size() <= 2;
				out.push_back(std::move(f));
			}
			std::sort(out.begin(), out.end(), [](const Function& a, const Function& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
			return out;
		}
	}

	const std::vector<Function>& List()
	{
		static std::once_flag        once;
		static std::vector<Function> list;
		std::call_once(once, [] { list = Build(); });
		return list;
	}

	const Function* Find(std::string_view name)
	{
		const auto& list = List();
		for (const auto& f : list) {
			if (f.name.size() == name.size() && _strnicmp(f.name.c_str(), name.data(), name.size()) == 0) {
				return &f;
			}
		}
		return nullptr;
	}
}
