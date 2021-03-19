//===-- PatternFunctionComparator.cpp - Code pattern instruction matcher --===//
//
//       SimpLL - Program simplifier for analysis of semantic difference      //
//
// This file is published under Apache 2.0 license. See LICENSE for details.
// Author: Petr Silling, psilling@redhat.com
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the implementation of the LLVM code pattern matcher. The
/// pattern matcher is a comparator extension of the LLVM FunctionComparator
/// tailored to difference pattern comparison.
///
//===----------------------------------------------------------------------===//

#include "PatternFunctionComparator.h"
#include "Config.h"
#include "Utils.h"

/// Compare the module function and the difference pattern from the starting
/// module instruction. This includes checks for correct input mappings.
int PatternFunctionComparator::compare() {
    // Clear all previous results.
    beginCompare();
    InstMatchMap.clear();
    InputMatchMap.clear();
    ReverseInputMatchMap.clear();

    // Run the main matchin algorithm.
    if (int Res = matchPattern())
        return Res;

    // Reset the comparison state without clearing pattern matches.
    beginCompare();

    // Ensure that the created input mapping is correct. All input instructions
    // and arguments have to be mapped correctly.
    if (int Res = checkInputMapping())
        return Res;

    return 0;
}

/// Set the starting module instruction.
void PatternFunctionComparator::setStartInstruction(
        const Instruction *StartModInst) {
    StartInst = StartModInst;
}

/// Compare a module input value with a pattern input value. Used for comparing
/// input values that could not be mapped during the first one-side comparison.
int PatternFunctionComparator::cmpInputValues(const Value *L, const Value *R) {
    // Check pointer validity.
    if (!L)
        return -1;
    if (!R)
        return 1;

    // The pattern value may have been already mapped. If so, it must be mapped
    // to the given module value.
    if (InputMatchMap.find(R) != InputMatchMap.end()) {
        if (InputMatchMap[R] != L)
            return 1;
        return 0;
    }

    // Reset the comparison state without clearing pattern matches.
    beginCompare();

    auto Input = IsLeftSide ? &ParentPattern->InputL : &ParentPattern->InputR;

    SmallVector<const Value *, 8> InputL, InputR;
    SmallPtrSet<const Value *, 32> VisitedL;

    // Initialize the input comparison with the given values.
    InputL.push_back(L);
    InputR.push_back(R);

    // Try to find a matching module counterpart for all instructions that use
    // the compared pattern value (either directly or indirectly).
    while (!InputL.empty()) {
        auto ValueL = InputL.pop_back_val();
        auto ValueR = InputR.pop_back_val();

        // Map the values to each other.
        InputMatchMap[R] = L;
        ReverseInputMatchMap[L] = R;
        VisitedL.insert(L);

        auto UserL = ValueL->user_begin(), UserR = ValueR->user_begin();
        auto UserLB = UserL;
        auto UserLE = ValueL->user_end(), UserRE = ValueR->user_end();

        while (UserL != UserLE && UserR != UserRE) {
            // Skip pattern users that do not reperesent input values.
            if (ParentPattern->MetadataMap[*UserR].NotAnInput) {
                ++UserR;
                continue;
            }

            auto InstL = dyn_cast<Instruction>(*UserL);
            auto InstR = dyn_cast<Instruction>(*UserR);

            // Users are expected to be instructions.
            if (!InstL) {
                ++UserL;
                continue;
            }
            if (!InstR)
                return 1;

            if (ReverseInputMatchMap.find(InstL)
                != ReverseInputMatchMap.end()) {
                // Skip already mapped module instructions.
                if (ReverseInputMatchMap[InstL] != InstR) {
                    ++UserL;
                    continue;
                }
            } else if (InputMatchMap.find(InstR) != InputMatchMap.end()) {
                // Skip pattern instructions that have already been mapped to
                // one of the analysed module instructions. When mapped to an
                // unrelated instruction, fail the comparison.
                if (std::find(UserLB, UserLE, InputMatchMap[InstR]) != UserLE) {
                    ++UserR;
                    continue;
                }
                break;
            } else if (int Res = cmpOperationsWithOperands(InstL, InstR)) {
                // Compare the instructions. If they match, continue the
                // comparison on both sides. Otherwise, try to find a different,
                // more suitable module instruction.
                eraseNewlyMapped();
                ++UserL;
                continue;
            }

            ++UserL;
            ++UserR;

            if (!VisitedL.insert(InstL).second)
                continue;

            // Schedule newly mapped instructions for comparison.
            InputL.push_back(InstL);
            InputR.push_back(InstR);
        }

        // If any users remain on the pattern side, ensure that they are mapped
        // to skipped users from the module.
        while (UserR != UserRE) {
            if (std::find(UserLB, UserLE, InputMatchMap[*UserR]) == UserLE)
                return -1;

            ++UserR;
        }
    }

    return 0;
}

/// Compare a module GEP operation with a pattern GEP operation. The
/// implementation is extended to support a name-based comparison of structure
/// types.
/// Note: Parts of this function have been adapted from FunctionComparator.
/// Therefore, LLVM licensing also applies here. See the LICENSE information in
/// the appropriate llvm-lib subdirectory for more details.
int PatternFunctionComparator::cmpGEPs(const GEPOperator *GEPL,
                                       const GEPOperator *GEPR) const {
    // When using the GEP operations on pointers, vectors or arrays, perform the
    // default comparison. Also use the default comparison if the name-based
    // structure comparison is disabled.
    if (!GEPL->getSourceElementType()->isStructTy()
        || !GEPR->getSourceElementType()->isStructTy()
        || ParentPattern->MetadataMap[GEPR].DisableNameComparison) {
        return FunctionComparator::cmpGEPs(GEPL, GEPR);
    }

    // Compare structures without calculating offsets since both structures
    // should be the same.
    unsigned int ASL = GEPL->getPointerAddressSpace();
    unsigned int ASR = GEPR->getPointerAddressSpace();

    Type *TyL = GEPL->getSourceElementType();
    Type *TyR = GEPR->getSourceElementType();

    if (int Res = cmpNumbers(ASL, ASR))
        return Res;

    // Try to perform the base comparison. Validate the result by comparing the
    // structure names.
    if (int Res = cmpTypes(TyL, TyR)) {
        if (cmpNames(TyL->getStructName(), TyR->getStructName()))
            return Res;
    }

    if (int Res = cmpNumbers(GEPL->getNumOperands(), GEPR->getNumOperands()))
        return Res;

    for (unsigned i = 0, e = GEPL->getNumOperands(); i != e; ++i) {
        if (int Res = cmpValues(GEPL->getOperand(i), GEPR->getOperand(i)))
            return Res;
    }

    return 0;
}

/// Compare a module function instruction with a pattern instruction along
/// with their operands.
/// Note: Parts of this function have been adapted from FunctionComparator.
/// Therefore, LLVM licensing also applies here. See the LICENSE information
/// in the appropriate llvm-lib subdirectory for more details.
int PatternFunctionComparator::cmpOperationsWithOperands(
        const Instruction *L, const Instruction *R) const {
    bool needToCmpOperands = true;

    // Clear newly mapped holders.
    NewlyMappedValuesL.clear();
    NewlyMappedValuesR.clear();
    NewlyMappedInputL.clear();
    NewlyMappedInputR.clear();

    // Compare the instruction and its operands.
    if (int Res = cmpOperations(&*L, &*R, needToCmpOperands))
        return Res;
    if (needToCmpOperands) {
        assert(L->getNumOperands() == R->getNumOperands());

        for (unsigned i = 0, e = L->getNumOperands(); i != e; ++i) {
            Value *OpL = L->getOperand(i);
            Value *OpR = R->getOperand(i);

            if (int Res = cmpValues(OpL, OpR))
                return Res;
            assert(cmpTypes(OpL->getType(), OpR->getType()) == 0);
        }
    }
    // Map the instructions to each other.
    InstMatchMap[R] = L;
    return 0;
}

/// Compare a module function basic block with a pattern basic block.
int PatternFunctionComparator::cmpBasicBlocks(const BasicBlock *BBL,
                                              const BasicBlock *BBR) const {
    BasicBlock::const_iterator InstL = BBL->begin(), InstLE = BBL->end();
    BasicBlock::const_iterator InstR = BBR->begin(), InstRE = BBR->end();

    // When comparing the first pair, jump to start instructions.
    if (InstMatchMap.empty()) {
        jumpToStartInst(InstL, StartInst);
        jumpToStartInst(InstR,
                        IsLeftSide ? ParentPattern->StartPositionL
                                   : ParentPattern->StartPositionR);
    }

    int GroupDepth = 0;
    while (InstL != InstLE && InstR != InstRE) {
        auto InstMetadata = ParentPattern->MetadataMap[&*InstR];

        // Check whether the compared pattern ends at this instruction.
        if (InstMetadata.PatternEnd)
            return 0;

        // Compare current instructions with operands.
        if (int Res = cmpOperationsWithOperands(&*InstL, &*InstR)) {
            // When in an instruction group, do not allow module instruction
            // skipping.
            if (GroupDepth > 0)
                return Res;

            // Remove newly added value and input mappings.
            eraseNewlyMapped();

            // Skip the module instruction.
            ++InstL;
            continue;
        }

        // Register pattern instruction groups.
        if (InstMetadata.GroupStart)
            ++GroupDepth;
        if (InstMetadata.GroupEnd)
            --GroupDepth;

        ++InstL;
        ++InstR;
    }

    if (InstL != InstLE && InstR == InstRE)
        return 1;
    if (InstL == InstLE && InstR != InstRE)
        return -1;

    return 0;
}

/// Compare global values by their names, because their indexes are not
/// expected to be the same.
int PatternFunctionComparator::cmpGlobalValues(GlobalValue *L,
                                               GlobalValue *R) const {
    if (L->hasName() && R->hasName()) {
        // Both values are named, compare them by names
        auto NameL = L->getName();
        auto NameR = R->getName();

        if (cmpNames(NameL, NameR))
            return 1;

        return 0;
    }

    return L != R;
}

/// Tests whether two names of types or globals match. Names match if they
/// are the same or if the DiffKemp specific name prefixes are used on the
/// pattern side.
int PatternFunctionComparator::cmpNames(const StringRef &L,
                                        const StringRef &R) const {
    auto NameL = L;
    auto NameR = R;

    // Remove number suffixes
    if (hasSuffix(NameL))
        NameL = NameL.substr(0, NameL.find_last_of("."));
    if (hasSuffix(NameR))
        NameR = NameR.substr(0, NameR.find_last_of("."));

    // Compare the names themselves.
    if (NameL == NameR)
        return 0;

    // If no prefix is present, the names are not equal.
    if (!NameR.startswith_lower(PatternSet::DefaultPrefix))
        return 1;

    // Remove all prefixes.
    auto PrefixR = IsLeftSide ? PatternSet::PrefixL : PatternSet::PrefixR;
    StringRef RealNameR = NameR.substr(PatternSet::DefaultPrefix.size());

    if (RealNameR.startswith_lower(PrefixR))
        RealNameR = RealNameR.substr(PrefixR.size());

    // Compare the names without prefixes.
    return NameL != RealNameR;
}

/// Compare a module value with a pattern value using serial numbers.
/// Note: Parts of this function have been adapted from FunctionComparator.
/// Therefore, LLVM licensing also applies here. See the LICENSE information
/// in the appropriate llvm-lib subdirectory for more details.
int PatternFunctionComparator::cmpValues(const Value *L, const Value *R) const {
    // Catch self-reference case.
    if (L == FnL) {
        if (R == FnR)
            return 0;
        return -1;
    }
    if (R == FnR) {
        if (L == FnL)
            return 0;
        return 1;
    }

    // Try to compare the values as constants.
    const Constant *ConstL = dyn_cast<Constant>(L);
    const Constant *ConstR = dyn_cast<Constant>(R);
    if (ConstL && ConstR) {
        if (L == R)
            return 0;
        return cmpConstants(ConstL, ConstR);
    }

    if (ConstL)
        return 1;
    if (ConstR)
        return -1;

    // Try to compare the values as inline assemblies.
    const InlineAsm *InlineAsmL = dyn_cast<InlineAsm>(L);
    const InlineAsm *InlineAsmR = dyn_cast<InlineAsm>(R);

    if (InlineAsmL && InlineAsmR)
        return cmpInlineAsm(InlineAsmL, InlineAsmR);
    if (InlineAsmL)
        return 1;
    if (InlineAsmR)
        return -1;

    // Try to insert serial numbers for both values.
    auto LeftSN = sn_mapL.insert(std::make_pair(L, sn_mapL.size())),
         RightSN = sn_mapR.insert(std::make_pair(R, sn_mapR.size()));

    // Register newly inserted values.
    if (LeftSN.second)
        NewlyMappedValuesL.insert(L);
    if (RightSN.second)
        NewlyMappedValuesR.insert(R);

    // Compare assigned serial numbers.
    if (int Res = cmpNumbers(LeftSN.first->second, RightSN.first->second))
        return Res;

    // Since the values are equal, try to match them as inputs.
    if (int Res = mapInputValues(L, R))
        return Res;

    return 0;
}

/// Uses function comparison to try and match the given pattern to the
/// corresponding module. Uses the implementation of the compare method from
/// LLVM FunctionComparator, extended to support comparisons starting from
/// specific instructions. Because of that, code reffering to the comparison of
/// whole functions has also been removed.
/// Note: Parts of this function have been adapted from the compare method of
/// FunctionComparator. Therefore, LLVM licensing also applies here. See the
/// LICENSE information in the appropriate llvm-lib subdirectory for more
/// details.
int PatternFunctionComparator::matchPattern() const {
    // We do a CFG-ordered walk since the actual ordering of the blocks in the
    // linked list is immaterial. Our walk starts at the containing blocks of
    // the starting instructions, then takes each block from each terminator in
    // order. Instructions from the first pair of blocks that are before the
    // starting instructions will get ignored. As an artifact, this also means
    // that unreachable blocks are ignored.
    SmallVector<const BasicBlock *, 8> FnLBBs, FnRBBs;
    SmallPtrSet<const BasicBlock *, 32> VisitedL;

    // Get the starting basic blocks.
    FnLBBs.push_back(StartInst->getParent());
    if (IsLeftSide) {
        FnRBBs.push_back(ParentPattern->StartPositionL->getParent());
    } else {
        FnRBBs.push_back(ParentPattern->StartPositionR->getParent());
    }

    // Run the pattern comparison.
    VisitedL.insert(FnLBBs[0]);
    while (!FnLBBs.empty()) {
        const BasicBlock *BBL = FnLBBs.pop_back_val();
        const BasicBlock *BBR = FnRBBs.pop_back_val();

        if (int Res = cmpValues(BBL, BBR))
            return Res;

        if (int Res = cmpBasicBlocks(BBL, BBR))
            return Res;

#if LLVM_VERSION_MAJOR < 8
        const TerminatorInst *TermL = BBL->getTerminator();
        const TerminatorInst *TermR = BBR->getTerminator();
#else
        const Instruction *TermL = BBL->getTerminator();
        const Instruction *TermR = BBR->getTerminator();
#endif
        // Do not descend to successors if the pattern terminating instruction
        // is not a direct part of the pattern.
        if (ParentPattern->MetadataMap[TermR].PatternEnd)
            continue;

        // Queue all successor basic blocks.
        assert(TermL->getNumSuccessors() == TermR->getNumSuccessors());
        for (unsigned i = 0, e = TermL->getNumSuccessors(); i != e; ++i) {

            if (!VisitedL.insert(TermL->getSuccessor(i)).second)
                continue;

            FnLBBs.push_back(TermL->getSuccessor(i));
            FnRBBs.push_back(TermR->getSuccessor(i));
        }
    }

    return 0;
}

/// Erases newly mapped instructions from synchronization maps and input maps.
void PatternFunctionComparator::eraseNewlyMapped() const {
    for (auto &&MappedValueL : NewlyMappedValuesL) {
        sn_mapL.erase(MappedValueL);
    }
    for (auto &&MappedValueR : NewlyMappedValuesR) {
        sn_mapR.erase(MappedValueR);
    }
    for (auto &&MappedInputR : NewlyMappedInputR) {
        InputMatchMap.erase(MappedInputR);
    }
    for (auto &&MappedInputL : NewlyMappedInputL) {
        ReverseInputMatchMap.erase(MappedInputL);
    }
}

/// Checks whether all currently mapped input instructions or arguments have
/// an associated module counterpart.
int PatternFunctionComparator::checkInputMapping() const {
    // Compare mapped input arguments.
    for (auto &&ArgR : FnR->args()) {
        auto MappedValues = InputMatchMap.find(&ArgR);
        if (MappedValues != InputMatchMap.end()) {
            if (int Res = cmpValues(MappedValues->second, &ArgR))
                return Res;
        }
    }

    // Compare mapped input instructions. Corresponding instructions or
    // arguments should be present on the module side. Comparison starts from
    // the entry block since input instructions should be placed before the
    // starting instruction.
    for (auto &&BB : *FnR) {
        for (auto &&InstR : BB) {
            // End after all input instructions have been processed.
            if (ParentPattern->MetadataMap[&InstR].PatternStart)
                return 0;

            // Only analyse input instructions.
            if (ParentPattern->MetadataMap[&InstR].NotAnInput)
                continue;

            auto MappedValues = InputMatchMap.find(&InstR);
            if (MappedValues != InputMatchMap.end()) {
                // Use instruction comparison when mapped to an instruction.
                // Otherwise, only compare values.
                if (auto InstL = dyn_cast<Instruction>(MappedValues->second)) {
                    if (int Res = cmpOperationsWithOperands(InstL, &InstR))
                        return Res;
                } else if (int Res = cmpValues(MappedValues->second, &InstR)) {
                    return Res;
                }
            }
        }
    }

    return 0;
}

/// Tries to map a module value (including possible predecessors) to a
/// pattern input value. If no input value is present, the mapping is
/// always successful.
int PatternFunctionComparator::mapInputValues(const Value *L,
                                              const Value *R) const {
    // The pattern input may have been already mapped. If so, it must be mapped
    // to the given module value.
    if (InputMatchMap.find(R) != InputMatchMap.end()) {
        if (InputMatchMap[R] != L)
            return 1;
        return 0;
    }

    auto Input = IsLeftSide ? &ParentPattern->InputL : &ParentPattern->InputR;

    SmallVector<const Value *, 16> InputL, InputR;
    SmallPtrSet<const Value *, 32> VisitedL;

    // Initialize the input search with the given values.
    InputL.push_back(L);
    InputR.push_back(R);
    VisitedL.insert(L);

    // If input values are given, map them to their module counterparts
    // (including predecessors).
    while (!InputL.empty()) {
        auto ValueL = InputL.pop_back_val();
        auto ValueR = InputR.pop_back_val();

        if (Input->find(ValueR) != Input->end()) {
            // Map the values together.
            InputMatchMap[ValueR] = ValueL;
            ReverseInputMatchMap[ValueL] = ValueR;
            NewlyMappedInputL.insert(ValueL);
            NewlyMappedInputR.insert(ValueR);

            // Descend only if both values are instructions.
            auto InstL = dyn_cast<Instruction>(ValueL);
            auto InstR = dyn_cast<Instruction>(ValueR);
            if (InstL && InstR) {
                // Mapped input instructions should have the same number of
                // operands.
                if (int Res = cmpNumbers(InstL->getNumOperands(),
                                         InstR->getNumOperands()))
                    return Res;

                // Descend into unvisited operands.
                for (int i = 0, e = InstL->getNumOperands(); i != e; ++i) {
                    Value *OpL = InstL->getOperand(i);
                    Value *OpR = InstR->getOperand(i);

                    if (InputMatchMap.find(OpR) != InputMatchMap.end()
                        || !VisitedL.insert(OpL).second)
                        continue;

                    InputL.push_back(OpL);
                    InputR.push_back(OpR);
                }
            }
        }
    }

    return 0;
}

/// Position the basic block instruction iterator forward to the given
/// starting instruction.
void PatternFunctionComparator::jumpToStartInst(
        BasicBlock::const_iterator &BBIterator,
        const Instruction *Start) const {
    while (&*BBIterator != Start) {
        ++BBIterator;
    }
}
