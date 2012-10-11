//===- PHITransAddr.cpp - PHI Translation for Addresses -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the PHITransAddr class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/PHITransAddr.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

bool PHITransAddr::CanPHITrans(const Instruction *Inst) const {
  if (isa<PHINode>(Inst) ||
      isa<BitCastInst>(Inst) ||
      isa<GetElementPtrInst>(Inst))
    return true;
  
  if (Inst->getOpcode() == Instruction::Add &&
      (isa<ConstantInt>(Inst->getOperand(1)) || getConstReplacement(Inst->getOperand(1))))
    return true;
  
  //   cerr << "MEMDEP: Could not PHI translate: " << *Pointer;
  //   if (isa<BitCastInst>(PtrInst) || isa<GetElementPtrInst>(PtrInst))
  //     cerr << "OP:\t\t\t\t" << *PtrInst->getOperand(0);
  return false;
}

void PHITransAddr::dump() const {
  if (Addr == 0) {
    dbgs() << "PHITransAddr: null\n";
    return;
  }
  dbgs() << "PHITransAddr: " << *Addr << "\n";
  for (unsigned i = 0, e = InstInputs.size(); i != e; ++i)
    dbgs() << "  Input #" << i << " is " << *InstInputs[i] << "\n";
}


bool PHITransAddr::VerifySubExpr(const Value* Expr,
				 SmallVectorImpl<Instruction*> &InstInputs) const {
  // If this is a non-instruction value, there is nothing to do.
  const Instruction *I = dyn_cast<Instruction>(Expr);
  if (I == 0) return true;

  // If it's an instruction, it is either in Tmp or its operands recursively
  // are.
  SmallVectorImpl<Instruction*>::iterator Entry =
    std::find(InstInputs.begin(), InstInputs.end(), I);
  if (Entry != InstInputs.end()) {
    InstInputs.erase(Entry);
    return true;
  }

  if(parent) {
    ValCtx Replacement = parent->getReplacement(const_cast<Value*>(Expr));
    if(Replacement.second != parent)
      return true;
    if(!(I = dyn_cast<Instruction>(Replacement.first)))
      return true;
  }
 
  // If it isn't in the InstInputs list it is a subexpr incorporated into the
  // address.  Sanity check that it is phi translatable.
  if (!CanPHITrans(I)) {
    errs() << "Non phi translatable instruction found in PHITransAddr, either "
              "something is missing from InstInputs or CanPHITrans is wrong:\n";
    errs() << *I << '\n';
    return false;
  }
  
  // Validate the operands of the instruction.
  for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i)
    if (!VerifySubExpr(I->getOperand(i), InstInputs))
      return false;

  return true;
}

/// Verify - Check internal consistency of this data structure.  If the
/// structure is valid, it returns true.  If invalid, it prints errors and
/// returns false.
bool PHITransAddr::Verify() const {
  if (Addr == 0) return true;
  
  SmallVector<Instruction*, 8> Tmp(InstInputs.begin(), InstInputs.end());  
  
  if (!VerifySubExpr(Addr, Tmp))
    return false;
  
  if (!Tmp.empty()) {
    errs() << "PHITransAddr inconsistent, contains extra instructions:\n";
    errs() << "Addr is ";
    if(Addr)
      errs() << *Addr;
    else
      errs() << "null";
    errs() << "\n";
    for (unsigned i = 0, e = InstInputs.size(); i != e; ++i)
      errs() << "  InstInput #" << i << " is " << *InstInputs[i] << "\n";
    return false;
  }
  
  // a-ok.
  return true;
}


/// IsPotentiallyPHITranslatable - If this needs PHI translation, return true
/// if we have some hope of doing it.  This should be used as a filter to
/// avoid calling PHITranslateValue in hopeless situations.
bool PHITransAddr::IsPotentiallyPHITranslatable() const {
  // If the input value is not an instruction, or if it is not defined in CurBB,
  // then we don't need to phi translate it.
  Value* CheckAddr = Addr;
  if(parent) {
    ValCtx VC = parent->getReplacement(Addr);
    if(VC.second != parent)
      return true;
    CheckAddr = VC.first;
  }

  Instruction *Inst = dyn_cast<Instruction>(CheckAddr);
  return Inst == 0 || CanPHITrans(Inst);
}


static void RemoveInstInputs(Value *V, 
                             SmallVectorImpl<Instruction*> &InstInputs) {
  Instruction *I = dyn_cast<Instruction>(V);
  if (I == 0) return;
  
  // If the instruction is in the InstInputs list, remove it.
  SmallVectorImpl<Instruction*>::iterator Entry =
    std::find(InstInputs.begin(), InstInputs.end(), I);
  if (Entry != InstInputs.end()) {
    InstInputs.erase(Entry);
    return;
  }
  
  assert(!isa<PHINode>(I) && "Error, removing something that isn't an input");
  
  // Otherwise, it must have instruction inputs itself.  Zap them recursively.
  for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
    if (Instruction *Op = dyn_cast<Instruction>(I->getOperand(i)))
      RemoveInstInputs(Op, InstInputs);
  }
}

Value *PHITransAddr::PHITranslateSubExpr(Value *V, BasicBlock *CurBB,
                                         BasicBlock *PredBB,
                                         const DominatorTree *DT) {
  // If this is a non-instruction value, it can't require PHI translation.
  Instruction *Inst = dyn_cast<Instruction>(V);
  Instruction *OrigInst = Inst;
  if (Inst == 0) return V;
  
  // Determine whether 'Inst' is an input to our PHI translatable expression.
  bool isInput = std::count(InstInputs.begin(), InstInputs.end(), Inst);

  if(parent) {
    // Check if our parent can replace this for us:
    ValCtx VC = parent->getReplacement(V);

    if(VC.second != parent) // The value is defined by something from an outer scope relative to PredBB. Fine.
      return V;

    Inst = dyn_cast<Instruction>(VC.first); // Investigate the replacement, not the real expression.
    if(!Inst) return V;
  }

  // Handle inputs instructions if needed.
  if (isInput) {
    if (Inst->getParent() != CurBB) {
      // If it is an input defined in a different block, then it remains an
      // input.
      return OrigInst;
    }

    // If 'Inst' is defined in this block and is an input that needs to be phi
    // translated, we need to incorporate the value into the expression or fail.

    // In either case, the instruction itself isn't an input any longer.
    InstInputs.erase(std::find(InstInputs.begin(), InstInputs.end(), OrigInst));
    
    // If this is a PHI, go ahead and translate it.
    if (PHINode *PN = dyn_cast<PHINode>(Inst)) {
      Value* V = PN->getIncomingValueForBlock(PredBB);
      
      return AddAsInput(V);
    }
    
    // If this is a non-phi value, and it is analyzable, we can incorporate it
    // into the expression by making all instruction operands be inputs.
    if (!CanPHITrans(Inst))
      return 0;
   
    // All instruction operands are now inputs (and of course, they may also be
    // defined in this block, so they may need to be phi translated themselves.
    for (unsigned i = 0, e = Inst->getNumOperands(); i != e; ++i)
      if (Instruction *Op = dyn_cast<Instruction>(Inst->getOperand(i)))
        InstInputs.push_back(Op);
  }

  // Ok, it must be an intermediate result (either because it started that way
  // or because we just incorporated it into the expression).  See if its
  // operands need to be phi translated, and if so, reconstruct it.
  
  if (BitCastInst *BC = dyn_cast<BitCastInst>(Inst)) {
    Value *CastArg = BC->getOperand(0);
    Value *PHIIn = PHITranslateSubExpr(CastArg, CurBB, PredBB, DT);
    if (PHIIn == 0) return 0;
    if (PHIIn == CastArg)
      return BC;
    
    // Find an available version of this cast.
    
    // Constants are trivial to find.
    if (Constant *C = dyn_cast<Constant>(PHIIn))
      return AddAsInput(ConstantExpr::getBitCast(C, BC->getType()));
    
    // Otherwise we have to see if a bitcasted version of the incoming pointer
    // is available.  If so, we can use it, otherwise we have to fail.
    for (Value::use_iterator UI = PHIIn->use_begin(), E = PHIIn->use_end();
         UI != E; ++UI) {
      if (BitCastInst *BCI = dyn_cast<BitCastInst>(*UI))
        if (BCI->getType() == BC->getType() &&
            (!DT || DT->dominates(BCI->getParent(), PredBB)))
          return BCI;
    }
    return 0;
  }
  
  // Handle getelementptr with at least one PHI translatable operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
    SmallVector<Value*, 8> GEPOps;
    bool AnyChanged = false;
    for (unsigned i = 0, e = GEP->getNumOperands(); i != e; ++i) {
      Value *GEPOp = PHITranslateSubExpr(GEP->getOperand(i), CurBB, PredBB, DT);
      if (GEPOp == 0) return 0;
      
      AnyChanged |= GEPOp != GEP->getOperand(i);
      GEPOps.push_back(GEPOp);
    }
    
    if (!AnyChanged)
      return GEP;
    
    // Simplify the GEP to handle 'gep x, 0' -> x etc.
    if (Value *V = SimplifyGEPInst(&GEPOps[0], GEPOps.size(), TD)) {
      for (unsigned i = 0, e = GEPOps.size(); i != e; ++i)
        RemoveInstInputs(GEPOps[i], InstInputs);
      
      return AddAsInput(V);
    }

    // Scan to see if we have this GEP available.
    Value *NewGEP = 0;
    Value *APHIOp = GEPOps[0];
    for (Value::use_iterator UI = APHIOp->use_begin(), E = APHIOp->use_end();
         UI != E; ++UI) {
      if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(*UI))
        if (GEPI->getType() == GEP->getType() &&
            GEPI->getNumOperands() == GEPOps.size() &&
            GEPI->getParent()->getParent() == CurBB->getParent() &&
            (!DT || DT->dominates(GEPI->getParent(), PredBB))) {
          bool Mismatch = false;
          for (unsigned i = 0, e = GEPOps.size(); i != e; ++i)
            if (GEPI->getOperand(i) != GEPOps[i]) {
              Mismatch = true;
              break;
            }
          if (!Mismatch) {
            NewGEP = GEPI;
	    break;
	  }
        }
    }

    if(NewGEP) {
      // If the new GEP has a replacement, disregard its arguments as they won't get used.
      if(parent) {
	ValCtx ReplacementVC = parent->getReplacement(NewGEP);
	if(ReplacementVC.second != parent) {
	  for (unsigned i = 0, e = GEPOps.size(); i != e; ++i)
	    RemoveInstInputs(GEPOps[i], InstInputs);
	  return AddAsInput(NewGEP);
	}
      }
    }

    return NewGEP;
  }
  
  // Handle add with a constant RHS.
  Constant* RHS = dyn_cast<ConstantInt>(Inst->getOperand(1));
  if(!RHS && parent)
    RHS = getConstReplacement(Inst->getOperand(1));
  if (Inst->getOpcode() == Instruction::Add && RHS) {

    // PHI translate the LHS.
    bool isNSW = cast<BinaryOperator>(Inst)->hasNoSignedWrap();
    bool isNUW = cast<BinaryOperator>(Inst)->hasNoUnsignedWrap();
    
    Value *LHS = PHITranslateSubExpr(Inst->getOperand(0), CurBB, PredBB, DT);
    if (LHS == 0) return 0;
    
    // If the PHI translated LHS is an add of a constant, fold the immediates.
    if (BinaryOperator *BOp = dyn_cast<BinaryOperator>(LHS))
      if (BOp->getOpcode() == Instruction::Add) {
	ConstantInt* CI = dyn_cast<ConstantInt>(BOp->getOperand(1));
	if(parent && !CI)
	  CI = dyn_cast_or_null<ConstantInt>(getConstReplacement(BOp->getOperand(1)));
        if (CI) {
          LHS = BOp->getOperand(0);
          RHS = ConstantExpr::getAdd(RHS, CI);
          isNSW = isNUW = false;
          
          // If the old 'LHS' was an input, add the new 'LHS' as an input.
          if (std::count(InstInputs.begin(), InstInputs.end(), BOp)) {
            RemoveInstInputs(BOp, InstInputs);
            AddAsInput(LHS);
          }
        }
      }
    
    // See if the add simplifies away.
    if (Value *Res = SimplifyAddInst(LHS, RHS, isNSW, isNUW, TD)) {
      // If we simplified the operands, the LHS is no longer an input, but Res
      // is.
      RemoveInstInputs(LHS, InstInputs);
      return AddAsInput(Res);
    }

    // If we didn't modify the add, just return it.
    if (LHS == Inst->getOperand(0) && RHS == Inst->getOperand(1))
      return Inst;
    
    // Otherwise, see if we have this add available somewhere.
    for (Value::use_iterator UI = LHS->use_begin(), E = LHS->use_end();
         UI != E; ++UI) {
      if (BinaryOperator *BO = dyn_cast<BinaryOperator>(*UI))
        if (BO->getOpcode() == Instruction::Add &&
            BO->getOperand(0) == LHS && BO->getOperand(1) == RHS &&
            BO->getParent()->getParent() == CurBB->getParent() &&
            (!DT || DT->dominates(BO->getParent(), PredBB)))
          return BO;
    }
    
    return 0;
  }
  
  // Otherwise, we failed.
  return 0;
}


/// PHITranslateValue - PHI translate the current address up the CFG from
/// CurBB to Pred, updating our state to reflect any needed changes.  If the
/// dominator tree DT is non-null, the translated value must dominate
/// PredBB.  This returns true on failure and sets Addr to null.
bool PHITransAddr::PHITranslateValue(BasicBlock *CurBB, BasicBlock *PredBB,
                                     const DominatorTree *DT) {
  assert(Verify() && "Invalid PHITransAddr!");
  Value* OldAddr = Addr;
  Addr = PHITranslateSubExpr(OldAddr, CurBB, PredBB, DT);
  assert(Verify() && "Invalid PHITransAddr!");

  if (DT) {
    // Make sure the value is live in the predecessor.
    if (Instruction *Inst = dyn_cast_or_null<Instruction>(Addr))
      if (!DT->dominates(Inst->getParent(), PredBB))
        Addr = 0;
  }

  return Addr == 0;
}

/// PHITranslateWithInsertion - PHI translate this value into the specified
/// predecessor block, inserting a computation of the value if it is
/// unavailable.
///
/// All newly created instructions are added to the NewInsts list.  This
/// returns null on failure.
///
Value *PHITransAddr::
PHITranslateWithInsertion(BasicBlock *CurBB, BasicBlock *PredBB,
                          const DominatorTree &DT,
                          SmallVectorImpl<Instruction*> &NewInsts) {
  unsigned NISize = NewInsts.size();
  
  // Attempt to PHI translate with insertion.
  Addr = InsertPHITranslatedSubExpr(Addr, CurBB, PredBB, DT, NewInsts);
  
  // If successful, return the new value.
  if (Addr) return Addr;
  
  // If not, destroy any intermediate instructions inserted.
  while (NewInsts.size() != NISize)
    NewInsts.pop_back_val()->eraseFromParent();
  return 0;
}


/// InsertPHITranslatedPointer - Insert a computation of the PHI translated
/// version of 'V' for the edge PredBB->CurBB into the end of the PredBB
/// block.  All newly created instructions are added to the NewInsts list.
/// This returns null on failure.
///
Value *PHITransAddr::
InsertPHITranslatedSubExpr(Value *InVal, BasicBlock *CurBB,
                           BasicBlock *PredBB, const DominatorTree &DT,
                           SmallVectorImpl<Instruction*> &NewInsts) {
  // See if we have a version of this value already available and dominating
  // PredBB.  If so, there is no need to insert a new instance of it.
  PHITransAddr Tmp(InVal, TD);
  if (!Tmp.PHITranslateValue(CurBB, PredBB, &DT))
    return Tmp.getAddr();

  // If we don't have an available version of this value, it must be an
  // instruction.
  Instruction *Inst = cast<Instruction>(InVal);
  
  // Handle bitcast of PHI translatable value.
  if (BitCastInst *BC = dyn_cast<BitCastInst>(Inst)) {
    Value *OpVal = InsertPHITranslatedSubExpr(BC->getOperand(0),
                                              CurBB, PredBB, DT, NewInsts);
    if (OpVal == 0) return 0;
    
    // Otherwise insert a bitcast at the end of PredBB.
    BitCastInst *New = new BitCastInst(OpVal, InVal->getType(),
                                       InVal->getName()+".phi.trans.insert",
                                       PredBB->getTerminator());
    NewInsts.push_back(New);
    return New;
  }
  
  // Handle getelementptr with at least one PHI operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
    SmallVector<Value*, 8> GEPOps;
    BasicBlock *CurBB = GEP->getParent();
    for (unsigned i = 0, e = GEP->getNumOperands(); i != e; ++i) {
      Value *OpVal = InsertPHITranslatedSubExpr(GEP->getOperand(i),
                                                CurBB, PredBB, DT, NewInsts);
      if (OpVal == 0) return 0;
      GEPOps.push_back(OpVal);
    }
    
    GetElementPtrInst *Result = 
    GetElementPtrInst::Create(GEPOps[0], GEPOps.begin()+1, GEPOps.end(),
                              InVal->getName()+".phi.trans.insert",
                              PredBB->getTerminator());
    Result->setIsInBounds(GEP->isInBounds());
    NewInsts.push_back(Result);
    return Result;
  }
  
#if 0
  // FIXME: This code works, but it is unclear that we actually want to insert
  // a big chain of computation in order to make a value available in a block.
  // This needs to be evaluated carefully to consider its cost trade offs.
  
  // Handle add with a constant RHS.
  if (Inst->getOpcode() == Instruction::Add &&
      isa<ConstantInt>(Inst->getOperand(1))) {
    // PHI translate the LHS.
    Value *OpVal = InsertPHITranslatedSubExpr(Inst->getOperand(0),
                                              CurBB, PredBB, DT, NewInsts);
    if (OpVal == 0) return 0;
    
    BinaryOperator *Res = BinaryOperator::CreateAdd(OpVal, Inst->getOperand(1),
                                           InVal->getName()+".phi.trans.insert",
                                                    PredBB->getTerminator());
    Res->setHasNoSignedWrap(cast<BinaryOperator>(Inst)->hasNoSignedWrap());
    Res->setHasNoUnsignedWrap(cast<BinaryOperator>(Inst)->hasNoUnsignedWrap());
    NewInsts.push_back(Res);
    return Res;
  }
#endif
  
  return 0;
}
