#pragma once
#include <cstdint>

namespace ReactiveDodgeAPI
{
	using Ticket = std::uint64_t;
	enum class RequestStatus : std::uint32_t { Unknown, Pending, Starting, Started, Rejected };
	enum class Reject : std::uint32_t { None, Rules, Graph, Unavailable, Cancelled, Expired };
	enum class ActorState : std::uint32_t { Idle, Pending, Starting, Dodging, Exiting };
	struct Result
	{
		RequestStatus status{ RequestStatus::Unknown };
		Reject reason{ Reject::None };
	};
}
