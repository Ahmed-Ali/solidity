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
 * Optimiser component that removes stores to variables that are not used
 * or overwritten later on.
 */

#pragma once

#include <libyul/ASTForward.h>
#include <libyul/optimiser/ASTWalker.h>
#include <libyul/optimiser/OptimiserStep.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/RedundantAssignUtils.h>

#include <map>
#include <vector>

namespace solidity::yul
{
struct Dialect;
struct AssignedValue;

class RedundantStoreEliminator: public ASTWalker
{
public:
	static constexpr char const* name{"RedundantStoreEliminator"};
	static void run(OptimiserStepContext& _context, Block& _ast);

	explicit RedundantStoreEliminator(
		Dialect const& _dialect,
		std::map<YulString, SideEffects> const& _functionSideEffects,
		std::map<YulString, AssignedValue> const& _ssaValues,
		bool _ignoreMemory
	):
		m_dialect(_dialect),
		m_ignoreMemory(_ignoreMemory),
		m_functionSideEffects(_functionSideEffects),
		m_ssaValues(_ssaValues)
	{}

	using ASTWalker::operator();
	void operator()(ExpressionStatement const& _statement) override;
	void operator()(FunctionCall const& _functionCall) override;
	void operator()(If const& _if) override;
	void operator()(Switch const& _switch) override;
	void operator()(FunctionDefinition const&) override;
	void operator()(ForLoop const&) override;
	void operator()(Break const&) override;
	void operator()(Continue const&) override;
	void operator()(Leave const&) override;

	enum class Location { Storage, Memory };
	enum class Effect { Read, Write };
	struct Operation
	{
		Location location;
		Effect effect;
		/// Start of affected area. Unknown if not provided.
		std::optional<YulString> start;
		/// Length of affected area, unknown if not provided.
		/// Unused for storage.
		std::optional<YulString> length;
	};

private:
	std::vector<Operation> operationsFromFunctionCall(FunctionCall const& _functionCall) const;
	void applyOperation(Operation const& _operation);
	bool knownUnrelated(Operation const& _op1, Operation const& _op2) const;
	bool knownCovered(Operation const& _covered, Operation const& _covering) const;

	using TrackedStores = std::map<ExpressionStatement const*, UseState>;

	/// Joins the assignment mapping of @a _source into @a _target according to the rules laid out
	/// above.
	/// Will destroy @a _source.
	static void merge(TrackedStores& _target, TrackedStores&& _source);
	static void merge(TrackedStores& _target, std::vector<TrackedStores>&& _source);

	void changeUndecidedTo(UseState _newState, std::optional<Location> _onlyLocation = std::nullopt);
	void scheduleForDeletion(UseState _inState = UseState::Unused);

	std::optional<YulString> identifierIfSSA(Expression const& _expression) const;

	Dialect const& m_dialect;
	bool const m_ignoreMemory;
	std::map<YulString, SideEffects> const& m_functionSideEffects;
	std::map<YulString, AssignedValue> const& m_ssaValues;

	TrackedStores m_stores;
	std::map<ExpressionStatement const*, Operation> m_storeOperations;

	std::set<ExpressionStatement const*> m_toDelete;

	ForLoopInfo<TrackedStores> m_forLoopInfo;
	size_t m_forLoopNestingDepth = 0;
};

}
