#pragma once
#include "../../api/ReactiveDodgeTypes.h"
#include "Math.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdf
{
	// Bounded, thread-safe handoff. Actor keys are Bethesda handles, never pointers/FormIDs.
	class AiRequests
	{
	public:
		using Ticket = ReactiveDodgeAPI::Ticket;
		using Status = ReactiveDodgeAPI::RequestStatus;
		using Reject = ReactiveDodgeAPI::Reject;
		using State = ReactiveDodgeAPI::ActorState;
		struct Request { Ticket ticket; std::uint32_t actor; Vec2 direction; double submitted; float maxDistance; };
		Ticket Submit(std::uint32_t actor, Vec2 direction, double now, float maxDistance = 220.0f)
		{
			if (!actor || !std::isfinite(direction.x) || !std::isfinite(direction.y) || Length(direction) < 1e-4f ||
				!std::isfinite(maxDistance) || maxDistance <= 0) { return 0; }
			std::scoped_lock lock(mutex_);
			Prune(now);
			if (results_.size() >= 512 || queue_.size() >= 512 || actors_.contains(actor)) { return 0; }
			const auto ticket = ++next_;
			queue_.push_back({ ticket, actor, FromHeading(HeadingOf(direction)), now, maxDistance });
			results_.emplace(ticket, Entry{ actor, { Status::Pending, Reject::None }, now });
			actors_[actor] = State::Pending;
			return ticket;
		}
		std::vector<Request> Drain(double now)
		{
			std::scoped_lock lock(mutex_);
			std::vector<Request> ready;
			for (auto r : queue_) {
				auto it = results_.find(r.ticket);
				if (it == results_.end() || it->second.result.status != Status::Pending) { continue; }
				if (now - r.submitted > 0.25) { FinishLocked(it->second, Status::Rejected, Reject::Expired, now); }
				else { ready.push_back(r); }
			}
			queue_.clear();
			Prune(now);
			return ready;
		}
		bool IsPending(Ticket ticket)
		{
			std::scoped_lock lock(mutex_);
			const auto it = results_.find(ticket);
			return it != results_.end() && it->second.result.status == Status::Pending;
		}
		bool Claim(Ticket ticket, double now)
		{
			std::scoped_lock lock(mutex_);
			const auto it = results_.find(ticket);
			if (it == results_.end() || it->second.result.status != Status::Pending) { return false; }
			FinishLocked(it->second, Status::Starting, Reject::None, now);
			return true;
		}
		void Resolve(Ticket ticket, Status status, Reject reason, double now)
		{
			std::scoped_lock lock(mutex_);
			const auto it = results_.find(ticket);
			if (it == results_.end() || Terminal(it->second.result.status)) { return; }
			FinishLocked(it->second, status, reason, now);
		}
		ReactiveDodgeAPI::Result Poll(Ticket ticket, bool consume, double now)
		{
			std::scoped_lock lock(mutex_);
			Prune(now);
			const auto it = results_.find(ticket);
			if (it == results_.end()) { return {}; }
			const auto result = it->second.result;
			if (consume && Terminal(result.status)) { results_.erase(it); }
			return result;
		}
		bool Cancel(Ticket ticket, double now)
		{
			std::scoped_lock lock(mutex_);
			const auto it = results_.find(ticket);
			if (it == results_.end() || it->second.result.status != Status::Pending) { return false; }
			FinishLocked(it->second, Status::Rejected, Reject::Cancelled, now);
			return true;
		}
		State GetState(std::uint32_t actor)
		{
			std::scoped_lock lock(mutex_);
			const auto it = actors_.find(actor);
			return it == actors_.end() ? State::Idle : it->second;
		}
		void Publish(std::uint32_t actor, State state)
		{
			std::scoped_lock lock(mutex_);
			// An idle snapshot must not erase a concurrent Submit reservation.
			if (state == State::Idle) {
				const auto it = actors_.find(actor);
				if (it != actors_.end() && it->second != State::Pending) { actors_.erase(it); }
			} else { actors_[actor] = state; }
		}
		void Reset()
		{
			std::scoped_lock lock(mutex_);
			queue_.clear(); results_.clear(); actors_.clear(); // deliberately retain next_
		}
	private:
		struct Entry { std::uint32_t actor; ReactiveDodgeAPI::Result result; double time; };
		static bool Terminal(Status status) { return status == Status::Started || status == Status::Rejected; }
		void FinishLocked(Entry& entry, Status status, Reject reason, double now)
		{
			entry.result = { status, reason }; entry.time = now;
			if (status == Status::Rejected) { actors_.erase(entry.actor); }
			else { actors_[entry.actor] = status == Status::Starting ? State::Starting : State::Dodging; }
		}
		void Prune(double now) { std::erase_if(results_, [&](const auto& pair) { return Terminal(pair.second.result.status) && now - pair.second.time > 60.0; }); }
		std::mutex mutex_;
		Ticket next_{ 0 };
		std::vector<Request> queue_;
		std::unordered_map<Ticket, Entry> results_;
		std::unordered_map<std::uint32_t, State> actors_;
	};
}
