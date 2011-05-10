//===- FlattenConditionalAssignments.cpp - Flatten conditional assignments ===//
//
// LunarGLASS: An Open Modular Shader Compiler Architecture
// Copyright (C) 2010-2011 LunarG, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; version 2 of the
// License.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301, USA.
//
//===----------------------------------------------------------------------===//
//
// Author: Michael Ilseman, LunarG
//
//===----------------------------------------------------------------------===//
//
// Flatten conditional assignments into select instructions.
//
// if (p)
//    v = foo;
// else
//    v = bar;
//
// Becomes something like:
//
// %v = select <ty>, i1 p, <ty> foo, <ty> bar
//
// This pass may modify the CFG. After all (or zero) conditional assignments
// have been removed, it will then see if the conditional is blank, and if so,
// remove it. Currently the flattening itself is unimplemented, just the code
// simplification and removal of empty conditionals
//
//===----------------------------------------------------------------------===//


#include "llvm/BasicBlock.h"
#include "llvm/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include "Passes/PassSupport.h"
#include "Passes/Analysis/IdentifyConditionals.h"
#include "Passes/Util/BasicBlockUtil.h"

using namespace llvm;
using namespace gla_llvm;

namespace  {
    class FlattenCondAssn : public FunctionPass {
    public:
        // Standard pass stuff
        static char ID;

        FlattenCondAssn() : FunctionPass(ID)
        {
            initializeFlattenCondAssnPass(*PassRegistry::getPassRegistry());
        }

        virtual bool runOnFunction(Function&);
        void print(raw_ostream&, const Module* = 0) const;
        virtual void getAnalysisUsage(AnalysisUsage&) const;

    protected:
        IdentifyConditionals* idConds;
        DominatorTree* domTree;


        // Have conditional assignments be select instructions in the merge
        // block. Currently only creates selects from phi nodes that receive
        // their values from empty left and right blocks.
        bool createSelects(const Conditional*);

        bool simplifyAndRemoveDeadCode(Conditional*);

        bool removeEmptyConditional(Conditional*);

        bool flattenConds();
    };
} // end namespace

bool FlattenCondAssn::runOnFunction(Function& F)
{
    domTree = &getAnalysis<DominatorTree>();
    idConds = &getAnalysis<IdentifyConditionals>();

    bool changed = false;

    // TODO: Refactor this file to eliminate some of these loops
    while (flattenConds())
        changed = true;

    return changed;
}

bool FlattenCondAssn::flattenConds()
{
    bool changed = false;

    for (IdentifyConditionals::iterator i = idConds->begin(), e = idConds->end(); i != e; ++i) {
        Conditional* cond = i->second;

        if (!cond)
            continue;

        BasicBlock* entry = cond->getEntryBlock();

        // See if the then, else, and merge blocks are all dominated by the
        // entry. If this doesn't hold, we're not interested in it.
        if ( ! (   domTree->dominates(entry, cond->getThenBlock())
                && domTree->dominates(entry, cond->getThenBlock())
                && domTree->dominates(entry, cond->getMergeBlock())))
            continue;

        // Move all the conditional assignments out, iteratively in case of some
        // interdependence
        while (createSelects(cond))
            changed = true;

        // Simplify/Eliminate instructions in the branches
        while (simplifyAndRemoveDeadCode(cond))
            changed = true;

        // Remove empty conditionals.
        if (removeEmptyConditional(cond)) {
            changed = true;
            idConds->nullConditional(entry);
        };
    }

    return changed;
}

// Have conditional assignments be select instructions in the merge
// block. Currently only creates selects from phi nodes that receive their
// values from empty left and right blocks.

// TODO: Revise and test for the case that the phi gets its value from the left
// or right subgraphs, but the value still is just contingent on the entry's
// condition. Also, handle the case where the left and right blocks are not
// empty, but the value is not defined in them.
bool FlattenCondAssn::createSelects(const Conditional* cond)
{
    bool changed = false;

    BasicBlock* left  = cond->getThenBlock();
    BasicBlock* right = cond->getElseBlock();
    BasicBlock* merge = cond->getMergeBlock();
    BasicBlock* entry = cond->getEntryBlock();

    // We only work on empty left and right blocks for now.
    if (! IsEmptyBB(left) || ! IsEmptyBB(right))
        return changed;

    // Check all the phis, and change any of the ones that receive their value
    // from left and right into selects on the entry's condition.
    for (BasicBlock::iterator instI = merge->begin(), instE = merge->end(), nextInst; instI != instE; instI = nextInst) {
        nextInst = instI;
        ++nextInst;

        PHINode* pn = dyn_cast<PHINode>(instI);
        if (!pn)
            break;

        // We only want phis from left and right
        if (pn->getNumIncomingValues() != 2) {
            continue;
        }

        Value* leftVal = NULL;
        Value* rightVal = NULL;

        for (int i = 0; i < 2; ++i) {
            if (pn->getIncomingBlock(i) == left)
                leftVal = pn->getIncomingValue(i);
            else if (pn->getIncomingBlock(i) == right)
                rightVal = pn->getIncomingValue(i);
        }

        if (! leftVal || ! rightVal) {
            continue;
        }

        // If we've made it this far, we have a phi we want to convert into a
        // select.

        BranchInst* br = dyn_cast<BranchInst>(entry->getTerminator());
        assert(br->isConditional());

        SelectInst* si = SelectInst::Create(br->getCondition(), leftVal, rightVal, "select");
        ReplaceInstWithInst(pn, si);
        changed = true;
    }

    return changed;
}

bool FlattenCondAssn::simplifyAndRemoveDeadCode(Conditional* cond)
{
    bool changed = false;
    changed |= SimplifyInstructionsInBlock(cond->getEntryBlock());
    changed |= SimplifyInstructionsInBlock(cond->getThenBlock());
    changed |= SimplifyInstructionsInBlock(cond->getElseBlock());

    // TODO: Remove dead code in the then and else subgraphs

    // <do stuff>
    return false;
}

bool FlattenCondAssn::removeEmptyConditional(Conditional* cond)
{
    // TODO: remove next line after updating affected conditionals has been
    // added
    cond->recalculate();

    if (!cond->isSelfContained() || !cond->isEmptyConditional())
        return false;

    BasicBlock* left  = cond->getThenBlock();
    BasicBlock* right = cond->getElseBlock();
    BasicBlock* merge = cond->getMergeBlock();
    BasicBlock* entry = cond->getEntryBlock();

    // We don't yet handle phis in the merge block
    // TODO: update once handling of phis in merge block has been taken care of.
    if (HasPHINodes(merge))
        return false;

    // TODO: change to an assert when conditionals are properly updated
    if (! entry->getParent())
        return false;

    BranchInst* entryBranch = dyn_cast<BranchInst>(entry->getTerminator());
    assert(entryBranch);

    bool changed = true;

    // errs() << " \n\n"
    //        << "RemoveEmptyConditional:\n"
    //        << "Entry: " << *entry
    //        << "Left:  " << *left
    //        << "Right: " << *right
    //        << "Merge: " << *merge;

    ReplaceInstWithInst(entryBranch, BranchInst::Create(merge));

    changed |= SimplifyInstructionsInBlock(entry);
    changed |= SimplifyInstructionsInBlock(merge);

    // TODO: check for the case where there's a phi node in merge, and handle it
    // appropriately

    RecursivelyRemoveNoPredecessorBlocks(left);
    RecursivelyRemoveNoPredecessorBlocks(right);

    // TODO: merge the block into the predecessor, and update any
    // affected conditionals
    // MergeBasicBlockIntoOnlyPred(merge);

    return changed;
}

void FlattenCondAssn::getAnalysisUsage(AnalysisUsage& AU) const
{
    AU.addRequired<DominatorTree>();
    AU.addRequired<IdentifyConditionals>();

    return;
}

void FlattenCondAssn::print(raw_ostream&, const Module*) const
{
    return;
}

char FlattenCondAssn::ID = 0;
INITIALIZE_PASS_BEGIN(FlattenCondAssn,
                      "flatten-conditional-assignments",
                      "Flatten conditional assignments into select instructions",
                      true,   // Whether it preserves the CFG
                      false); // Whether it is an analysis pass
INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_DEPENDENCY(IdentifyConditionals)
INITIALIZE_PASS_END(FlattenCondAssn,
                    "flatten-conditional-assignments",
                    "Flatten conditional assignments into select instructions",
                    true,   // Whether it preserves the CFG
                    false); // Whether it is an analysis pass

FunctionPass* gla_llvm::createFlattenConditionalAssignmentsPass()
{
    return new FlattenCondAssn();
}
