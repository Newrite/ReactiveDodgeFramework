#include "Api.h"

#include "UpdateHook.h"

#include "../../api/ReactiveDodgeAPI.h"

namespace rdf::game::api
{
	namespace
	{
		bool Dodge(RE::StaticFunctionTag*, RE::Actor* actor, float angleDeg)
		{
			return update::RequestDodge(actor, angleDeg);
		}

		bool Register(RE::BSScript::IVirtualMachine* vm)
		{
			vm->RegisterFunction("Dodge"sv, "ReactiveDodge"sv, Dodge);
			logger::info("Papyrus: ReactiveDodge.Dodge registered");
			return true;
		}
	}

	void Install()
	{
		if (const auto* papyrus = SKSE::GetPapyrusInterface(); !papyrus || !papyrus->Register(Register)) {
			logger::error("Papyrus natives not registered");
		}
	}
}

extern "C" __declspec(dllexport) bool RDF_RequestDodge(RE::Actor* actor, float angleDeg)
{
	return rdf::game::update::RequestDodge(actor, angleDeg);
}

extern "C" __declspec(dllexport) std::uint32_t RDF_GetApiVersion()
{
	return 2u; // v1 exports remain ABI-compatible
}

extern "C" __declspec(dllexport) const ReactiveDodgeAPI::V2* RDF_GetApiV2()
{
	static const ReactiveDodgeAPI::V2 api{
		sizeof(ReactiveDodgeAPI::V2), 2,
		rdf::game::update::RequestWorld, rdf::game::update::PollRequest,
		rdf::game::update::ActorStatus, rdf::game::update::CancelRequest
	};
	return &api;
}
