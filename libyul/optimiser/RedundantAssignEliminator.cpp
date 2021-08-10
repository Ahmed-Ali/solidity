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
 * Optimiser component that removes assignments to variables that are not used
 * until they go out of scope or are re-assigned.
 */

#include <libyul/optimiser/RedundantAssignEliminator.h>

#include <libyul/optimiser/Semantics.h>
#include <libyul/AST.h>

#include <libsolutil/CommonData.h>

#include <range/v3/action/remove_if.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::yul;

void RedundantAssignEliminator::run(OptimiserStepContext& _context, Block& _ast)
{
	RedundantAssignEliminator rae{_context.dialect};
	rae(_ast);

	StatementRemover remover{rae.m_pendingRemovals};
	remover(_ast);
}

void RedundantAssignEliminator::operator()(Identifier const& _identifier)
{
	changeUndecidedTo(_identifier.name, UseState::Used);
}

void RedundantAssignEliminator::operator()(VariableDeclaration const& _variableDeclaration)
{
	ASTWalker::operator()(_variableDeclaration);

	for (auto const& var: _variableDeclaration.variables)
		m_declaredVariables.emplace(var.name);
}

void RedundantAssignEliminator::operator()(Assignment const& _assignment)
{
	visit(*_assignment.value);
	for (auto const& var: _assignment.variableNames)
		changeUndecidedTo(var.name, UseState::Unused);

	if (_assignment.variableNames.size() == 1)
		// Default-construct it in "Undecided" state if it does not yet exist.
		m_assignments[_assignment.variableNames.front().name][&_assignment];
}

void RedundantAssignEliminator::operator()(If const& _if)
{
	visit(*_if.condition);

	TrackedAssignments skipBranch{m_assignments};
	(*this)(_if.body);

	merge(m_assignments, move(skipBranch));
}

void RedundantAssignEliminator::operator()(Switch const& _switch)
{
	visit(*_switch.expression);

	TrackedAssignments const preState{m_assignments};

	bool hasDefault = false;
	vector<TrackedAssignments> branches;
	for (auto const& c: _switch.cases)
	{
		if (!c.value)
			hasDefault = true;
		(*this)(c.body);
		branches.emplace_back(move(m_assignments));
		m_assignments = preState;
	}

	if (hasDefault)
	{
		m_assignments = move(branches.back());
		branches.pop_back();
	}
	for (auto& branch: branches)
		merge(m_assignments, move(branch));
}

void RedundantAssignEliminator::operator()(FunctionDefinition const& _functionDefinition)
{
	ScopedSaveAndRestore outerDeclaredVariables(m_declaredVariables, {});
	ScopedSaveAndRestore outerReturnVariables(m_returnVariables, {});
	ScopedSaveAndRestore outerAssignments(m_assignments, {});
	ScopedSaveAndRestore forLoopInfo(m_forLoopInfo, {});

	for (auto const& retParam: _functionDefinition.returnVariables)
		m_returnVariables.insert(retParam.name);

	(*this)(_functionDefinition.body);

	for (auto const& param: _functionDefinition.parameters)
		finalize(param.name, UseState::Unused);
	for (auto const& retParam: _functionDefinition.returnVariables)
		finalize(retParam.name, UseState::Used);
}

void RedundantAssignEliminator::operator()(ForLoop const& _forLoop)
{
	ScopedSaveAndRestore outerForLoopInfo(m_forLoopInfo, {});
	++m_forLoopNestingDepth;

	// If the pre block was not empty,
	// we would have to deal with more complicated scoping rules.
	assertThrow(_forLoop.pre.statements.empty(), OptimizerException, "");

	// We just run the loop twice to account for the back edge.
	// There need not be more runs because we only have three different states.

	visit(*_forLoop.condition);

	TrackedAssignments zeroRuns{m_assignments};

	(*this)(_forLoop.body);
	merge(m_assignments, move(m_forLoopInfo.pendingContinueStmts));
	m_forLoopInfo.pendingContinueStmts = {};
	(*this)(_forLoop.post);

	visit(*_forLoop.condition);

	if (m_forLoopNestingDepth < 6)
	{
		// Do the second run only for small nesting depths to avoid horrible runtime.
		TrackedAssignments oneRun{m_assignments};

		(*this)(_forLoop.body);

		merge(m_assignments, move(m_forLoopInfo.pendingContinueStmts));
		m_forLoopInfo.pendingContinueStmts.clear();
		(*this)(_forLoop.post);

		visit(*_forLoop.condition);
		// Order of merging does not matter because "max" is commutative and associative.
		merge(m_assignments, move(oneRun));
	}
	else
	{
		// Shortcut to avoid horrible runtime:
		// Change all assignments that were newly introduced in the for loop to "used".
		// We do not have to do that with the "break" or "continue" paths, because
		// they will be joined later anyway.
		// TODO parallel traversal might be more efficient here.
		for (auto& var: m_assignments)
			for (auto& assignment: var.second)
			{
				auto zeroIt = zeroRuns.find(var.first);
				if (zeroIt != zeroRuns.end() && zeroIt->second.count(assignment.first))
					continue;
				assignment.second = UseState::Value::Used;
			}
	}

	// Order of merging does not matter because "max" is commutative and associative.
	merge(m_assignments, move(zeroRuns));
	merge(m_assignments, move(m_forLoopInfo.pendingBreakStmts));
	m_forLoopInfo.pendingBreakStmts.clear();

	--m_forLoopNestingDepth;
}

void RedundantAssignEliminator::operator()(Break const&)
{
	m_forLoopInfo.pendingBreakStmts.emplace_back(move(m_assignments));
	m_assignments.clear();
}

void RedundantAssignEliminator::operator()(Continue const&)
{
	m_forLoopInfo.pendingContinueStmts.emplace_back(move(m_assignments));
	m_assignments.clear();
}

void RedundantAssignEliminator::operator()(Leave const&)
{
	for (YulString name: m_returnVariables)
		changeUndecidedTo(name, UseState::Used);
}

void RedundantAssignEliminator::operator()(Block const& _block)
{
	ScopedSaveAndRestore outerDeclaredVariables(m_declaredVariables, {});

	ASTWalker::operator()(_block);

	for (auto const& var: m_declaredVariables)
		finalize(var, UseState::Unused);
}


void RedundantAssignEliminator::merge(TrackedAssignments& _target, TrackedAssignments&& _other)
{
	util::joinMap(_target, move(_other), [](
		map<Assignment const*, UseState>& _assignmentHere,
		map<Assignment const*, UseState>&& _assignmentThere
	)
	{
		return util::joinMap(_assignmentHere, move(_assignmentThere), UseState::join);
	});
}

void RedundantAssignEliminator::merge(TrackedAssignments& _target, vector<TrackedAssignments>&& _source)
{
	for (TrackedAssignments& ts: _source)
		merge(_target, move(ts));
	_source.clear();
}

void RedundantAssignEliminator::changeUndecidedTo(YulString _variable, UseState _newState)
{
	for (auto& assignment: m_assignments[_variable])
		if (assignment.second == UseState::Undecided)
			assignment.second = _newState;
}

void RedundantAssignEliminator::finalize(YulString _variable, UseState _finalState)
{
	std::map<Assignment const*, UseState> assignments;
	util::joinMap(assignments, std::move(m_assignments[_variable]), UseState::join);
	m_assignments.erase(_variable);

	for (auto& breakAssignments: m_forLoopInfo.pendingBreakStmts)
	{
		util::joinMap(assignments, std::move(breakAssignments[_variable]), UseState::join);
		breakAssignments.erase(_variable);
	}
	for (auto& continueAssignments: m_forLoopInfo.pendingContinueStmts)
	{
		util::joinMap(assignments, std::move(continueAssignments[_variable]), UseState::join);
		continueAssignments.erase(_variable);
	}

	for (auto const& assignment: assignments)
	{
		UseState const state = assignment.second == UseState::Undecided ? _finalState : assignment.second;

		if (state == UseState::Unused && SideEffectsCollector{*m_dialect, *assignment.first->value}.movable())
			m_pendingRemovals.insert(assignment.first);
	}
}
