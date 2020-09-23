/*
 * TypeAnalysis.cpp - Type Analysis Detection Utilities
 *
 * Copyright (C) 2020 William S. Moses (enzyme@wsmoses.com) - All Rights
 * Reserved
 *
 * For commercial use of this code please contact the author(s) above.
 */

#include <cstdint>
#include <deque>

#include <llvm/Config/llvm-config.h>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include "llvm/IR/InstIterator.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/InlineAsm.h"

#include "TypeAnalysis.h"
#include "../Utils.h"

#include "TBAA.h"

llvm::cl::opt<bool> printtype("enzyme_printtype", cl::init(false), cl::Hidden,
                              cl::desc("Print type detection algorithm"));

TypeAnalyzer::TypeAnalyzer(const FnTypeInfo &fn, TypeAnalysis &TA)
    : intseen(), fntypeinfo(fn), interprocedural(TA), DT(*fn.function) {
  // assert(fntypeinfo.knownValues.size() ==
  // fntypeinfo.function->getFunctionType()->getNumParams());
  for (BasicBlock &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {
      workList.push_back(&inst);
    }
  }
  for (BasicBlock &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {
      for (auto &op : inst.operands()) {
        addToWorkList(op);
      }
    }
  }
}

TypeTree getConstantAnalysis(Constant *val, const FnTypeInfo &nfti,
                              TypeAnalysis &TA) {
  auto &dl = nfti.function->getParent()->getDataLayout();
  // Undefined value is an anything everywhere
  if (isa<UndefValue>(val) || isa<ConstantAggregateZero>(val)) {
    return TypeTree(BaseType::Anything).Only(-1);
  }

  // Null pointer is a pointer to anything, everywhere
  if (isa<ConstantPointerNull>(val)) {
    TypeTree vd(BaseType::Pointer);
    vd |= TypeTree(BaseType::Anything).Only(-1);
    return vd.Only(-1);
  }

  // Known pointers are pointers at offset 0
  if (isa<Function>(val) || isa<BlockAddress>(val)) {
    return TypeTree(BaseType::Pointer).Only(-1);
  }

  if (auto ca = dyn_cast<ConstantAggregate>(val)) {
    TypeTree res;
    int off = 0;
    for (unsigned i = 0; i < ca->getNumOperands(); ++i) {
      assert(nfti.function);
      auto op = ca->getOperand(i);
      // TODO check this for i1 constant aggregates packing/etc
      auto size =
          (nfti.function->getParent()->getDataLayout().getTypeSizeInBits(
               op->getType()) +
           7) /
          8;
      res |= getConstantAnalysis(op, nfti, TA)
                 .ShiftIndices(dl, /*init offset*/ 0, /*maxSize*/ size,
                               /*addOffset*/ off);
      off += size;
    }
    return res;
  }

  if (auto ca = dyn_cast<ConstantDataSequential>(val)) {
    TypeTree res;
    int off = 0;
    for (unsigned i = 0; i < ca->getNumElements(); ++i) {
      assert(nfti.function);
      auto op = ca->getElementAsConstant(0);
      // TODO check this for i1 constant aggregates packing/etc
      auto size =
          (nfti.function->getParent()->getDataLayout().getTypeSizeInBits(
               op->getType()) +
           7) /
          8;
      res |= getConstantAnalysis(op, nfti, TA)
                 .ShiftIndices(dl, /*init offset*/ 0, /*maxSize*/ size,
                               /*addOffset*/ off);
      off += size;
    }
    return res;
  }

  if (isa<ConstantData>(val)) {
    if (auto fp = dyn_cast<ConstantFP>(val)) {
      if (fp->isExactlyValue(0.0))
        return TypeTree(BaseType::Anything).Only(-1);
      return TypeTree(fp->getType()).Only(-1);
    }

    if (auto ci = dyn_cast<ConstantInt>(val)) {
      if (ci->getLimitedValue() >= 1 && ci->getLimitedValue() <= 4096) {
        return TypeTree(ConcreteType(BaseType::Integer)).Only(-1);
      }
      if (ci->getType()->getBitWidth() == 8 && ci->getLimitedValue() == 0) {
        return TypeTree(ConcreteType(BaseType::Integer)).Only(-1);
      }
      if (ci->getLimitedValue() == 0)
        return TypeTree(BaseType::Anything).Only(-1);
      return TypeTree(BaseType::Anything).Only(-1);
    }
  }

  if (auto ce = dyn_cast<ConstantExpr>(val)) {
    TypeTree vd;

    auto ae = ce->getAsInstruction();
    ae->insertBefore(nfti.function->getEntryBlock().getTerminator());

    {
      TypeAnalyzer tmp(nfti, TA);
      tmp.workList.clear();
      tmp.visit(*ae);
      vd = tmp.getAnalysis(ae);
    }

    ae->eraseFromParent();
    return vd;
  }

  if (auto gv = dyn_cast<GlobalVariable>(val)) {
    if (gv->isConstant() && gv->hasInitializer()) {
      TypeTree vd = ConcreteType(BaseType::Pointer);
      vd |= getConstantAnalysis(gv->getInitializer(), nfti, TA);
      return vd.Only(-1);
    }
    auto globalSize = dl.getTypeSizeInBits(gv->getValueType()) / 8;
    // since halfs are 16bit (2 byte) and pointers are >=32bit (4 byte) any
    // single byte object must be integral
    if (globalSize == 1) {
      TypeTree vd = ConcreteType(BaseType::Pointer);
      vd |= TypeTree(ConcreteType(BaseType::Integer)).Only(0);
      return vd.Only(-1);
    }
    return TypeTree(BaseType::Pointer).Only(-1);
  }

  return TypeTree();
}

TypeTree TypeAnalyzer::getAnalysis(Value *val) {
  if (val->getType()->isIntegerTy() &&
      cast<IntegerType>(val->getType())->getBitWidth() == 1)
    return TypeTree(ConcreteType(BaseType::Integer)).Only(-1);

  Type *vt = val->getType();
  if (vt->isPointerTy()) {
    vt = cast<PointerType>(vt)->getElementType();
  }

  if (auto con = dyn_cast<Constant>(val)) {
    return getConstantAnalysis(con, fntypeinfo, interprocedural);
  }

  if (auto inst = dyn_cast<Instruction>(val)) {
    if (inst->getParent()->getParent() != fntypeinfo.function) {
      llvm::errs() << " function: " << *fntypeinfo.function << "\n";
      llvm::errs() << " instParent: " << *inst->getParent()->getParent()
                   << "\n";
      llvm::errs() << " inst: " << *inst << "\n";
    }
    assert(inst->getParent()->getParent() == fntypeinfo.function);
  }
  if (auto arg = dyn_cast<Argument>(val)) {
    if (arg->getParent() != fntypeinfo.function) {
      llvm::errs() << " function: " << *fntypeinfo.function << "\n";
      llvm::errs() << " argParent: " << *arg->getParent() << "\n";
      llvm::errs() << " arg: " << *arg << "\n";
    }
    assert(arg->getParent() == fntypeinfo.function);
  }

  if (isa<Argument>(val) || isa<Instruction>(val))
    return analysis[val];

  llvm::errs() << "ERROR UNKNOWN: " << *val << "\n";
  // TODO consider other things like globals perhaps?
  return TypeTree();
}

void TypeAnalyzer::updateAnalysis(Value *val, ConcreteType data, Value *origin) {
  updateAnalysis(val, TypeTree(data), origin);
}

void TypeAnalyzer::updateAnalysis(Value *val, BaseType data, Value *origin) {
  updateAnalysis(val, TypeTree(ConcreteType(data)), origin);
}

void TypeAnalyzer::addToWorkList(Value *val) {
  if (!isa<Instruction>(val) && !isa<Argument>(val))
    return;
  // llvm::errs() << " - adding to work list: " << *val << "\n";
  if (std::find(workList.begin(), workList.end(), val) != workList.end())
    return;

  if (auto inst = dyn_cast<Instruction>(val)) {
    if (fntypeinfo.function != inst->getParent()->getParent()) {
      llvm::errs() << "function: " << *fntypeinfo.function << "\n";
      llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
      llvm::errs() << "inst: " << *inst << "\n";
    }
    assert(fntypeinfo.function == inst->getParent()->getParent());
  }
  if (auto arg = dyn_cast<Argument>(val))
    assert(fntypeinfo.function == arg->getParent());

  // llvm::errs() << " - - true add : " << *val << "\n";
  workList.push_back(val);
}

void TypeAnalyzer::updateAnalysis(Value *val, TypeTree data, Value *origin) {
  if (isa<ConstantData>(val) || isa<Function>(val)) {
    return;
  }

  if (printtype) {
    llvm::errs() << "updating analysis of val: " << *val
                 << " current: " << analysis[val].str() << " new "
                 << data.str();
    if (origin)
      llvm::errs() << " from " << *origin;
    llvm::errs() << "\n";
  }

  if (auto inst = dyn_cast<Instruction>(val)) {
    if (fntypeinfo.function != inst->getParent()->getParent()) {
      llvm::errs() << "function: " << *fntypeinfo.function << "\n";
      llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
      llvm::errs() << "inst: " << *inst << "\n";
    }
    assert(fntypeinfo.function == inst->getParent()->getParent());
  }
  if (auto arg = dyn_cast<Argument>(val))
    assert(fntypeinfo.function == arg->getParent());

  if (isa<GetElementPtrInst>(val) && data[{}] == BaseType::Integer) {
    llvm::errs() << "illegal gep update\n";
    assert(0 && "illegal gep update");
  }

  if (val->getType()->isPointerTy() && data[{}] == BaseType::Integer) {
    llvm::errs() << "illegal gep update for val: " << *val << "\n";
    if (origin)
      llvm::errs() << " + " << *origin << "\n";
    assert(0 && "illegal gep update");
  }

  if (analysis[val] |= data) {
    // Add val so it can explicitly propagate this new info, if able to
    if (val != origin)
      addToWorkList(val);

    // Add users and operands of the value so they can update from the new
    // operand/use
    for (User *use : val->users()) {
      if (use != origin) {

        if (auto inst = dyn_cast<Instruction>(use)) {
          if (fntypeinfo.function != inst->getParent()->getParent()) {
            continue;
          }
        }

        addToWorkList(use);
      }
    }

    if (User *me = dyn_cast<User>(val)) {
      for (Value *op : me->operands()) {
        if (op != origin) {
          addToWorkList(op);
        }
      }
    }
  }
}

void TypeAnalyzer::prepareArgs() {
  for (auto &pair : fntypeinfo.first) {
    assert(pair.first->getParent() == fntypeinfo.function);
    updateAnalysis(pair.first, pair.second, nullptr);
  }

  for (auto &arg : fntypeinfo.function->args()) {
    // Get type and other information about argument
    updateAnalysis(&arg, getAnalysis(&arg), &arg);
  }

  // Propagate return value type information
  for (auto &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {
      if (auto ri = dyn_cast<ReturnInst>(&inst)) {
        if (auto rv = ri->getReturnValue()) {
          updateAnalysis(rv, fntypeinfo.second, nullptr);
        }
      }
    }
  }
}

void TypeAnalyzer::considerTBAA() {
  auto &dl = fntypeinfo.function->getParent()->getDataLayout();

  for (auto &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {

      auto vdptr = parseTBAA(&inst, dl);

      if (!vdptr.isKnownPastPointer())
        continue;

      if (auto call = dyn_cast<CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            (call->getCalledFunction()->getIntrinsicID() == Intrinsic::memcpy ||
             call->getCalledFunction()->getIntrinsicID() ==
                 Intrinsic::memmove)) {
          int64_t sz = 1;
          for (auto val :
               fntypeinfo.knownIntegralValues(call->getOperand(2), DT, intseen)) {
            sz = max(sz, val);
          }
          auto update = vdptr.ShiftIndices(dl, /*init offset*/ 0,
                                           /*max size*/ sz, /*new offset*/ 0);

          updateAnalysis(call->getOperand(0), update.Only(-1), call);
          updateAnalysis(call->getOperand(1), update.Only(-1), call);
          continue;
        } else if (call->getType()->isPointerTy()) {
          updateAnalysis(call, vdptr.Only(-1), call);
        } else {
          llvm::errs() << " inst: " << inst << " vdptr: " << vdptr.str()
                       << "\n";
          assert(0 && "unknown tbaa call instruction user");
        }
      } else if (auto si = dyn_cast<StoreInst>(&inst)) {
        auto size =
            (dl.getTypeSizeInBits(si->getValueOperand()->getType()) + 7) / 8;
        updateAnalysis(si->getPointerOperand(),
                       vdptr
                           .ShiftIndices(dl, /*init offset*/ 0,
                                         /*max size*/ size, /*new offset*/ 0)
                           .PurgeAnything()
                           .Only(-1),
                       si);
        auto req = vdptr.Only(-1);
        updateAnalysis(si->getValueOperand(), req.Lookup(size, dl), si);
      } else if (auto li = dyn_cast<LoadInst>(&inst)) {
        auto size = (dl.getTypeSizeInBits(li->getType()) + 7) / 8;
        updateAnalysis(li->getPointerOperand(),
                       vdptr
                           .ShiftIndices(dl, /*init offset*/ 0,
                                         /*max size*/ size, /*new offset*/ 0)
                           .PurgeAnything()
                           .Only(-1),
                       li);
        auto req = vdptr.Only(-1);
        updateAnalysis(li, req.Lookup(size, dl), li);
      } else {
        llvm::errs() << " inst: " << inst << " vdptr: " << vdptr.str() << "\n";
        assert(0 && "unknown tbaa instruction user");
      }
    }
  }
}

bool hasAnyUse(TypeAnalyzer &TAZ,
               Value *val, std::map<Value *, bool> &intseen, bool *sawReturn /*if sawReturn != nullptr, we can ignore uses of returninst, setting the bool to true if we see one*/) {
  if (intseen.find(val) != intseen.end())
    return intseen[val];
  // todo what to insert to intseen

  bool unknownUse = false;
  intseen[val] = false;

  for (User *use : val->users()) {
    if (auto ci = dyn_cast<CastInst>(use)) {
      unknownUse |= hasAnyUse(TAZ, ci, intseen, sawReturn);
      continue;
    }

    if (auto pn = dyn_cast<PHINode>(use)) {
      unknownUse |= hasAnyUse(TAZ, pn, intseen, sawReturn);
      continue;
    }

    if (auto seli = dyn_cast<SelectInst>(use)) {
      unknownUse |= hasAnyUse(TAZ, seli, intseen, sawReturn);
      continue;
    }

    if (auto call = dyn_cast<CallInst>(use)) {
      if (Function *ci = call->getCalledFunction()) {
        // These function calls are known uses that do not potentially have an
        // inactive use
        if (ci->getName() == "__cxa_guard_acquire" ||
            ci->getName() == "__cxa_guard_release" ||
            ci->getName() == "__cxa_guard_abort" || ci->getName() == "printf" ||
            ci->getName() == "fprintf") {
          continue;
        }

        // TODO recursive fns

        if (!ci->empty()) {
          auto a = ci->arg_begin();

          bool shouldHandleReturn = false;

          for (size_t i = 0; i < call->getNumArgOperands(); ++i) {
            if (call->getArgOperand(i) == val) {
              if (hasAnyUse(TAZ, a, intseen, &shouldHandleReturn)) {
                return intseen[val] = unknownUse = true;
              }
            }
            ++a;
          }

          if (shouldHandleReturn) {
            if (hasAnyUse(TAZ, call, intseen, sawReturn)) {
              return intseen[val] = unknownUse = true;
            }
          }
          continue;
        }
      }
    }

    if (sawReturn && isa<ReturnInst>(use)) {
      *sawReturn = true;
      continue;
    }

    unknownUse = true;
    // llvm::errs() << "unknown use : " << *use << " of v: " << *v << "\n";
    continue;
  }

  return intseen[val] = unknownUse;
}

bool hasNonIntegralUse(TypeAnalyzer &TAZ,
                       Value *val, std::map<Value *, bool> &intseen, bool *sawReturn /*if sawReturn != nullptr, we can ignore uses of returninst, setting the bool to true if we see one*/) {
  if (intseen.find(val) != intseen.end())
    return intseen[val];
  // todo what to insert to intseen

  bool unknownUse = false;
  intseen[val] = false;

  for (User *use : val->users()) {
    if (auto ci = dyn_cast<CastInst>(use)) {

      if (isa<SIToFPInst>(use) || isa<UIToFPInst>(use)) {
        continue;
      }

      if (isa<FPToSIInst>(use) || isa<FPToUIInst>(use)) {
        continue;
      }

      if (ci->getDestTy()->isPointerTy()) {
        unknownUse = true;
        break;
      }

      unknownUse |= hasNonIntegralUse(TAZ, ci, intseen, sawReturn);
      continue;
    }

    if (auto bi = dyn_cast<BinaryOperator>(use)) {

      unknownUse |= hasNonIntegralUse(TAZ, bi, intseen, sawReturn);
      continue;
    }

    if (auto pn = dyn_cast<PHINode>(use)) {
      unknownUse |= hasNonIntegralUse(TAZ, pn, intseen, sawReturn);
      continue;
    }

    if (auto seli = dyn_cast<SelectInst>(use)) {
      unknownUse |= hasNonIntegralUse(TAZ, seli, intseen, sawReturn);
      continue;
    }

    if (auto gep = dyn_cast<GetElementPtrInst>(use)) {
      if (gep->getPointerOperand() == val) {
        unknownUse = true;
        break;
      }

      // this assumes that the original value doesnt propagate out through the
      // pointer
      continue;
    }

    if (auto call = dyn_cast<CallInst>(use)) {
      if (Function *ci = call->getCalledFunction()) {
        // These function calls are known uses that do not potentially have an
        // inactive use
        if (ci->getName() == "__cxa_guard_acquire" ||
            ci->getName() == "__cxa_guard_release" ||
            ci->getName() == "__cxa_guard_abort" || ci->getName() == "printf" ||
            ci->getName() == "fprintf") {
          continue;
        }

        // TODO recursive fns

        if (!ci->empty()) {
          auto a = ci->arg_begin();

          bool shouldHandleReturn = false;

          for (size_t i = 0; i < call->getNumArgOperands(); ++i) {
            if (call->getArgOperand(i) == val) {
              if (hasNonIntegralUse(TAZ, a, intseen, &shouldHandleReturn)) {
                return intseen[val] = unknownUse = true;
              }
            }
            ++a;
          }

          if (shouldHandleReturn) {
            if (hasNonIntegralUse(TAZ, call, intseen, sawReturn)) {
              return intseen[val] = unknownUse = true;
            }
          }
          continue;
        }
      }
    }

    if (isa<AllocaInst>(use)) {
      continue;
    }

    if (isa<CmpInst>(use))
      continue;
    if (isa<SwitchInst>(use))
      continue;
    if (isa<BranchInst>(use))
      continue;

    if (sawReturn && isa<ReturnInst>(use)) {
      *sawReturn = true;
      continue;
    }

    unknownUse = true;
    // llvm::errs() << "unknown use : " << *use << " of v: " << *v << "\n";
    continue;
  }

  return intseen[val] = unknownUse;
}

bool TypeAnalyzer::runUnusedChecks() {
  // NOTE explicitly NOT doing arguments here
  //  this is done to prevent information being propagated up via IPO that is
  //  incorrect (since there may be other uses in the caller)
  bool changed = false;
  std::vector<Value *> todo;

  std::map<Value *, bool> anyseen;
  std::map<Value *, bool> intseen;

  for (BasicBlock &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {
      auto analysis = getAnalysis(&inst);
      if (analysis[{0}] != BaseType::Unknown)
        continue;

      if (!inst.getType()->isIntOrIntVectorTy())
        continue;

      // This deals with integers representing floats or pointers with no use
      // (and thus can be anything)
      {
        if (!hasAnyUse(*this, &inst, anyseen, nullptr)) {
          updateAnalysis(&inst,
                         TypeTree(BaseType::Anything)
                             .Only(inst.getType()->isIntegerTy() ? -1 : 0),
                         &inst);
          changed = true;
        }
      }

      // This deals with integers with no use
      {
        if (!hasNonIntegralUse(*this, &inst, intseen, nullptr)) {
          updateAnalysis(&inst,
                         TypeTree(BaseType::Integer)
                             .Only(inst.getType()->isIntegerTy() ? -1 : 0),
                         &inst);
          changed = true;
        }
      }
    }
  }

  return changed;
}

void TypeAnalyzer::run() {
  std::deque<CallInst *> pendingCalls;

  do {

    while (workList.size()) {
      auto todo = workList.front();
      workList.pop_front();
      if (auto ci = dyn_cast<CallInst>(todo)) {
        pendingCalls.push_back(ci);
        continue;
      }
      visitValue(*todo);
    }

    if (pendingCalls.size() > 0) {
      auto todo = pendingCalls.front();
      pendingCalls.pop_front();
      visitValue(*todo);
      continue;
    } else
      break;

  } while (1);

  runUnusedChecks();

  do {

    while (workList.size()) {
      auto todo = workList.front();
      workList.pop_front();
      if (auto ci = dyn_cast<CallInst>(todo)) {
        pendingCalls.push_back(ci);
        continue;
      }
      visitValue(*todo);
    }

    if (pendingCalls.size() > 0) {
      auto todo = pendingCalls.front();
      pendingCalls.pop_front();
      visitValue(*todo);
      continue;
    } else
      break;

  } while (1);
}

void TypeAnalyzer::visitValue(Value &val) {
  if (isa<Constant>(&val)) {
    return;
  }

  if (!isa<Argument>(&val) && !isa<Instruction>(&val))
    return;

  // TODO add no users integral here

  if (auto inst = dyn_cast<Instruction>(&val)) {
    visit(*inst);
  }
}

void TypeAnalyzer::visitCmpInst(CmpInst &cmp) {
  updateAnalysis(&cmp, TypeTree(BaseType::Integer).Only(-1), &cmp);
}

void TypeAnalyzer::visitAllocaInst(AllocaInst &I) {
  updateAnalysis(I.getArraySize(), TypeTree(BaseType::Integer).Only(-1), &I);
  updateAnalysis(&I, TypeTree(BaseType::Pointer).Only(-1), &I);
}

void TypeAnalyzer::visitLoadInst(LoadInst &I) {
  auto &dl = I.getParent()->getParent()->getParent()->getDataLayout();
  auto loadSize = (dl.getTypeSizeInBits(I.getType()) + 7) / 8;

  auto ptr = getAnalysis(&I)
                 .ShiftIndices(dl, /*start*/ 0, loadSize, /*addOffset*/ 0)
                 .PurgeAnything();
  ptr |= TypeTree(BaseType::Pointer);
  // llvm::errs() << "LI: " << I << " prev i0: " <<
  // getAnalysis(I.getOperand(0)).str() << " ptr only-1:" << ptr.Only(-1).str()
  // << "\n"; llvm::errs() << "  + " << " prev i: " << getAnalysis(&I).str() <<"
  // ga lu:" <<  getAnalysis(I.getOperand(0)).Lookup(loadSize).str() << "\n";
  updateAnalysis(I.getOperand(0), ptr.Only(-1), &I);
  updateAnalysis(&I, getAnalysis(I.getOperand(0)).Lookup(loadSize, dl), &I);
}

void TypeAnalyzer::visitStoreInst(StoreInst &I) {
  auto &dl = I.getParent()->getParent()->getParent()->getDataLayout();
  auto storeSize =
      (dl.getTypeSizeInBits(I.getValueOperand()->getType()) + 7) / 8;

  auto ptr = TypeTree(BaseType::Pointer);
  auto purged = getAnalysis(I.getValueOperand())
                    .ShiftIndices(dl, /*start*/ 0, storeSize, /*addOffset*/ 0)
                    .PurgeAnything();
  ptr |= purged;

  // llvm::errs() << "considering si: " << I << "\n";
  // llvm::errs() << " prevanalysis: " <<
  // getAnalysis(I.getPointerOperand()).str() << "\n"; llvm::errs() << " new: "
  // << ptr.str() << "\n";

  updateAnalysis(I.getPointerOperand(), ptr.Only(-1), &I);
  updateAnalysis(
      I.getValueOperand(),
      getAnalysis(I.getPointerOperand()).PurgeAnything().Lookup(storeSize, dl),
      &I);
}

template <typename T>
std::set<std::vector<T>> getSet(const std::vector<std::set<T>> &todo,
                                size_t idx) {
  std::set<std::vector<T>> out;
  if (idx == 0) {
    for (auto val : todo[0]) {
      out.insert({val});
    }
    return out;
  }

  auto old = getSet(todo, idx - 1);
  for (const auto &oldv : old) {
    for (auto val : todo[idx]) {
      auto nex = oldv;
      nex.push_back(val);
      out.insert(nex);
    }
  }
  return out;
}

void TypeAnalyzer::visitGetElementPtrInst(GetElementPtrInst &gep) {
  auto &dl = fntypeinfo.function->getParent()->getDataLayout();

  auto pointerAnalysis = getAnalysis(gep.getPointerOperand());
  updateAnalysis(&gep, pointerAnalysis.KeepMinusOne(), &gep);

  // If one of these is known to be a pointer, propagate it
  updateAnalysis(&gep, TypeTree(pointerAnalysis.Data0()[{}]).Only(-1), &gep);
  updateAnalysis(gep.getPointerOperand(),
                 TypeTree(getAnalysis(&gep).Data0()[{}]).Only(-1), &gep);

  if (isa<UndefValue>(gep.getPointerOperand())) {
    return;
  }

  std::vector<std::set<Value *>> idnext;

  // If we know that the pointer operand is indeed a pointer, then the indicies
  // must be integers Note that we can't do this if we don't know the pointer
  // operand is a pointer since doing 1[pointer] is legal
  //  sadly this still may not work since (nullptr)[fn] => fn where fn is
  //  pointer and not int (whereas nullptr is a pointer) However if we are
  //  inbounds you are only allowed to have nullptr[0] or nullptr[nullptr],
  //  making this valid
  // TODO note that we don't force the inttype::pointer (commented below)
  // assuming nullptr[nullptr] doesn't occur in practice
  // if (gep.isInBounds() && pointerAnalysis[{}] == BaseType::Pointer) {
  if (gep.isInBounds()) {
    // llvm::errs() << "gep: " << gep << "\n";
    for (auto &ind : gep.indices()) {
      // llvm::errs() << " + ind: " << *ind << " - prev - " <<
      // getAnalysis(ind).str() << "\n";
      updateAnalysis(ind, TypeTree(BaseType::Integer).Only(-1), &gep);
    }
  }

  for (auto &a : gep.indices()) {
    auto iset = fntypeinfo.knownIntegralValues(a, DT, intseen);
    std::set<Value *> vset;
    for (auto i : iset) {
      // Don't consider negative indices of gep
      if (i < 0)
        continue;
      vset.insert(ConstantInt::get(a->getType(), i));
    }
    idnext.push_back(vset);
    if (idnext.back().size() == 0)
      return;
  }

  for (auto vec : getSet(idnext, idnext.size() - 1)) {
    auto g2 = GetElementPtrInst::Create(nullptr, gep.getOperand(0), vec);
#if LLVM_VERSION_MAJOR > 6
    APInt ai(dl.getIndexSizeInBits(gep.getPointerAddressSpace()), 0);
#else
    APInt ai(dl.getPointerSize(gep.getPointerAddressSpace()) * 8, 0);
#endif
    g2->accumulateConstantOffset(dl, ai);
    // Using destructor rather than eraseFromParent
    //   as g2 has no parent
    delete g2;

    int off = (int)ai.getLimitedValue();

    // TODO also allow negative offsets
    if (off < 0)
      continue;

    int maxSize = -1;
    if (cast<ConstantInt>(vec[0])->getLimitedValue() == 0) {
      maxSize = dl.getTypeAllocSizeInBits(
                    cast<PointerType>(gep.getType())->getElementType()) /
                8;
    }

    auto unmerged = pointerAnalysis.Data0()
                        .ShiftIndices(dl, /*init offset*/ off,
                                      /*max size*/ maxSize, /*newoffset*/ 0)
                        .Only(-1);

    updateAnalysis(&gep, unmerged, &gep);

    auto merged = getAnalysis(&gep)
                      .Data0()
                      .ShiftIndices(dl, /*init offset*/ 0, /*max size*/ -1,
                                    /*new offset*/ off)
                      .Only(-1);

    updateAnalysis(gep.getPointerOperand(), merged, &gep);
  }
}

void TypeAnalyzer::visitPHINode(PHINode &phi) {
  for (auto &op : phi.incoming_values()) {
    updateAnalysis(op, getAnalysis(&phi), &phi);
  }

  assert(phi.getNumIncomingValues() > 0);
  // TODO phi needs reconsidering here
  TypeTree vd;
  bool set = false;

  auto consider = [&](TypeTree &&newData, Value *v) {
    if (set) {
      vd.andIn(newData, /*assertIfIllegal*/ false);
    } else {
      set = true;
      vd = newData;
    }
  };

  // TODO generalize this (and for recursive, etc)
  std::deque<Value *> vals;
  std::set<Value *> seen{&phi};
  for (auto &op : phi.incoming_values()) {
    vals.push_back(op);
  }

  std::vector<BinaryOperator *> bos;

  while (vals.size()) {
    Value *todo = vals.front();
    vals.pop_front();

    if (auto bo = dyn_cast<BinaryOperator>(todo)) {
      if (bo->getOpcode() == BinaryOperator::Add) {
        if (isa<ConstantInt>(bo->getOperand(0))) {
          bos.push_back(bo);
          todo = bo->getOperand(1);
        }
        if (isa<ConstantInt>(bo->getOperand(1))) {
          bos.push_back(bo);
          todo = bo->getOperand(0);
        }
      }
    }

    if (seen.count(todo))
      continue;
    seen.insert(todo);

    if (auto nphi = dyn_cast<PHINode>(todo)) {
      for (auto &op : nphi->incoming_values()) {
        vals.push_back(op);
      }
      continue;
    }
    if (auto sel = dyn_cast<SelectInst>(todo)) {
      vals.push_back(sel->getOperand(1));
      vals.push_back(sel->getOperand(2));
      continue;
    }

    consider(getAnalysis(todo), todo);
  }

  assert(set);
  for (BinaryOperator *bo : bos) {
    TypeTree vd1 = isa<ConstantInt>(bo->getOperand(0))
                        ? getAnalysis(bo->getOperand(0)).Data0()
                        : vd.Data0();
    TypeTree vd2 = isa<ConstantInt>(bo->getOperand(1))
                        ? getAnalysis(bo->getOperand(1)).Data0()
                        : vd.Data0();
    vd1.pointerIntMerge(vd2, bo->getOpcode());
    vd.andIn(vd1.Only(bo->getType()->isIntegerTy() ? -1 : 0),
             /*assertIfIllegal*/ false);
  }

  updateAnalysis(&phi, vd, &phi);
}

void TypeAnalyzer::visitTruncInst(TruncInst &I) {
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitZExtInst(ZExtInst &I) {
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitSExtInst(SExtInst &I) {
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitAddrSpaceCastInst(AddrSpaceCastInst &I) {
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitFPTruncInst(FPTruncInst &I) {
  updateAnalysis(&I, TypeTree(ConcreteType(I.getType())).Only(-1), &I);
  updateAnalysis(I.getOperand(0),
                 TypeTree(ConcreteType(I.getOperand(0)->getType())).Only(-1), &I);
}

void TypeAnalyzer::visitFPToUIInst(FPToUIInst &I) {
  updateAnalysis(&I, TypeTree(BaseType::Integer).Only(-1), &I);
  updateAnalysis(I.getOperand(0),
                 TypeTree(ConcreteType(I.getOperand(0)->getType())).Only(-1), &I);
}

void TypeAnalyzer::visitFPToSIInst(FPToSIInst &I) {
  updateAnalysis(&I, TypeTree(BaseType::Integer).Only(-1), &I);
  updateAnalysis(I.getOperand(0),
                 TypeTree(ConcreteType(I.getOperand(0)->getType())).Only(-1), &I);
}

void TypeAnalyzer::visitUIToFPInst(UIToFPInst &I) {
  updateAnalysis(I.getOperand(0), TypeTree(BaseType::Integer).Only(-1), &I);
  updateAnalysis(&I, TypeTree(ConcreteType(I.getType())).Only(-1), &I);
}

void TypeAnalyzer::visitSIToFPInst(SIToFPInst &I) {
  updateAnalysis(I.getOperand(0), TypeTree(BaseType::Integer).Only(-1), &I);
  updateAnalysis(&I, TypeTree(ConcreteType(I.getType())).Only(-1), &I);
}

void TypeAnalyzer::visitPtrToIntInst(PtrToIntInst &I) {
  // Note it is illegal to assume here that either is a pointer or an int
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitIntToPtrInst(IntToPtrInst &I) {
  // Note it is illegal to assume here that either is a pointer or an int
  updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitBitCastInst(BitCastInst &I) {
  if (I.getType()->isIntOrIntVectorTy() || I.getType()->isFPOrFPVectorTy()) {
    updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
    updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
    return;
  }

  if (I.getType()->isPointerTy() && I.getOperand(0)->getType()->isPointerTy()) {
    Type *et1 = cast<PointerType>(I.getType())->getElementType();
    Type *et2 = cast<PointerType>(I.getOperand(0)->getType())->getElementType();

    updateAnalysis(
        &I,
        getAnalysis(I.getOperand(0))
            .Data0()
            .KeepForCast(fntypeinfo.function->getParent()->getDataLayout(), et2,
                         et1)
            .Only(-1),
        &I);
    updateAnalysis(
        I.getOperand(0),
        getAnalysis(&I)
            .Data0()
            .KeepForCast(fntypeinfo.function->getParent()->getDataLayout(), et1,
                         et2)
            .Only(-1),
        &I);
  }
}

void TypeAnalyzer::visitSelectInst(SelectInst &I) {
  updateAnalysis(I.getTrueValue(), getAnalysis(&I), &I);
  updateAnalysis(I.getFalseValue(), getAnalysis(&I), &I);

  TypeTree vd = getAnalysis(I.getTrueValue());
  vd.andIn(getAnalysis(I.getFalseValue()), /*assertIfIllegal*/ false);
  updateAnalysis(&I, vd, &I);
}

void TypeAnalyzer::visitExtractElementInst(ExtractElementInst &I) {
  updateAnalysis(I.getIndexOperand(), BaseType::Integer, &I);
  updateAnalysis(I.getVectorOperand(), getAnalysis(&I), &I);
  updateAnalysis(&I, getAnalysis(I.getVectorOperand()), &I);
}

void TypeAnalyzer::visitInsertElementInst(InsertElementInst &I) {
  updateAnalysis(I.getOperand(2), BaseType::Integer, &I);

  // if we are inserting into undef/etc the anything should not be propagated
  auto res = getAnalysis(I.getOperand(0)).PurgeAnything();

  res |= getAnalysis(I.getOperand(1));
  // res |= getAnalysis(I.getOperand(1)).Only(idx);
  res |= getAnalysis(&I);

  updateAnalysis(I.getOperand(0), res, &I);
  updateAnalysis(&I, res, &I);
  updateAnalysis(I.getOperand(1), res, &I);
}

void TypeAnalyzer::visitShuffleVectorInst(ShuffleVectorInst &I) {
  updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
  updateAnalysis(I.getOperand(1), getAnalysis(&I), &I);

  TypeTree vd = getAnalysis(I.getOperand(0));
  vd.andIn(getAnalysis(I.getOperand(1)), /*assertIfIllegal*/ false);

  updateAnalysis(&I, vd, &I);
}

void TypeAnalyzer::visitExtractValueInst(ExtractValueInst &I) {
  auto &dl = fntypeinfo.function->getParent()->getDataLayout();
  std::vector<Value *> vec;
  vec.push_back(ConstantInt::get(Type::getInt64Ty(I.getContext()), 0));
  for (auto ind : I.indices()) {
    vec.push_back(ConstantInt::get(Type::getInt32Ty(I.getContext()), ind));
  }
  auto ud = UndefValue::get(PointerType::getUnqual(I.getOperand(0)->getType()));
  auto g2 = GetElementPtrInst::Create(nullptr, ud, vec);
#if LLVM_VERSION_MAJOR > 6
  APInt ai(dl.getIndexSizeInBits(g2->getPointerAddressSpace()), 0);
#else
  APInt ai(dl.getPointerSize(g2->getPointerAddressSpace()) * 8, 0);
#endif
  g2->accumulateConstantOffset(dl, ai);
  // Using destructor rather than eraseFromParent
  //   as g2 has no parent
  delete g2;

  int off = (int)ai.getLimitedValue();

  int size = dl.getTypeSizeInBits(I.getType()) / 8;

  updateAnalysis(&I,
                 getAnalysis(I.getOperand(0))
                     .ShiftIndices(dl, off, size, /*addOffset*/ 0)
                     .CanonicalizeValue(size, dl),
                 &I);
  updateAnalysis(I.getOperand(0),
                 getAnalysis(&I).ShiftIndices(dl, 0, size, off), &I);
}

void TypeAnalyzer::visitInsertValueInst(InsertValueInst &I) {
  auto &dl = fntypeinfo.function->getParent()->getDataLayout();
  std::vector<Value *> vec;
  vec.push_back(ConstantInt::get(Type::getInt64Ty(I.getContext()), 0));
  for (auto ind : I.indices()) {
    vec.push_back(ConstantInt::get(Type::getInt32Ty(I.getContext()), ind));
  }
  auto ud = UndefValue::get(PointerType::getUnqual(I.getOperand(0)->getType()));
  auto g2 = GetElementPtrInst::Create(nullptr, ud, vec);
#if LLVM_VERSION_MAJOR > 6
  APInt ai(dl.getIndexSizeInBits(g2->getPointerAddressSpace()), 0);
#else
  APInt ai(dl.getPointerSize(g2->getPointerAddressSpace()) * 8, 0);
#endif
  g2->accumulateConstantOffset(dl, ai);
  // Using destructor rather than eraseFromParent
  //   as g2 has no parent
  delete g2;

  int off = (int)ai.getLimitedValue();

  int agg_size = dl.getTypeSizeInBits(I.getType()) / 8;
  int ins_size =
      dl.getTypeSizeInBits(I.getInsertedValueOperand()->getType()) / 8;

  updateAnalysis(I.getAggregateOperand(),
                 getAnalysis(&I).Clear(off, off + ins_size, agg_size), &I);
  updateAnalysis(I.getInsertedValueOperand(),
                 getAnalysis(&I)
                     .ShiftIndices(dl, off, ins_size, 0)
                     .CanonicalizeValue(ins_size, dl),
                 &I);

  auto new_res =
      getAnalysis(I.getAggregateOperand()).Clear(off, off + ins_size, agg_size);
  auto shifted = getAnalysis(I.getInsertedValueOperand())
                     .ShiftIndices(dl, 0, ins_size, off);
  new_res |= shifted;
  updateAnalysis(&I, new_res.CanonicalizeValue(agg_size, dl), &I);
}

void TypeAnalyzer::dump() {
  llvm::errs() << "<analysis>\n";
  for (auto &pair : analysis) {
    llvm::errs() << *pair.first << ": " << pair.second.str()
                 << ", intvals: " << to_string(knownIntegralValues(pair.first))
                 << "\n";
  }
  llvm::errs() << "</analysis>\n";
}

void TypeAnalyzer::visitBinaryOperator(BinaryOperator &I) {
  if (I.getOpcode() == BinaryOperator::FAdd ||
      I.getOpcode() == BinaryOperator::FSub ||
      I.getOpcode() == BinaryOperator::FMul ||
      I.getOpcode() == BinaryOperator::FDiv ||
      I.getOpcode() == BinaryOperator::FRem) {
    auto ty = I.getType()->getScalarType();
    assert(ty->isFloatingPointTy());
    ConcreteType dt(ty);
    updateAnalysis(I.getOperand(0), TypeTree(dt).Only(-1), &I);
    updateAnalysis(I.getOperand(1), TypeTree(dt).Only(-1), &I);
    updateAnalysis(&I, TypeTree(dt).Only(-1), &I);
  } else {
    auto analysis = getAnalysis(&I).Data0();
    switch (I.getOpcode()) {
    case BinaryOperator::Sub:
      // TODO propagate this info
      // ptr - ptr => int and int - int => int; thus int = a - b says only that
      // these are equal ptr - int => ptr and int - ptr => ptr; thus
      analysis = ConcreteType(BaseType::Unknown);
      break;

    case BinaryOperator::Add:
    case BinaryOperator::Mul:
      // if a + b or a * b == int, then a and b must be ints
      analysis = analysis.JustInt();
      break;

    case BinaryOperator::UDiv:
    case BinaryOperator::SDiv:
    case BinaryOperator::URem:
    case BinaryOperator::SRem:
    case BinaryOperator::And:
    case BinaryOperator::Or:
    case BinaryOperator::Xor:
    case BinaryOperator::Shl:
    case BinaryOperator::AShr:
    case BinaryOperator::LShr:
      analysis = ConcreteType(BaseType::Unknown);
      break;
    default:
      llvm_unreachable("unknown binary operator");
    }
    updateAnalysis(I.getOperand(0), analysis.Only(-1), &I);
    updateAnalysis(I.getOperand(1), analysis.Only(-1), &I);

    TypeTree vd = getAnalysis(I.getOperand(0)).Data0();
    vd.pointerIntMerge(getAnalysis(I.getOperand(1)).Data0(), I.getOpcode());

    if (I.getOpcode() == BinaryOperator::And) {
      for (int i = 0; i < 2; ++i) {
        for (auto andval :
             fntypeinfo.knownIntegralValues(I.getOperand(i), DT, intseen)) {
          if (andval <= 16 && andval >= 0) {

            vd |= TypeTree(BaseType::Integer);
          }
        }
      }
    }
    updateAnalysis(&I, vd.Only(-1), &I);
  }
}

void TypeAnalyzer::visitMemTransferInst(llvm::MemTransferInst &MTI) {
  // If memcpy / memmove of pointer, we can propagate type information from src
  // to dst up to the length and vice versa
  size_t sz = 1;
  for (auto val : fntypeinfo.knownIntegralValues(MTI.getArgOperand(2), DT, intseen)) {
    assert(val >= 0);
    sz = max(sz, (size_t)val);
  }

  TypeTree res = getAnalysis(MTI.getArgOperand(0)).AtMost(sz).PurgeAnything();
  TypeTree res2 = getAnalysis(MTI.getArgOperand(1)).AtMost(sz).PurgeAnything();
  res |= res2;

  updateAnalysis(MTI.getArgOperand(0), res, &MTI);
  updateAnalysis(MTI.getArgOperand(1), res, &MTI);
  for (unsigned i = 2; i < MTI.getNumArgOperands(); ++i) {
    updateAnalysis(MTI.getArgOperand(i), TypeTree(BaseType::Integer).Only(-1),
                   &MTI);
  }
}

void TypeAnalyzer::visitIntrinsicInst(llvm::IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::log:
  case Intrinsic::log2:
  case Intrinsic::log10:
  case Intrinsic::exp:
  case Intrinsic::exp2:
  case Intrinsic::sin:
  case Intrinsic::cos:
  case Intrinsic::floor:
  case Intrinsic::ceil:
  case Intrinsic::trunc:
  case Intrinsic::rint:
  case Intrinsic::nearbyint:
  case Intrinsic::round:
  case Intrinsic::sqrt:
  case Intrinsic::fabs:
    updateAnalysis(
        &I, TypeTree(ConcreteType(I.getType()->getScalarType())).Only(-1), &I);
    updateAnalysis(
        I.getOperand(0),
        TypeTree(ConcreteType(I.getOperand(0)->getType()->getScalarType()))
            .Only(-1),
        &I);
    return;

  case Intrinsic::powi:
    updateAnalysis(
        &I, TypeTree(ConcreteType(I.getType()->getScalarType())).Only(-1), &I);
    updateAnalysis(
        I.getOperand(0),
        TypeTree(ConcreteType(I.getOperand(0)->getType()->getScalarType()))
            .Only(-1),
        &I);
    updateAnalysis(
        I.getOperand(1),
        TypeTree(BaseType::Integer)
            .Only(-1),
        &I);
    return;

#if LLVM_VERSION_MAJOR < 10
  case Intrinsic::x86_sse_max_ss:
  case Intrinsic::x86_sse_max_ps:
  case Intrinsic::x86_sse_min_ss:
  case Intrinsic::x86_sse_min_ps:
#endif
#if LLVM_VERSION_MAJOR >= 9
  case Intrinsic::experimental_vector_reduce_v2_fadd:
#endif
  case Intrinsic::maxnum:
  case Intrinsic::minnum:
  case Intrinsic::pow:
    updateAnalysis(
        &I, TypeTree(ConcreteType(I.getType()->getScalarType())).Only(-1), &I);
    updateAnalysis(
        I.getOperand(0),
        TypeTree(ConcreteType(I.getOperand(0)->getType()->getScalarType()))
            .Only(-1),
        &I);
    updateAnalysis(
        I.getOperand(1),
        TypeTree(ConcreteType(I.getOperand(1)->getType()->getScalarType()))
            .Only(-1),
        &I);
    return;
  case Intrinsic::umul_with_overflow:
  case Intrinsic::smul_with_overflow:
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow: {
    // val, bool
    auto analysis = getAnalysis(&I).Data0();

    BinaryOperator::BinaryOps opcode;

    switch (I.getIntrinsicID()) {
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::usub_with_overflow: {
      // TODO propagate this info
      // ptr - ptr => int and int - int => int; thus int = a - b says only that
      // these are equal ptr - int => ptr and int - ptr => ptr; thus
      analysis = ConcreteType(BaseType::Unknown);
      opcode = BinaryOperator::Sub;
      break;
    }

    case Intrinsic::smul_with_overflow:
    case Intrinsic::umul_with_overflow: {
      opcode = BinaryOperator::Mul;
      // if a + b or a * b == int, then a and b must be ints
      analysis = analysis.JustInt();
      break;
    }
    case Intrinsic::sadd_with_overflow:
    case Intrinsic::uadd_with_overflow: {
      opcode = BinaryOperator::Add;
      // if a + b or a * b == int, then a and b must be ints
      analysis = analysis.JustInt();
      break;
    }
    default:
      llvm_unreachable("unknown binary operator");
    }

    updateAnalysis(I.getOperand(0), analysis.Only(-1), &I);
    updateAnalysis(I.getOperand(1), analysis.Only(-1), &I);

    TypeTree vd = getAnalysis(I.getOperand(0)).Data0();
    vd.pointerIntMerge(getAnalysis(I.getOperand(1)).Data0(), opcode);

    TypeTree overall = vd.Only(0);

    auto &dl = I.getParent()->getParent()->getParent()->getDataLayout();
    overall |=
        TypeTree(BaseType::Integer)
            .Only((dl.getTypeSizeInBits(I.getOperand(0)->getType()) + 7) / 8);

    updateAnalysis(&I, overall, &I);
    return;
  }
  default:
    return;
  }
}

template <typename T> struct TypeHandler {};

template <> struct TypeHandler<double> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TA.updateAnalysis(
        val, TypeTree(ConcreteType(Type::getDoubleTy(call.getContext()))).Only(-1),
        &call);
  }
};

template <> struct TypeHandler<float> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TA.updateAnalysis(
        val, TypeTree(ConcreteType(Type::getFloatTy(call.getContext()))).Only(-1),
        &call);
  }
};

template <> struct TypeHandler<long double> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TA.updateAnalysis(
        val,
        TypeTree(ConcreteType(Type::getX86_FP80Ty(call.getContext()))).Only(-1),
        &call);
  }
};

#if defined(__FLOAT128__) || defined(__SIZEOF_FLOAT128__)
template <> struct TypeHandler<__float128> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TA.updateAnalysis(
        val, TypeTree(ConcreteType(Type::getFP128Ty(call.getContext()))).Only(-1),
        &call);
  }
};
#endif

template <> struct TypeHandler<double *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(Type::getDoubleTy(call.getContext())).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<float *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(Type::getFloatTy(call.getContext())).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long double *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(Type::getX86_FP80Ty(call.getContext())).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

#if defined(__FLOAT128__) || defined(__SIZEOF_FLOAT128__)
template <> struct TypeHandler<__float128 *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(Type::getFP128Ty(call.getContext())).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};
#endif

template <> struct TypeHandler<void> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {}
};

template <> struct TypeHandler<void *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<unsigned int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<unsigned int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long unsigned int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long unsigned int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long long int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long long int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long long unsigned int> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <> struct TypeHandler<long long unsigned int *> {
  static void analyzeType(Value *val, CallInst &call, TypeAnalyzer &TA) {
    TypeTree vd = TypeTree(BaseType::Integer).Only(0);
    vd |= TypeTree(BaseType::Pointer);
    TA.updateAnalysis(val, vd.Only(-1), &call);
  }
};

template <typename... Arg0> struct FunctionArgumentIterator {
  static void analyzeFuncTypesHelper(unsigned idx, CallInst &call,
                                     TypeAnalyzer &TA) {}
};

template <typename Arg0, typename... Args>
struct FunctionArgumentIterator<Arg0, Args...> {
  static void analyzeFuncTypesHelper(unsigned idx, CallInst &call,
                                     TypeAnalyzer &TA) {
    TypeHandler<Arg0>::analyzeType(call.getOperand(idx), call, TA);
    FunctionArgumentIterator<Args...>::analyzeFuncTypesHelper(idx + 1, call, TA);
  }
};

template <typename RT, typename... Args>
void analyzeFuncTypes(RT (*fn)(Args...), CallInst &call, TypeAnalyzer &TA) {
  TypeHandler<RT>::analyzeType(&call, call, TA);
  FunctionArgumentIterator<Args...>::analyzeFuncTypesHelper(0, call, TA);
}

void TypeAnalyzer::visitCallInst(CallInst &call) {
  assert(fntypeinfo.knownValues.size() ==
         fntypeinfo.function->getFunctionType()->getNumParams());

  #if LLVM_VERSION_MAJOR >= 11
  if (auto iasm = dyn_cast<InlineAsm>(call.getCalledOperand())) {
  #else
  if (auto iasm = dyn_cast<InlineAsm>(call.getCalledValue())) {
  #endif
    if (iasm->getAsmString() == "cpuid") {
      updateAnalysis(&call, TypeTree(BaseType::Integer).Only(-1), &call);
      for (unsigned i = 0; i < call.getNumArgOperands(); ++i) {
        updateAnalysis(call.getArgOperand(i),
                       TypeTree(BaseType::Integer).Only(-1), &call);
      }
    }
  }

  if (Function *ci = call.getCalledFunction()) {

#define CONSIDER(fn)                                                           \
  if (ci->getName() == #fn) {                                                  \
    analyzeFuncTypes(::fn, call, *this);                                       \
    return;                                                                    \
  }

  #define CONSIDER2(fn, ...)                                                           \
  if (ci->getName() == #fn) {                                                  \
    analyzeFuncTypes<__VA_ARGS__>(::fn, call, *this);                                       \
    return;                                                                    \
  }

    CONSIDER(malloc)
    // CONSIDER(__lgamma_r_finite)
    CONSIDER2(frexp, double, double, int*)

    CONSIDER(frexpf)
    CONSIDER(frexpl)
    CONSIDER2(ldexp, double, double, int)
    CONSIDER2(modf, double, double, double*)

    CONSIDER2(cos, double, double)
    CONSIDER2(sin, double, double)
    CONSIDER2(tan, double, double)
    CONSIDER2(acos, double, double)
    CONSIDER2(asin, double, double)
    CONSIDER2(atan, double, double)
    CONSIDER2(atan2, double, double, double)
    CONSIDER2(cosh, double, double)
    CONSIDER2(sinh, double, double)
    CONSIDER2(tanh, double, double)
    CONSIDER(tanhf)
    CONSIDER2(acosh, double, double)
    CONSIDER(acoshf)
    CONSIDER(acoshl)
    CONSIDER2(asinh, double, double)
    CONSIDER(asinhf)
    CONSIDER(asinhl)
    CONSIDER2(atanh, double, double)
    CONSIDER(atanhl)
    CONSIDER(atanhf)
    CONSIDER2(exp, double, double)
    CONSIDER2(log, double, double)
    CONSIDER2(log10, double, double)
    CONSIDER2(exp2, double, double)
    CONSIDER(exp2f)
    CONSIDER(exp2l)
    CONSIDER2(expm1, double, double)
    CONSIDER(expm1f)
    CONSIDER(expm1l)
    CONSIDER2(ilogb, int, double)
    CONSIDER(ilogbf)
    CONSIDER(ilogbl)
    CONSIDER2(log1p, double, double)
    CONSIDER(log1pf)
    CONSIDER(log1pl)
    CONSIDER2(log2, double, double)
    CONSIDER(log2f)
    CONSIDER(log2l)
    CONSIDER2(logb, double, double)
    CONSIDER(logbf)
    CONSIDER(logbl)
    CONSIDER2(scalbn, double, double, int)
    CONSIDER(scalbnf)
    CONSIDER(scalbnl)
    CONSIDER2(scalbln, double, double, long)
    CONSIDER(scalblnf)
    CONSIDER(scalblnl)
    CONSIDER2(pow, double, double, double)
    CONSIDER2(sqrt, double, double)
    CONSIDER2(cbrt, double, double)
    CONSIDER(cbrtf)
    CONSIDER(cbrtl)
    CONSIDER2(hypot, double, double, double)
    CONSIDER2(erf, double, double)
    CONSIDER(erff)
    CONSIDER(erfl)
    CONSIDER2(erfc, double, double)
    CONSIDER(erfcf)
    CONSIDER(erfcl)
    CONSIDER2(tgamma, double, double)
    CONSIDER(tgammaf)
    CONSIDER(tgammal)
    CONSIDER2(lgamma, double, double)
    CONSIDER(lgammaf)
    CONSIDER(lgammal)
    CONSIDER2(ceil, double, double)
    CONSIDER2(floor, double, double)
    CONSIDER2(fmod, double, double, double)
    CONSIDER2(trunc, double, double)
    CONSIDER(truncf)
    CONSIDER(truncl)
    CONSIDER2(round, double, double)
    CONSIDER(roundf)
    CONSIDER(roundl)
    CONSIDER2(lround, long, double)
    CONSIDER(lroundf)
    CONSIDER(lroundl)
    CONSIDER2(llround, long long, double)
    CONSIDER(llroundf)
    CONSIDER(llroundl)
    CONSIDER2(rint, double, double)
    CONSIDER(rintf)
    CONSIDER(rintl)
    CONSIDER2(lrint, long, double)
    CONSIDER(lrintf)
    CONSIDER(lrintl)
    CONSIDER2(llrint, long long, double)
    CONSIDER(llrintf)
    CONSIDER(llrintl)
    CONSIDER2(remainder, double, double, double)
    CONSIDER(remainderf)
    CONSIDER(remainderl)
    CONSIDER2(remquo, double, double, double, int*)
    CONSIDER(remquof)
    CONSIDER(remquol)
    CONSIDER2(copysign, double, double, double)
    CONSIDER(copysignf)
    CONSIDER(copysignl)
    CONSIDER2(nextafter, double, double, double)
    CONSIDER(nextafterf)
    CONSIDER(nextafterl)
    CONSIDER2(nexttoward, double, double, long double)
    CONSIDER(nexttowardf)
    CONSIDER(nexttowardl)
    CONSIDER2(fdim, double, double, double)
    CONSIDER(fdimf)
    CONSIDER(fdiml)
    CONSIDER2(fmax, double, double, double)
    CONSIDER(fmaxf)
    CONSIDER(fmaxl)
    CONSIDER2(fmin, double, double, double)
    CONSIDER(fminf)
    CONSIDER(fminl)
    CONSIDER2(fabs, double, double)
    CONSIDER2(fma, double, double, double, double)
    CONSIDER(fmaf)
    CONSIDER(fmal)

    if (ci->getName() == "__lgamma_r_finite") {
      updateAnalysis(
          call.getArgOperand(0),
          TypeTree(ConcreteType(Type::getDoubleTy(call.getContext()))).Only(-1),
          &call);
      updateAnalysis(call.getArgOperand(1),
                     TypeTree(BaseType::Integer).Only(0).Only(-1), &call);
      updateAnalysis(
          &call,
          TypeTree(ConcreteType(Type::getDoubleTy(call.getContext()))).Only(-1),
          &call);
    }

    if (!ci->empty()) {
      visitIPOCall(call, *ci);
    }
  }
}

TypeTree TypeAnalyzer::getReturnAnalysis() {
  bool set = false;
  TypeTree vd;
  for (BasicBlock &BB : *fntypeinfo.function) {
    for (auto &inst : BB) {
      if (auto ri = dyn_cast<ReturnInst>(&inst)) {
        if (auto rv = ri->getReturnValue()) {
          if (set == false) {
            set = true;
            vd = getAnalysis(rv);
            continue;
          }
          vd.andIn(getAnalysis(rv), /*assertIfIllegal*/ false);
        }
      }
    }
  }
  return vd;
}

std::set<int64_t> FnTypeInfo::knownIntegralValues(
    llvm::Value *val, const DominatorTree &DT,
    std::map<Value *, std::set<int64_t>> &intseen) const {
  if (auto constant = dyn_cast<ConstantInt>(val)) {
    return {constant->getSExtValue()};
  }

  assert(knownValues.size() == function->getFunctionType()->getNumParams());

  if (auto arg = dyn_cast<llvm::Argument>(val)) {
    auto found = knownValues.find(arg);
    if (found == knownValues.end()) {
      for (const auto &pair : knownValues) {
        llvm::errs() << " knownValues[" << *pair.first << "] - "
                     << pair.first->getParent()->getName() << "\n";
      }
      llvm::errs() << " arg: " << *arg << " - " << arg->getParent()->getName()
                   << "\n";
    }
    assert(found != knownValues.end());
    return found->second;
  }

  if (intseen.find(val) != intseen.end())
    return intseen[val];
  intseen[val] = {};

  if (auto ci = dyn_cast<CastInst>(val)) {
    intseen[val] = knownIntegralValues(ci->getOperand(0), DT, intseen);
  }

  auto insert = [&](int64_t v) {
    if (v > -100 && v < 100) {
      intseen[val].insert(v);
    }
  };

  if (auto pn = dyn_cast<PHINode>(val)) {
    for (unsigned i = 0; i < pn->getNumIncomingValues(); ++i) {
      auto a = pn->getIncomingValue(i);
      auto b = pn->getIncomingBlock(i);

      // do not consider loop incoming edges
      if (pn->getParent() == b || DT.dominates(pn, b)) {
        continue;
      }

      auto inset = knownIntegralValues(a, DT, intseen);

      // TODO this here is not fully justified yet
      for (auto pval : inset) {
        if (pval < 20 && pval > -20) {
          insert(pval);
        }
      }

      // if we are an iteration variable, suppose that it could be zero in that
      // range
      // TODO: could actually check the range intercepts 0
      if (auto bo = dyn_cast<BinaryOperator>(a)) {
        if (bo->getOperand(0) == pn || bo->getOperand(1) == pn) {
          if (bo->getOpcode() == BinaryOperator::Add ||
              bo->getOpcode() == BinaryOperator::Sub) {
            insert(0);
          }
        }
      }
    }
    return intseen[val];
  }

  if (auto bo = dyn_cast<BinaryOperator>(val)) {
    auto inset0 = knownIntegralValues(bo->getOperand(0), DT, intseen);
    auto inset1 = knownIntegralValues(bo->getOperand(1), DT, intseen);
    if (bo->getOpcode() == BinaryOperator::Mul) {

      if (inset0.size() == 1 || inset1.size() == 1) {
        for (auto val0 : inset0) {
          for (auto val1 : inset1) {

            insert(val0 * val1);
          }
        }
      }
      if (inset0.count(0) || inset1.count(0)) {
        intseen[val].insert(0);
      }
    }

    if (bo->getOpcode() == BinaryOperator::Add) {
      if (inset0.size() == 1 || inset1.size() == 1) {
        for (auto val0 : inset0) {
          for (auto val1 : inset1) {
            insert(val0 + val1);
          }
        }
      }
    }
    if (bo->getOpcode() == BinaryOperator::Sub) {
      if (inset0.size() == 1 || inset1.size() == 1) {
        for (auto val0 : inset0) {
          for (auto val1 : inset1) {
            insert(val0 - val1);
          }
        }
      }
    }

    if (bo->getOpcode() == BinaryOperator::Shl) {
      if (inset0.size() == 1 || inset1.size() == 1) {
        for (auto val0 : inset0) {
          for (auto val1 : inset1) {
            insert(val0 << val1);
          }
        }
      }
    }

    // TODO note C++ doesnt guarantee behavior of >> being arithmetic or logical
    //     and should replace with llvm apint internal
    if (bo->getOpcode() == BinaryOperator::AShr ||
        bo->getOpcode() == BinaryOperator::LShr) {
      if (inset0.size() == 1 || inset1.size() == 1) {
        for (auto val0 : inset0) {
          for (auto val1 : inset1) {
            insert(val0 >> val1);
          }
        }
      }
    }
  }

  return intseen[val];
}

void TypeAnalyzer::visitIPOCall(CallInst &call, Function &fn) {
  assert(fntypeinfo.knownValues.size() ==
         fntypeinfo.function->getFunctionType()->getNumParams());

  FnTypeInfo typeInfo(&fn);

  int argnum = 0;
  for (auto &arg : fn.args()) {
    auto dt = getAnalysis(call.getArgOperand(argnum));
    typeInfo.first.insert(std::pair<Argument *, TypeTree>(&arg, dt));
    typeInfo.knownValues.insert(std::pair<Argument *, std::set<int64_t>>(
        &arg,
        fntypeinfo.knownIntegralValues(call.getArgOperand(argnum), DT, intseen)));
    ++argnum;
  }

  typeInfo.second = getAnalysis(&call);

  if (printtype)
    llvm::errs() << " starting IPO of " << call << "\n";

  auto a = fn.arg_begin();
  for (size_t i = 0; i < call.getNumArgOperands(); ++i) {
    auto dt = interprocedural.query(a, typeInfo);
    updateAnalysis(call.getArgOperand(i), dt, &call);
    ++a;
  }

  TypeTree vd = interprocedural.getReturnAnalysis(typeInfo);
  updateAnalysis(&call, vd, &call);
}

TypeResults TypeAnalysis::analyzeFunction(const FnTypeInfo &fn) {

  auto found = analyzedFunctions.find(fn);
  if (found != analyzedFunctions.end()) {
    auto &analysis = found->second;
    if (analysis.fntypeinfo.function != fn.function) {
      llvm::errs() << " queryFunc: " << *fn.function << "\n";
      llvm::errs() << " analysisFunc: " << *analysis.fntypeinfo.function
                   << "\n";
    }
    assert(analysis.fntypeinfo.function == fn.function);

    return TypeResults(*this, fn);
  }

  auto res = analyzedFunctions.emplace(fn, TypeAnalyzer(fn, *this));
  auto &analysis = res.first->second;

  if (printtype) {
    llvm::errs() << "analyzing function " << fn.function->getName() << "\n";
    for (auto &pair : fn.first) {
      llvm::errs() << " + knowndata: " << *pair.first << " : "
                   << pair.second.str();
      auto found = fn.knownValues.find(pair.first);
      if (found != fn.knownValues.end()) {
        llvm::errs() << " - " << to_string(found->second);
      }
      llvm::errs() << "\n";
    }
    llvm::errs() << " + retdata: " << fn.second.str() << "\n";
  }

  analysis.prepareArgs();
  analysis.considerTBAA();
  analysis.run();

  if (analysis.fntypeinfo.function != fn.function) {
    llvm::errs() << " queryFunc: " << *fn.function << "\n";
    llvm::errs() << " analysisFunc: " << *analysis.fntypeinfo.function << "\n";
  }
  assert(analysis.fntypeinfo.function == fn.function);

  {
    auto &analysis = analyzedFunctions.find(fn)->second;
    if (analysis.fntypeinfo.function != fn.function) {
      llvm::errs() << " queryFunc: " << *fn.function << "\n";
      llvm::errs() << " analysisFunc: " << *analysis.fntypeinfo.function
                   << "\n";
    }
    assert(analysis.fntypeinfo.function == fn.function);
  }

  return TypeResults(*this, fn);
}

TypeTree TypeAnalysis::query(Value *val, const FnTypeInfo &fn) {
  assert(val);
  assert(val->getType());

  if (auto con = dyn_cast<Constant>(val)) {
    return getConstantAnalysis(con, fn, *this);
  }

  Function *func = nullptr;
  if (auto arg = dyn_cast<Argument>(val))
    func = arg->getParent();
  else if (auto inst = dyn_cast<Instruction>(val))
    func = inst->getParent()->getParent();
  else {
    llvm::errs() << "unknown value: " << *val << "\n";
    assert(0 && "could not handle unknown value type");
  }

  analyzeFunction(fn);
  auto &found = analyzedFunctions.find(fn)->second;
  if (func && found.fntypeinfo.function != func) {
    llvm::errs() << " queryFunc: " << *func << "\n";
    llvm::errs() << " foundFunc: " << *found.fntypeinfo.function << "\n";
  }
  assert(!func || found.fntypeinfo.function == func);
  return found.getAnalysis(val);
}

ConcreteType TypeAnalysis::intType(Value *val, const FnTypeInfo &fn,
                               bool errIfNotFound) {
  assert(val);
  assert(val->getType());
  auto q = query(val, fn).Data0();
  auto dt = q[{}];
  // dump();
  if (errIfNotFound && (!dt.isKnown() || dt.typeEnum == BaseType::Anything)) {
    if (auto inst = dyn_cast<Instruction>(val)) {
      llvm::errs() << *inst->getParent()->getParent()->getParent() << "\n";
      llvm::errs() << *inst->getParent()->getParent() << "\n";
      for (auto &pair : analyzedFunctions.find(fn)->second.analysis) {
        llvm::errs() << "val: " << *pair.first << " - " << pair.second.str()
                     << "\n";
      }
    }
    llvm::errs() << "could not deduce type of integer " << *val << "\n";
    assert(0 && "could not deduce type of integer");
  }
  return dt;
}

ConcreteType TypeAnalysis::firstPointer(size_t num, Value *val,
                                    const FnTypeInfo &fn, bool errIfNotFound,
                                    bool pointerIntSame) {
  assert(val);
  assert(val->getType());
  assert(val->getType()->isPointerTy());
  auto q = query(val, fn).Data0();
  auto dt = q[{0}];
  dt.mergeIn(q[{-1}], pointerIntSame);
  for (size_t i = 1; i < num; ++i) {
    dt.mergeIn(q[{(int)i}], pointerIntSame);
  }

  if (errIfNotFound && (!dt.isKnown() || dt.typeEnum == BaseType::Anything)) {
    auto &res = analyzedFunctions.find(fn)->second;
    if (auto inst = dyn_cast<Instruction>(val)) {
      llvm::errs() << *inst->getParent()->getParent() << "\n";
      for (auto &pair : res.analysis) {
        if (auto in = dyn_cast<Instruction>(pair.first)) {
          if (in->getParent()->getParent() != inst->getParent()->getParent()) {
            llvm::errs() << "inf: " << *in->getParent()->getParent() << "\n";
            llvm::errs() << "instf: " << *inst->getParent()->getParent()
                         << "\n";
            llvm::errs() << "in: " << *in << "\n";
            llvm::errs() << "inst: " << *inst << "\n";
          }
          assert(in->getParent()->getParent() ==
                 inst->getParent()->getParent());
        }
        llvm::errs() << "val: " << *pair.first << " - " << pair.second.str()
                     << " int: " + to_string(res.knownIntegralValues(pair.first))
                     << "\n";
      }
    }
    if (auto arg = dyn_cast<Argument>(val)) {
      llvm::errs() << *arg->getParent() << "\n";
      for (auto &pair : res.analysis) {
        if (auto in = dyn_cast<Instruction>(pair.first))
          assert(in->getParent()->getParent() == arg->getParent());
        llvm::errs() << "val: " << *pair.first << " - " << pair.second.str()
                     << " int: " + to_string(res.knownIntegralValues(pair.first))
                     << "\n";
      }
    }
    llvm::errs() << "could not deduce type of integer " << *val
                 << " num:" << num << " q:" << q.str() << " \n";
    assert(0 && "could not deduce type of integer");
  }
  return dt;
}

TypeResults::TypeResults(TypeAnalysis &analysis, const FnTypeInfo &fn)
    : analysis(analysis), info(fn) {}

FnTypeInfo TypeResults::getAnalyzedTypeInfo() {
  FnTypeInfo res(info.function);
  for (auto &arg : info.function->args()) {
    res.first.insert(
        std::pair<Argument *, TypeTree>(&arg, analysis.query(&arg, info)));
  }
  res.second = getReturnAnalysis();
  res.knownValues = info.knownValues;
  return res;
}

TypeTree TypeResults::query(Value *val) {
  if (auto inst = dyn_cast<Instruction>(val)) {
    assert(inst->getParent()->getParent() == info.function);
  }
  if (auto arg = dyn_cast<Argument>(val)) {
    assert(arg->getParent() == info.function);
  }
  for (auto &pair : info.first) {
    assert(pair.first->getParent() == info.function);
  }
  return analysis.query(val, info);
}

void TypeResults::dump() {
  assert(analysis.analyzedFunctions.find(info) !=
         analysis.analyzedFunctions.end());
  analysis.analyzedFunctions.find(info)->second.dump();
}

ConcreteType TypeResults::intType(Value *val, bool errIfNotFound) {
  return analysis.intType(val, info, errIfNotFound);
}

ConcreteType TypeResults::firstPointer(size_t num, Value *val, bool errIfNotFound,
                                   bool pointerIntSame) {
  return analysis.firstPointer(num, val, info, errIfNotFound, pointerIntSame);
}

TypeTree TypeResults::getReturnAnalysis() {
  return analysis.getReturnAnalysis(info);
}

std::set<int64_t> TypeResults::knownIntegralValues(Value *val) const {
  auto found = analysis.analyzedFunctions.find(info);
  assert(found != analysis.analyzedFunctions.end());
  auto &sub = found->second;
  return sub.knownIntegralValues(val);
}

std::set<int64_t> TypeAnalyzer::knownIntegralValues(Value *val) {
  return fntypeinfo.knownIntegralValues(val, DT, intseen);
}
