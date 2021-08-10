/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Optimiser utilities that are used both by the RedundantAsssignEliminator and the
 * RedundantStoreEliminator.
 */

#pragma once

#include <libyul/optimiser/ASTWalker.h>
#include <libyul/AST.h>

#include <range/v3/action/remove_if.hpp>

#include <variant>


namespace solidity::yul
{

class UseState
{
public:
	enum Value { Unused, Undecided, Used };

	UseState(Value _value = Undecided): m_value(_value) {}

	inline bool operator==(UseState _other) const { return m_value == _other.m_value; }
	inline bool operator!=(UseState _other) const { return !operator==(_other); }

	static inline void join(UseState& _a, UseState const& _b)
	{
		// Using "max" works here because of the order of the values in the enum.
		_a.m_value =  Value(std::max(int(_a.m_value), int(_b.m_value)));
	}
private:
	Value m_value = Undecided;
};


/// Working data for traversing for-loops.
template <class TrackedAssignments>
struct ForLoopInfo
{
	/// Tracked assignment states for each break statement.
	std::vector<TrackedAssignments> pendingBreakStmts;
	/// Tracked assignment states for each continue statement.
	std::vector<TrackedAssignments> pendingContinueStmts;
};

template <class T>
class StatementRemover: public ASTModifier
{
public:
	explicit StatementRemover(std::set<T const*> const& _toRemove): m_toRemove(_toRemove) {}

	void operator()(Block& _block) override
	{
		ranges::actions::remove_if(_block.statements, [&](Statement const& _statement) -> bool {
			return m_toRemove.count(std::get_if<T>(&_statement));
		});
		ASTModifier::operator()(_block);
	}

private:
	std::set<T const*> const& m_toRemove;
};

}
