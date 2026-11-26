#pragma once

// Colony item inventory with atomic transactions (plan C7), headless.
//
// Six integer item types share ONE capacity (100 per completed Storage, F3; before the
// first Storage completes, the scenario's temporary cargoCapacity). Every change goes
// through apply(), which commits all of a transaction's inputs and outputs together or
// nothing at all: no item ever goes negative and an addition that would exceed the
// capacity is refused instead of deleting something. A lower capacity never removes
// items either; it only blocks further additions until the total falls below it.

#include "arkfall_types.h"

#include <string>

class Inventory
{
public:
	Inventory() = default;
	Inventory(const ResourceAmounts& initial, int capacity) : amounts(initial), capacity(capacity < 0 ? 0 : capacity) {}

	int get(ResourceType type) const { return amounts[type]; }
	const ResourceAmounts& getAmounts() const { return amounts; }
	int getTotal() const { return amounts.total(); }
	int getCapacity() const { return capacity; }
	int getFreeSpace() const { return capacity > getTotal() ? capacity - getTotal() : 0; }
	void setCapacity(int new_capacity) { capacity = new_capacity < 0 ? 0 : new_capacity; }

	// True when every amount in `items` is available.
	bool has(const ResourceAmounts& items) const
	{
		for (std::size_t i = 0; i < resource_type_count; ++i)
			if (amounts.values[i] < items.values[i])
				return false;
		return true;
	}

	enum class Refusal : std::uint8_t { None, MissingInput, NoSpace };

	// Checks removing `remove` and then adding `add` as one transaction. The space check
	// uses the total AFTER the transaction, so a 1 ore -> 1 Steel conversion still runs
	// when the inventory is exactly full (net change 0).
	Refusal check(const ResourceAmounts& remove, const ResourceAmounts& add) const
	{
		if (!has(remove))
			return Refusal::MissingInput;
		const int after = getTotal() - remove.total() + add.total();
		if (add.total() > remove.total() && after > capacity)
			return Refusal::NoSpace;
		return Refusal::None;
	}

	// Commits the transaction only if check() allows it. Returns the refusal (None = done).
	Refusal apply(const ResourceAmounts& remove, const ResourceAmounts& add)
	{
		const Refusal refusal = check(remove, add);
		if (refusal != Refusal::None)
			return refusal;
		for (std::size_t i = 0; i < resource_type_count; ++i)
			amounts.values[i] += add.values[i] - remove.values[i];
		return Refusal::None;
	}

private:
	ResourceAmounts amounts;
	int capacity = 0;
};
