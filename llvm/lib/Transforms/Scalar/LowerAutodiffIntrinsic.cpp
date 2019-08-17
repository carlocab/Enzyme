/*
 * LowerAutodiffIntrinsic.cpp - Lower autodiff intrinsic
 * 
 * Copyright (C) 2019 William S. Moses (enzyme@wsmoses.com) - All Rights Reserved
 *
 * For commercial use of this code please contact the author(s) above.
 *
 * For research use of the code please use the following citation.
 *
 * \misc{mosesenzyme,
    author = {William S. Moses, Tim Kaler},
    title = {Enzyme: LLVM Automatic Differentiation},
    year = {2019},
    howpublished = {\url{https://github.com/wsmoses/AutoDiff/}},
    note = {commit xxxxxxx}
 */

#include "llvm/Transforms/Scalar/LowerAutodiffIntrinsic.h"

#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/PhiValues.h"

#include "llvm/Transforms/Utils.h"

#include "llvm/InitializePasses.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Verifier.h"
//#include "llvm/Transforms/Utils/EaryCSE.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"

#include "llvm/ADT/SmallSet.h"
using namespace llvm;

#define DEBUG_TYPE "lower-autodiff-intrinsic"

#include <utility>
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"

#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/DeadStoreElimination.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Transforms/Scalar/CorrelatedValuePropagation.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"

static cl::opt<bool> autodiff_inline(
            "autodiff_inline", cl::init(false), cl::Hidden,
                cl::desc("Force inlining of autodiff"));

static cl::opt<bool> printconst(
            "autodiff_printconst", cl::init(false), cl::Hidden,
                cl::desc("Print constant detection algorithm"));

static cl::opt<bool> autodiff_print(
            "autodiff_print", cl::init(false), cl::Hidden,
                cl::desc("Print before and after fns for autodiff"));

enum class DIFFE_TYPE {
  OUT_DIFF=0, // add differential to output struct
  DUP_ARG=1,  // duplicate the argument and store differential inside
  CONSTANT=2  // no differential
};

//note this doesn't handle recursive types!
static inline DIFFE_TYPE whatType(llvm::Type* arg) {
  if (arg->isPointerTy()) {
    switch(whatType(cast<llvm::PointerType>(arg)->getElementType())) {
      case DIFFE_TYPE::OUT_DIFF:
        return DIFFE_TYPE::DUP_ARG;
      case DIFFE_TYPE::CONSTANT:
        return DIFFE_TYPE::CONSTANT;
      case DIFFE_TYPE::DUP_ARG:
        return DIFFE_TYPE::DUP_ARG;
    }
    assert(arg);
    llvm::errs() << "arg: " << *arg << "\n";
    assert(0 && "Cannot handle type0");
    return DIFFE_TYPE::CONSTANT;
  } else if (arg->isArrayTy()) {
    return whatType(cast<llvm::ArrayType>(arg)->getElementType());
  } else if (arg->isStructTy()) {
    auto st = cast<llvm::StructType>(arg);
    if (st->getNumElements() == 0) return DIFFE_TYPE::CONSTANT;

    auto ty = DIFFE_TYPE::CONSTANT;
    for(unsigned i=0; i<st->getNumElements(); i++) {
      switch(whatType(st->getElementType(i))) {
        case DIFFE_TYPE::OUT_DIFF:
              switch(ty) {
                case DIFFE_TYPE::OUT_DIFF:
                case DIFFE_TYPE::CONSTANT:
                  ty = DIFFE_TYPE::OUT_DIFF;
                  break;
                case DIFFE_TYPE::DUP_ARG:
                  ty = DIFFE_TYPE::DUP_ARG;
                  return ty;
              }
        case DIFFE_TYPE::CONSTANT:
              switch(ty) {
                case DIFFE_TYPE::OUT_DIFF:
                  ty = DIFFE_TYPE::OUT_DIFF;
                  break;
                case DIFFE_TYPE::CONSTANT:
                  break;
                case DIFFE_TYPE::DUP_ARG:
                  ty = DIFFE_TYPE::DUP_ARG;
                  return ty;
              }
        case DIFFE_TYPE::DUP_ARG:
            return DIFFE_TYPE::DUP_ARG;
      }
    }

    return ty;
  } else if (arg->isIntOrIntVectorTy() || arg->isFunctionTy ()) {
    return DIFFE_TYPE::CONSTANT;
  } else if  (arg->isFPOrFPVectorTy()) {
    return DIFFE_TYPE::OUT_DIFF;
  } else {
    assert(arg);
    llvm::errs() << "arg: " << *arg << "\n";
    assert(0 && "Cannot handle type");
    return DIFFE_TYPE::CONSTANT;
  }
}

bool isReturned(Instruction *inst) {
	for (const auto &a:inst->users()) {
		if(isa<ReturnInst>(a))
			return true;
	}
	return false;
}

bool isconstantValueM(Value* val, SmallPtrSetImpl<Value*> &constants, SmallPtrSetImpl<Value*> &nonconstant, const SmallPtrSetImpl<Value*> &retvals, const SmallPtrSetImpl<Instruction*> &originalInstructions, uint8_t directions=3);

// TODO separate if the instruction is constant (i.e. could change things)
//    from if the value is constant (the value is something that could be differentiated)
bool isconstantM(Instruction* inst, SmallPtrSetImpl<Value*> &constants, SmallPtrSetImpl<Value*> &nonconstant, const SmallPtrSetImpl<Value*> &retvals, const SmallPtrSetImpl<Instruction*> &originalInstructions, uint8_t directions=3) {
    assert(inst);
	constexpr uint8_t UP = 1;
	constexpr uint8_t DOWN = 2;
	//assert(directions >= 0);
	assert(directions <= 3);
    if (isa<ReturnInst>(inst)) return true;

	if(isa<UnreachableInst>(inst) || isa<BranchInst>(inst) || (constants.find(inst) != constants.end()) || (originalInstructions.find(inst) == originalInstructions.end()) ) {
    	return true;
    }

    if((nonconstant.find(inst) != nonconstant.end())) {
        return false;
    }

	if (auto op = dyn_cast<CallInst>(inst)) {
		if(auto called = op->getCalledFunction()) {
			if (called->getName() == "printf" || called->getName() == "puts") {
			//if (called->getName() == "printf" || called->getName() == "puts" || called->getName() == "__assert_fail") {
				nonconstant.insert(inst);
				return false;
			}
		}
	}
	
    if (auto op = dyn_cast<CallInst>(inst)) {
		if(auto called = op->getCalledFunction()) {
			if (called->getName() == "__assert_fail" || called->getName() == "free" || called->getName() == "_ZdlPv" || called->getName() == "_ZdlPvm") {
				constants.insert(inst);
				return true;
			}
		}
	}
	
    if (auto op = dyn_cast<IntrinsicInst>(inst)) {
		switch(op->getIntrinsicID()) {
			case Intrinsic::stacksave:
			case Intrinsic::stackrestore:
			case Intrinsic::lifetime_start:
			case Intrinsic::lifetime_end:
			case Intrinsic::dbg_addr:
			case Intrinsic::dbg_declare:
			case Intrinsic::dbg_value:
			case Intrinsic::invariant_start:
			case Intrinsic::invariant_end:
			case Intrinsic::var_annotation:
			case Intrinsic::ptr_annotation:
			case Intrinsic::annotation:
			case Intrinsic::codeview_annotation:
			case Intrinsic::expect:
			case Intrinsic::type_test:
			case Intrinsic::donothing:
			//case Intrinsic::is_constant:
				constants.insert(inst);
				return true;
			default:
				break;
		}
	}

	if (isa<CmpInst>(inst)) {
		constants.insert(inst);
		return true;
	}

    if (printconst)
	  llvm::errs() << "checking if is constant " << *inst << "\n";

	if (inst->getType()->isPointerTy()) {
		//Proceed assuming this is constant, can we prove this should be constant otherwise
		SmallPtrSet<Value*, 20> constants2;
		constants2.insert(constants.begin(), constants.end());
		SmallPtrSet<Value*, 20> nonconstant2;
		nonconstant2.insert(nonconstant.begin(), nonconstant.end());
		constants2.insert(inst);

		if (printconst)
			llvm::errs() << " < MEMSEARCH" << (int)directions << ">" << *inst << "\n";

		for (const auto &a:inst->users()) {
		  if(auto store = dyn_cast<StoreInst>(a)) {
			if (inst == store->getPointerOperand() && !isconstantValueM(store->getValueOperand(), constants2, nonconstant2, retvals, originalInstructions, directions)) {
				if (directions == 3)
				  nonconstant.insert(inst);
    			if (printconst)
				  llvm::errs() << "memory erase 1: " << *inst << "\n";
				return false;
			}
			if (inst == store->getValueOperand() && !isconstantValueM(store->getPointerOperand(), constants2, nonconstant2, retvals, originalInstructions, directions)) {
				if (directions == 3)
				  nonconstant.insert(inst);
    			if (printconst)
				  llvm::errs() << "memory erase 2: " << *inst << "\n";
				return false;
			}
		  } else if (isa<LoadInst>(a)) continue;
		  else {
			if (!isconstantM(cast<Instruction>(a), constants2, nonconstant2, retvals, originalInstructions, directions)) {
				if (directions == 3)
				  nonconstant.insert(inst);
    			if (printconst)
				  llvm::errs() << "memory erase 3: " << *inst << " op " << *a << "\n";
				return false;
			}
		  }

		}
		
		if (printconst)
			llvm::errs() << " </MEMSEARCH" << (int)directions << ">" << *inst << "\n";
	}

	if (!inst->getType()->isPointerTy() && !inst->mayWriteToMemory() && (directions & DOWN) ) { 
		//Proceed assuming this is constant, can we prove this should be constant otherwise
		SmallPtrSet<Value*, 20> constants2;
		constants2.insert(constants.begin(), constants.end());
		SmallPtrSet<Value*, 20> nonconstant2;
		nonconstant2.insert(nonconstant.begin(), nonconstant.end());
		constants2.insert(inst);

		if (printconst)
			llvm::errs() << " < USESEARCH" << (int)directions << ">" << *inst << "\n";

		assert(!inst->mayWriteToMemory());
		assert(!isa<StoreInst>(inst));
		bool seenuse = false;
		for (const auto &a:inst->users()) {
			if (auto gep = dyn_cast<GetElementPtrInst>(a)) {
				assert(inst != gep->getPointerOperand());
				continue;
			}
			if (auto call = dyn_cast<CallInst>(a)) {
                auto fnp = call->getCalledFunction();
                if (fnp) {
                    auto fn = fnp->getName();
                    // todo realloc consider?
                    if (fn == "malloc" || fn == "_Znwm")
				        continue;
                    if (fnp->getIntrinsicID() == Intrinsic::memset && call->getArgOperand(0) != inst && call->getArgOperand(1) != inst)
                        continue;
                }
			}

		  	if (!isconstantM(cast<Instruction>(a), constants2, nonconstant2, retvals, originalInstructions, DOWN)) {
    			if (printconst)
			      llvm::errs() << "nonconstant inst (uses):" << *inst << " user " << *a << "\n";
				seenuse = true;
				break;
			} else {
               if (printconst)
			     llvm::errs() << "found constant inst use:" << *inst << " user " << *a << "\n";
			}
		}
		if (!seenuse) {
			constants.insert(inst);
			constants.insert(constants2.begin(), constants2.end());
			// not here since if had full updown might not have been nonconstant
			//nonconstant.insert(nonconstant2.begin(), nonconstant2.end());
    		if (printconst)
			  llvm::errs() << "constant inst (uses):" << *inst << "\n";
			return true;
		}
		
        if (printconst)
			llvm::errs() << " </USESEARCH" << (int)directions << ">" << *inst << "\n";
	}

	SmallPtrSet<Value*, 20> constants2;
	constants2.insert(constants.begin(), constants.end());
	SmallPtrSet<Value*, 20> nonconstant2;
	nonconstant2.insert(nonconstant.begin(), nonconstant.end());
	constants2.insert(inst);
		
    if (printconst)
		llvm::errs() << " < PRESEARCH" << (int)directions << ">" << *inst << "\n";

	if (directions & UP) {
        if (auto gep = dyn_cast<GetElementPtrInst>(inst)) {
            // Handled uses above
            if (!isconstantValueM(gep->getPointerOperand(), constants2, nonconstant2, retvals, originalInstructions, UP)) {
                if (directions == 3)
                  nonconstant.insert(inst);
                if (printconst)
                  llvm::errs() << "nonconstant gep " << *inst << " op " << *gep->getPointerOperand() << "\n";
                return false;
            }
            constants.insert(inst);
            constants.insert(constants2.begin(), constants2.end());
            if (directions == 3)
              nonconstant.insert(nonconstant2.begin(), nonconstant2.end());
            if (printconst)
              llvm::errs() << "constant gep:" << *inst << "\n";
            return true;
        } else {
            for(auto& a: inst->operands()) {
                if (!isconstantValueM(a, constants2, nonconstant2, retvals, originalInstructions, UP)) {
                    if (directions == 3)
                      nonconstant.insert(inst);
                    if (printconst)
                      llvm::errs() << "nonconstant inst " << *inst << " op " << *a << "\n";
                    return false;
                }
            }

            constants.insert(inst);
            constants.insert(constants2.begin(), constants2.end());
            if (directions == 3)
              nonconstant.insert(nonconstant2.begin(), nonconstant2.end());
            if (printconst)
              llvm::errs() << "constant inst:" << *inst << "\n";
            return true;
        }
	}

    if (printconst)
		llvm::errs() << " </PRESEARCH" << (int)directions << ">" << *inst << "\n";

    if (directions == 3)
	  nonconstant.insert(inst);
    if (printconst)
	  llvm::errs() << "couldnt decide nonconstants:" << *inst << "\n";
	return false;
}

// TODO separate if the instruction is constant (i.e. could change things)
//    from if the value is constant (the value is something that could be differentiated)
bool isconstantValueM(Value* val, SmallPtrSetImpl<Value*> &constants, SmallPtrSetImpl<Value*> &nonconstant, const SmallPtrSetImpl<Value*> &retvals, const SmallPtrSetImpl<Instruction*> &originalInstructions, uint8_t directions) {
    assert(val);
	constexpr uint8_t UP = 1;
	constexpr uint8_t DOWN = 2;
	//assert(directions >= 0);
	assert(directions <= 3);
    
    if (val->getType()->isVoidTy()) return true;
	
    if (isa<Constant>(val)) return true;
	if (isa<BasicBlock>(val)) return true;
    assert(!isa<InlineAsm>(val));

    if((constants.find(val) != constants.end())) {
        return true;
    }
    if((retvals.find(val) != retvals.end())) {
        if (printconst) {
		    llvm::errs() << " VALUE nonconst from retval " << *val << "\n";
        }
        return false;
    }

    //All arguments should be marked constant/nonconstant ahead of time
    if (isa<Argument>(val)) {
        if((nonconstant.find(val) != nonconstant.end())) {
		    if (printconst)
		      llvm::errs() << " VALUE nonconst from arg nonconst " << *val << "\n";
            return false;
        }
        assert(0 && "must've put arguments in constant/nonconstant");
    }
    
    if (auto inst = dyn_cast<Instruction>(val)) {
        if (isconstantM(inst, constants, nonconstant, retvals, originalInstructions, directions)) return true;
    }
	
    if (!val->getType()->isPointerTy() && (directions & DOWN) ) { 
		auto &constants2 = constants;
		auto &nonconstant2 = nonconstant;

		if (printconst)
			llvm::errs() << " <Value USESEARCH" << (int)directions << ">" << *val << "\n";

		bool seenuse = false;
		
        for (const auto &a:val->users()) {
		    if (printconst)
			  llvm::errs() << "      considering use of " << *val << " - " << *a << "\n";

			if (auto gep = dyn_cast<GetElementPtrInst>(a)) {
				assert(val != gep->getPointerOperand());
				continue;
			}
			if (auto call = dyn_cast<CallInst>(a)) {
                auto fnp = call->getCalledFunction();
                if (fnp) {
                    auto fn = fnp->getName();
                    // todo realloc consider?
                    if (fn == "malloc" || fn == "_Znwm")
				        continue;
                    if (fnp->getIntrinsicID() == Intrinsic::memset && call->getArgOperand(0) != val && call->getArgOperand(1) != val)
                        continue;
                }
			}
            
		  	if (!isconstantM(cast<Instruction>(a), constants2, nonconstant2, retvals, originalInstructions, DOWN)) {
    			if (printconst)
			      llvm::errs() << "Value nonconstant inst (uses):" << *val << " user " << *a << "\n";
				seenuse = true;
				break;
			} else {
               if (printconst)
			     llvm::errs() << "Value found constant inst use:" << *val << " user " << *a << "\n";
			}
		}

		if (!seenuse) {
    		if (printconst)
			  llvm::errs() << "Value constant inst (uses):" << *val << "\n";
			return true;
		}
		
        if (printconst)
			llvm::errs() << " </Value USESEARCH" << (int)directions << ">" << *val << "\n";
	}

    return false;
}

 static bool promoteMemoryToRegister(Function &F, DominatorTree &DT,
                                     AssumptionCache &AC) {
   std::vector<AllocaInst *> Allocas;
   BasicBlock &BB = F.getEntryBlock(); // Get the entry node for the function
   bool Changed = false;
 
   while (true) {
     Allocas.clear();
 
     // Find allocas that are safe to promote, by looking at all instructions in
     // the entry node
     for (BasicBlock::iterator I = BB.begin(), E = --BB.end(); I != E; ++I)
       if (AllocaInst *AI = dyn_cast<AllocaInst>(I)) // Is it an alloca?
         if (isAllocaPromotable(AI))
           Allocas.push_back(AI);
 
     if (Allocas.empty())
       break;
 
     PromoteMemToReg(Allocas, DT, &AC);
     Changed = true;
   }
   return Changed;
 }

enum class ReturnType {
    Normal, ArgsWithReturn, Args
};

Function *CloneFunctionWithReturns(Function *F, ValueToValueMapTy& ptrInputs, const SmallSet<unsigned,4>& constant_args, SmallPtrSetImpl<Value*> &constants, SmallPtrSetImpl<Value*> &nonconstant, SmallPtrSetImpl<Value*> &returnvals, ReturnType returnValue, bool differentialReturn, Twine name, ValueToValueMapTy *VMapO, bool diffeReturnArg, llvm::Type* additionalArg = nullptr) {
 assert(!F->empty());
 diffeReturnArg &= differentialReturn;
 std::vector<Type*> RetTypes;
 if (returnValue == ReturnType::ArgsWithReturn)
   RetTypes.push_back(F->getReturnType());
 std::vector<Type*> ArgTypes;

 ValueToValueMapTy VMap;

 // The user might be deleting arguments to the function by specifying them in
 // the VMap.  If so, we need to not add the arguments to the arg ty vector
 //
 unsigned argno = 0;
 for (const Argument &I : F->args()) {
     ArgTypes.push_back(I.getType());
     if (constant_args.count(argno)) {
        argno++;
        continue;
     }
     if (I.getType()->isPointerTy() || I.getType()->isIntegerTy()) {
        ArgTypes.push_back(I.getType());
        /*
        if (I.getType()->isPointerTy() && !(I.hasAttribute(Attribute::ReadOnly) || I.hasAttribute(Attribute::ReadNone) ) ) {
          llvm::errs() << "Cannot take derivative of function " <<F->getName()<< " input argument to function " << I.getName() << " is not marked read-only\n";
          exit(1);
        }
        */
     } else { 
       RetTypes.push_back(I.getType());
     }
     argno++;
 }

 if (diffeReturnArg && !F->getReturnType()->isPointerTy() && !F->getReturnType()->isIntegerTy()) {
    assert(!F->getReturnType()->isVoidTy());
    ArgTypes.push_back(F->getReturnType());
 }
 if (additionalArg) {
    ArgTypes.push_back(additionalArg);
 }
 Type* RetType = StructType::get(F->getContext(), RetTypes);
 if (returnValue == ReturnType::Normal)
     RetType = F->getReturnType();

 // Create a new function type...
 FunctionType *FTy = FunctionType::get(RetType,
                                   ArgTypes, F->getFunctionType()->isVarArg());

 // Create the new function...
 Function *NewF = Function::Create(FTy, F->getLinkage(), name, F->getParent());
 if (diffeReturnArg && !F->getReturnType()->isPointerTy() && !F->getReturnType()->isIntegerTy()) {
    auto I = NewF->arg_end();
    I--;
    if(additionalArg)
        I--;
    I->setName("differeturn");
 }
 if (additionalArg) {
    auto I = NewF->arg_end();
    I--;
    I->setName("tapeArg");
 }

 bool hasPtrInput = false;

 unsigned ii = 0, jj = 0;
 for (auto i=F->arg_begin(), j=NewF->arg_begin(); i != F->arg_end(); ) {
   bool isconstant = (constant_args.count(ii) > 0);

   if (isconstant) {
      constants.insert(j);
      if (printconst)
        llvm::errs() << "in new function " << NewF->getName() << " constant arg " << *j << "\n";
   } else {
	  nonconstant.insert(j);
      if (printconst)
        llvm::errs() << "in new function " << NewF->getName() << " nonconstant arg " << *j << "\n";
   }

   if (!isconstant && ( i->getType()->isPointerTy() || i->getType()->isIntegerTy()) ) {
     VMap[i] = j;
     hasPtrInput = true;
     ptrInputs[j] = (j+1);
     if (F->hasParamAttribute(ii, Attribute::NoCapture)) {
       NewF->addParamAttr(jj, Attribute::NoCapture);
       NewF->addParamAttr(jj+1, Attribute::NoCapture);
     }
     if (F->hasParamAttribute(ii, Attribute::NoAlias)) {
       NewF->addParamAttr(jj, Attribute::NoAlias);
       NewF->addParamAttr(jj+1, Attribute::NoAlias);
     }

     j->setName(i->getName());
     j++;
     j->setName(i->getName()+"'");
	 nonconstant.insert(j);
     j++;
     jj+=2;

     i++;
     ii++;

   } else {
     VMap[i] = j;
     j->setName(i->getName());

     j++;
     jj++;
     i++;
     ii++;
   }
 }

 // Loop over the arguments, copying the names of the mapped arguments over...
 Function::arg_iterator DestI = NewF->arg_begin();


 for (const Argument & I : F->args())
   if (VMap.count(&I) == 0) {     // Is this argument preserved?
     DestI->setName(I.getName()); // Copy the name over...
     VMap[&I] = &*DestI++;        // Add mapping to VMap
   }
 SmallVector <ReturnInst*,4> Returns;
 CloneFunctionInto(NewF, F, VMap, F->getSubprogram() != nullptr, Returns, "",
                   nullptr);
 if (VMapO) VMapO->insert(VMap.begin(), VMap.end());

 if (hasPtrInput) {
    if (NewF->hasFnAttribute(Attribute::ReadNone)) {
    NewF->removeFnAttr(Attribute::ReadNone);
    }
    if (NewF->hasFnAttribute(Attribute::ReadOnly)) {
    NewF->removeFnAttr(Attribute::ReadOnly);
    }
 }
 NewF->setLinkage(Function::LinkageTypes::InternalLinkage);
 assert(NewF->hasLocalLinkage());

 if (differentialReturn) {
   for(auto& r : Returns) {
     if (auto a = r->getReturnValue()) {
       nonconstant.insert(a);
       returnvals.insert(a);
       if (printconst)
         llvm::errs() << "in new function " << NewF->getName() << " nonconstant retval " << *a << "\n";
     }
   }
 }

 //SmallPtrSet<Value*,4> constants2;
 //for (auto a :constants){
 //   constants2.insert(a);
// }
 //for (auto a :nonconstant){
 //   nonconstant2.insert(a);
 //}
 if (true) {
    FunctionAnalysisManager AM;
 AM.registerPass([] { return AAManager(); });
 AM.registerPass([] { return ScalarEvolutionAnalysis(); });
 AM.registerPass([] { return AssumptionAnalysis(); });
 AM.registerPass([] { return TargetLibraryAnalysis(); });
 AM.registerPass([] { return DominatorTreeAnalysis(); });
 AM.registerPass([] { return MemoryDependenceAnalysis(); });
 AM.registerPass([] { return LoopAnalysis(); });
 AM.registerPass([] { return OptimizationRemarkEmitterAnalysis(); });
 AM.registerPass([] { return PhiValuesAnalysis(); });

    LoopSimplifyPass().run(*NewF, AM);

 }

  if(autodiff_inline) {
      llvm::errs() << "running inlining process\n";
   remover:
     SmallPtrSet<Instruction*, 10> originalInstructions;
     for (inst_iterator I = inst_begin(NewF), E = inst_end(NewF); I != E; ++I) {
         originalInstructions.insert(&*I);
     }
   for (inst_iterator I = inst_begin(NewF), E = inst_end(NewF); I != E; ++I)
     if (auto call = dyn_cast<CallInst>(&*I)) {
        if (isconstantM(call, constants, nonconstant, returnvals, originalInstructions)) continue;
        if (call->getCalledFunction() == nullptr) continue;
        if (call->getCalledFunction()->empty()) continue;
        /*
        if (call->getCalledFunction()->hasFnAttribute(Attribute::NoInline)) {
            llvm::errs() << "can't inline noinline " << call->getCalledFunction()->getName() << "\n";
            continue;
        }
        */
        if (call->getCalledFunction()->hasFnAttribute(Attribute::ReturnsTwice)) continue;
        if (call->getCalledFunction() == F || call->getCalledFunction() == NewF) {
            llvm::errs() << "can't inline recursive " << call->getCalledFunction()->getName() << "\n";
            continue;
        }
        llvm::errs() << "inlining " << call->getCalledFunction()->getName() << "\n";
        InlineFunctionInfo IFI;
        InlineFunction(call, IFI);
        goto remover;
     }
 }

 if (autodiff_inline) {
 DominatorTree DT(*NewF);
 AssumptionCache AC(*NewF);
 promoteMemoryToRegister(*NewF, DT, AC);

 GVN gvn;

 FunctionAnalysisManager AM;
 AM.registerPass([] { return AAManager(); });
 AM.registerPass([] { return AssumptionAnalysis(); });
 AM.registerPass([] { return TargetLibraryAnalysis(); });
 AM.registerPass([] { return DominatorTreeAnalysis(); });
 AM.registerPass([] { return MemoryDependenceAnalysis(); });
 AM.registerPass([] { return LoopAnalysis(); });
 AM.registerPass([] { return OptimizationRemarkEmitterAnalysis(); });
 AM.registerPass([] { return PhiValuesAnalysis(); });
 //AM.registerPass([] { return DominatorTreeWrapperPass() });
 gvn.run(*NewF, AM);

 SROA().run(*NewF, AM);
 //LCSSAPass().run(*NewF, AM);
 //gvn.run(*NewF, AM);

 //gvn.runImpl(*NewF, AC, DT, TLI, AA);
 /*
     auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
     auto &TTI = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
     auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
     auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
     auto *MSSA =
         UseMemorySSA ? &getAnalysis<MemorySSAWrapperPass>().getMSSA() : nullptr;
 
   EarlyCSE CSE(F.getParent()->getDataLayout(), TLI, TTI, DT, AC, MSSA);
   CSE.run();
*/
 }

    FunctionAnalysisManager AM;
     AM.registerPass([] { return AAManager(); });
     AM.registerPass([] { return ScalarEvolutionAnalysis(); });
     AM.registerPass([] { return AssumptionAnalysis(); });
     AM.registerPass([] { return TargetLibraryAnalysis(); });
     AM.registerPass([] { return TargetIRAnalysis(); });
     AM.registerPass([] { return MemorySSAAnalysis(); });
     AM.registerPass([] { return DominatorTreeAnalysis(); });
     AM.registerPass([] { return MemoryDependenceAnalysis(); });
     AM.registerPass([] { return LoopAnalysis(); });
     AM.registerPass([] { return OptimizationRemarkEmitterAnalysis(); });
     AM.registerPass([] { return PhiValuesAnalysis(); });
     AM.registerPass([] { return LazyValueAnalysis(); });
 SimplifyCFGOptions scfgo(/*unsigned BonusThreshold=*/1, /*bool ForwardSwitchCond=*/false, /*bool SwitchToLookup=*/false, /*bool CanonicalLoops=*/true, /*bool SinkCommon=*/true, /*AssumptionCache *AssumpCache=*/nullptr);
 SimplifyCFGPass(scfgo).run(*NewF, AM);
 LoopSimplifyPass().run(*NewF, AM);

 return NewF;
}

#include "llvm/IR/Constant.h"
#include <deque>
#include "llvm/IR/CFG.h"

PHINode* canonicalizeIVs(Type *Ty, Loop *L, ScalarEvolution &SE, DominatorTree &DT) {

  BasicBlock* Header = L->getHeader();
  Module* M = Header->getParent()->getParent();
  const DataLayout &DL = M->getDataLayout();

  SCEVExpander Exp(SE, DL, "ls");

  PHINode *CanonicalIV = Exp.getOrInsertCanonicalInductionVariable(L, Ty);
  assert (CanonicalIV && "canonicalizing IV");
  //DEBUG(dbgs() << "Canonical induction variable " << *CanonicalIV << "\n");

  SmallVector<WeakTrackingVH, 16> DeadInsts;
  Exp.replaceCongruentIVs(L, &DT, DeadInsts);

  
  for (WeakTrackingVH V : DeadInsts) {
    //DEBUG(dbgs() << "erasing dead inst " << *V << "\n");
    Instruction *I = cast<Instruction>(V);
    I->eraseFromParent();
  }
  

  return CanonicalIV;
}

/// \brief Replace the latch of the loop to check that IV is always less than or
/// equal to the limit.
///
/// This method assumes that the loop has a single loop latch.
Value* canonicalizeLoopLatch(PHINode *IV, Value *Limit, Loop* L, ScalarEvolution &SE, BasicBlock* ExitBlock) {
  Value *NewCondition;
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  assert(Latch && "No single loop latch found for loop.");

  IRBuilder<> Builder(&*Latch->getFirstInsertionPt());
  Builder.setFastMathFlags(FastMathFlags::getFast());

  // This process assumes that IV's increment is in Latch.

  // Create comparison between IV and Limit at top of Latch.
  NewCondition = Builder.CreateICmpULT(IV, Limit);

  // Replace the conditional branch at the end of Latch.
  BranchInst *LatchBr = dyn_cast_or_null<BranchInst>(Latch->getTerminator());
  assert(LatchBr && LatchBr->isConditional() &&
         "Latch does not terminate with a conditional branch.");
  Builder.SetInsertPoint(Latch->getTerminator());
  Builder.CreateCondBr(NewCondition, Header, ExitBlock);

  // Erase the old conditional branch.
  Value *OldCond = LatchBr->getCondition();
  LatchBr->eraseFromParent();
  
  if (!OldCond->hasNUsesOrMore(1))
    if (Instruction *OldCondInst = dyn_cast<Instruction>(OldCond))
      OldCondInst->eraseFromParent();
  

  return NewCondition;
}

bool shouldRecompute(Value* val, const ValueToValueMapTy& available) {
          if (available.count(val)) return false;
          if (isa<Argument>(val) || isa<Constant>(val) ) {
            return false;
          } else if (auto op = dyn_cast<CastInst>(val)) {
            return shouldRecompute(op->getOperand(0), available);
          } else if (isa<AllocaInst>(val)) {
            return true;
          } else if (auto op = dyn_cast<BinaryOperator>(val)) {
            bool a0 = shouldRecompute(op->getOperand(0), available);
            if (a0) {
                //llvm::errs() << "need recompute: " << *op->getOperand(0) << "\n";
            }
            bool a1 = shouldRecompute(op->getOperand(1), available);
            if (a1) {
                //llvm::errs() << "need recompute: " << *op->getOperand(1) << "\n";
            }
            return a0 || a1;
          } else if (auto op = dyn_cast<CmpInst>(val)) {
            return shouldRecompute(op->getOperand(0), available) || shouldRecompute(op->getOperand(1), available);
          } else if (auto op = dyn_cast<SelectInst>(val)) {
            return shouldRecompute(op->getOperand(0), available) || shouldRecompute(op->getOperand(1), available) || shouldRecompute(op->getOperand(2), available);
          } else if (auto load = dyn_cast<LoadInst>(val)) {
                Value* idx = load->getOperand(0);
                while (!isa<Argument>(idx)) {
                    if (auto gep = dyn_cast<GetElementPtrInst>(idx)) {
                        for(auto &a : gep->indices()) {
                            if (shouldRecompute(a, available)) {
                                //llvm::errs() << "not recomputable: " << *a << "\n";
                                return true;
                            }
                        }
                        idx = gep->getPointerOperand();
                    } else if(auto cast = dyn_cast<CastInst>(idx)) {
                        idx = cast->getOperand(0);
                    } else if(isa<CallInst>(idx)) {
                    //} else if(auto call = dyn_cast<CallInst>(idx)) {
                        //if (call->getCalledFunction()->getName() == "malloc")
                        //    return false;
                        //else
                        {
                            //llvm::errs() << "unknown call " << *call << "\n";
                            return true;
                        }
                    } else {
                      //llvm::errs() << "not a gep " << *idx << "\n";
                      return true;
                    }
                }
                Argument* arg = cast<Argument>(idx);
                if (! ( arg->hasAttribute(Attribute::ReadOnly) || arg->hasAttribute(Attribute::ReadNone)) ) {
                    //llvm::errs() << "argument " << *arg << " not marked read only\n";
                    return true;
                }
                return false;
          } else if (auto phi = dyn_cast<PHINode>(val)) {
            if (phi->getNumIncomingValues () == 1) {
                bool b = shouldRecompute(phi->getIncomingValue(0) , available);
                if (b) {
                    //llvm::errs() << "phi need recompute: " <<*phi->getIncomingValue(0) << "\n";
                }
                return b;
            }

            return true;
          } else if (auto op = dyn_cast<IntrinsicInst>(val)) {
            switch(op->getIntrinsicID()) {
                case Intrinsic::sin:
                case Intrinsic::cos:
                    return false;
                    return shouldRecompute(op->getOperand(0), available);
                default:
                    return true;
            }
        }

          //llvm::errs() << "unknown inst " << *val << " unable to recompute\n";
          return true;
}

    Type* FloatToIntTy(Type* T) {
        assert(T->isFPOrFPVectorTy());
        if (auto ty = dyn_cast<VectorType>(T)) {
            return VectorType::get(FloatToIntTy(ty->getElementType()), ty->getNumElements());
        }
        if (T->isHalfTy()) return IntegerType::get(T->getContext(), 16); 
        if (T->isFloatTy()) return IntegerType::get(T->getContext(), 32); 
        if (T->isDoubleTy()) return IntegerType::get(T->getContext(), 64);
        assert(0 && "unknown floating point type");
        return nullptr;
    }

    Type* IntToFloatTy(Type* T) {
        assert(T->isIntOrIntVectorTy());
        if (auto ty = dyn_cast<VectorType>(T)) {
            return VectorType::get(IntToFloatTy(ty->getElementType()), ty->getNumElements());
        }
        if (auto ty = dyn_cast<IntegerType>(T)) {
            switch(ty->getBitWidth()) {
                case 16: return Type::getHalfTy(T->getContext());
                case 32: return Type::getFloatTy(T->getContext());
                case 64: return Type::getDoubleTy(T->getContext());
            }
        }
        assert(0 && "unknown int to floating point type");
        return nullptr;
    }

typedef struct {
  PHINode* var;
  PHINode* antivar;
  BasicBlock* latch;
  BasicBlock* header;
  BasicBlock* preheader;
  bool dynamic;
  //limit is last value, iters is number of iters (thus iters = limit + 1)
  Value* limit;
  BasicBlock* exit;
  Loop* parent;
} LoopContext;

bool operator==(const LoopContext& lhs, const LoopContext &rhs) {
    return lhs.parent == rhs.parent;
}

bool getContextM(BasicBlock *BB, LoopContext &loopContext, std::map<Loop*,LoopContext> &loopContexts, LoopInfo &LI,ScalarEvolution &SE,DominatorTree &DT) {
    if (auto L = LI.getLoopFor(BB)) {
        if (loopContexts.find(L) != loopContexts.end()) {
            loopContext = loopContexts.find(L)->second;
            return true;
        }

        SmallVector<BasicBlock *, 8> PotentialExitBlocks;
        SmallPtrSet<BasicBlock *, 8> ExitBlocks;
        L->getExitBlocks(PotentialExitBlocks);
        for(auto a:PotentialExitBlocks) {

            SmallVector<BasicBlock*, 4> tocheck;
            SmallPtrSet<BasicBlock*, 4> checked;
            tocheck.push_back(a);

            bool isExit = false;

            while(tocheck.size()) {
                auto foo = tocheck.back();
                tocheck.pop_back();
                if (checked.count(foo)) {
                    isExit = true;
                    goto exitblockcheck;
                }
                checked.insert(foo);
                if(auto bi = dyn_cast<BranchInst>(foo->getTerminator())) {
                    for(auto nb : bi->successors()) {
                        if (L->contains(nb)) continue;
                        tocheck.push_back(nb);
                    }
                } else if (isa<UnreachableInst>(foo->getTerminator())) {
                    continue;
                } else {
                    isExit = true;
                    goto exitblockcheck;
                }
            }

            
            exitblockcheck:
            if (isExit) {
				ExitBlocks.insert(a);
            }
        }

        if (ExitBlocks.size() != 1) {
            assert(BB);
            assert(BB->getParent());
            assert(L);
            llvm::errs() << *BB->getParent() << "\n";
            llvm::errs() << *L << "\n";
			for(auto b:ExitBlocks) {
                assert(b);
                llvm::errs() << *b << "\n";
            }
			llvm::errs() << "offending: \n";
			llvm::errs() << "No unique exit block (1)\n";
        }

        BasicBlock* ExitBlock = *ExitBlocks.begin(); //[0];

        BasicBlock *Header = L->getHeader();
        BasicBlock *Preheader = L->getLoopPreheader();
        assert(Preheader && "requires preheader");
        BasicBlock *Latch = L->getLoopLatch();

        const SCEV *Limit = SE.getExitCount(L, Latch);
        
		SCEVExpander Exp(SE, Preheader->getParent()->getParent()->getDataLayout(), "ad");

		PHINode *CanonicalIV = nullptr;
		Value *LimitVar = nullptr;
		if (SE.getCouldNotCompute() != Limit) {

        	CanonicalIV = canonicalizeIVs(Limit->getType(), L, SE, DT);
        	if (!CanonicalIV) {
                report_fatal_error("Couldn't get canonical IV.");
        	}
        	
			const SCEVAddRecExpr *CanonicalSCEV = cast<const SCEVAddRecExpr>(SE.getSCEV(CanonicalIV));

        	assert(SE.isLoopBackedgeGuardedByCond(L, ICmpInst::ICMP_ULT,
                                              CanonicalSCEV, Limit) &&
               "Loop backedge is not guarded by canonical comparison with limit.");
        
			LimitVar = Exp.expandCodeFor(Limit, CanonicalIV->getType(),
                                            Preheader->getTerminator());

        	// Canonicalize the loop latch.
			canonicalizeLoopLatch(CanonicalIV, LimitVar, L, SE, ExitBlock);

			loopContext.dynamic = false;
		} else {
          llvm::errs() << "Se has any info: " << SE.getBackedgeTakenInfo(L).hasAnyInfo() << "\n";
          llvm::errs() << "SE could not compute loop limit.\n";

		  IRBuilder <>B(&Header->front());
		  CanonicalIV = B.CreatePHI(Type::getInt64Ty(Header->getContext()), 1); // should be Header->getNumPredecessors());

		  B.SetInsertPoint(Header->getTerminator());
		  auto inc = B.CreateNUWAdd(CanonicalIV, ConstantInt::get(CanonicalIV->getType(), 1));
		  CanonicalIV->addIncoming(inc, Latch);
		  for (BasicBlock *Pred : predecessors(Header)) {
			  if (Pred != Latch) {
				  CanonicalIV->addIncoming(ConstantInt::get(CanonicalIV->getType(), 0), Pred);
			  }
		  }

		  B.SetInsertPoint(&ExitBlock->front());
		  LimitVar = B.CreatePHI(CanonicalIV->getType(), 1); // should be ExitBlock->getNumPredecessors());

		  for (BasicBlock *Pred : predecessors(ExitBlock)) {
    		if (LI.getLoopFor(Pred) == L)
		    	cast<PHINode>(LimitVar)->addIncoming(CanonicalIV, Pred);
			else
				cast<PHINode>(LimitVar)->addIncoming(ConstantInt::get(CanonicalIV->getType(), 0), Pred);
		  }
		  loopContext.dynamic = true;
		}
	
		// Remove Canonicalizable IV's
		{
		  SmallVector<PHINode*, 8> IVsToRemove;
		  for (BasicBlock::iterator II = Header->begin(); isa<PHINode>(II); ++II) {
			PHINode *PN = cast<PHINode>(II);
			if (PN == CanonicalIV) continue;
			if (!SE.isSCEVable(PN->getType())) continue;
			const SCEV *S = SE.getSCEV(PN);
			if (SE.getCouldNotCompute() == S) continue;
			Value *NewIV = Exp.expandCodeFor(S, S->getType(), CanonicalIV);
			if (NewIV == PN) {
				llvm::errs() << "TODO: odd case need to ensure replacement\n";
				continue;
			}
			PN->replaceAllUsesWith(NewIV);
			IVsToRemove.push_back(PN);
		  }
		  for (PHINode *PN : IVsToRemove) {
			//llvm::errs() << "ERASING: " << *PN << "\n";
			PN->eraseFromParent();
		  }
		}

        //if (SE.getCouldNotCompute() == Limit) {
        //Limit = SE.getMaxBackedgeTakenCount(L);
        //}
		assert(CanonicalIV);
		assert(LimitVar);
        loopContext.var = CanonicalIV;
        loopContext.limit = LimitVar;
        loopContext.antivar = PHINode::Create(CanonicalIV->getType(), CanonicalIV->getNumIncomingValues(), CanonicalIV->getName()+"'phi");
        loopContext.exit = ExitBlock;
        loopContext.latch = Latch;
        loopContext.preheader = Preheader;
		loopContext.header = Header;
        loopContext.parent = L->getParentLoop();

        loopContexts[L] = loopContext;
        return true;
    }
    return false;
  }

bool isCertainMallocOrFree(Function* called) {
    if (called == nullptr) return false;
    if (called->getName() == "printf" || called->getName() == "puts" || called->getName() == "malloc" || called->getName() == "_Znwm" || called->getName() == "_ZdlPv" || called->getName() == "_ZdlPvm" || called->getName() == "free") return true;
    switch(called->getIntrinsicID()) {
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_label:
            case Intrinsic::dbg_addr:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
                return true;
            default:
                break;
    }

    return false;
}

bool isCertainPrintMallocOrFree(Function* called) {
    if (called == nullptr) return false;
    
    if (called->getName() == "printf" || called->getName() == "puts" || called->getName() == "malloc" || called->getName() == "_Znwm" || called->getName() == "_ZdlPv" || called->getName() == "_ZdlPvm" || called->getName() == "free") return true;
    switch(called->getIntrinsicID()) {
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_label:
            case Intrinsic::dbg_addr:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
                return true;
            default:
                break;
    }
    return false;
}

class GradientUtils {
public:
  llvm::Function *newFunc;
  ValueToValueMapTy invertedPointers;
  DominatorTree DT;
  SmallPtrSet<Value*,4> constants;
  SmallPtrSet<Value*,20> nonconstant;
  LoopInfo LI;
  AssumptionCache AC;
  ScalarEvolution SE;
  std::map<Loop*, LoopContext> loopContexts;
  SmallPtrSet<Instruction*, 10> originalInstructions;
  SmallVector<BasicBlock*, 12> originalBlocks;
  ValueMap<BasicBlock*,BasicBlock*> reverseBlocks;
  BasicBlock* inversionAllocs;
  ValueToValueMapTy scopeMap;
  std::vector<Instruction*> addedFrees;
  ValueToValueMapTy originalToNewFn;

  Value* getNewFromOriginal(Value* originst) {
    auto m = originalToNewFn[originst];
    assert(m);
    return m;
  }
  Value* getOriginal(Value* newinst) {
    for(auto v: originalToNewFn) {
        if (v.second == newinst) return const_cast<Value*>(v.first);
    }
    assert(0 && "could not invert new inst");
    report_fatal_error("could not invert new inst");
  }

  Value* getOriginalPointer(Value* newinst) {
    for(auto v: originalToNewFn) {
        if (invertedPointers[v.second] == newinst) return const_cast<Value*>(v.first);
    }
    assert(0 && "could not invert new pointer inst");
    report_fatal_error("could not invert new pointer inst");
  }

private:
  SmallVector<Value*, 4> addedMallocs;
  unsigned tapeidx;
  Value* tape;
public:
  void setTape(Value* newtape) {
    assert(tape == nullptr);
    assert(newtape != nullptr);
    assert(tapeidx == 0);
    assert(addedMallocs.size() == 0);
    tape = newtape;
  }
  Instruction* addMallocAndMemset(IRBuilder <> &BuilderQ, Instruction* malloc, Instruction* memset) {
    assert(malloc);
    assert(memset);
    if (tape) {
        Instruction* ret = cast<Instruction>(BuilderQ.CreateExtractValue(tape, {tapeidx}));
        malloc->replaceAllUsesWith(ret);
        malloc->eraseFromParent();
        memset->eraseFromParent();
        tapeidx++;
        return ret;
    } else {
      assert(!isa<PHINode>(malloc));
      addedMallocs.push_back(malloc);
      return malloc;
    }
  }

  Value* addMalloc(IRBuilder<> &BuilderQ, Value* malloc) {
    if (tape) {
        Instruction* ret = cast<Instruction>(BuilderQ.CreateExtractValue(tape, {tapeidx}));
        if (malloc && !isa<UndefValue>(malloc)) {
          cast<Instruction>(malloc)->replaceAllUsesWith(ret);
          cast<Instruction>(malloc)->eraseFromParent();
        }
        tapeidx++;
        if (malloc) {
            assert(malloc->getType() == ret->getType());
        }
        return ret;
    } else {
      assert(malloc);
      assert(!isa<PHINode>(malloc));
      addedMallocs.push_back(malloc);
      return malloc;
    }
  }

  std::pair<Instruction*,Instruction*> addMallocAndAnti(IRBuilder<> &BuilderQ, Instruction* malloc, Instruction* antiptr) {
    if (tape) {
        Instruction* ret = cast<Instruction>(BuilderQ.CreateExtractValue(tape, {tapeidx}));
        if (malloc) {
          malloc->replaceAllUsesWith(ret);
          malloc->eraseFromParent();
        }
        tapeidx++;

        Instruction* ret2 = nullptr;
        if (antiptr) {
            ret2 = cast<Instruction>(BuilderQ.CreateExtractValue(tape, {tapeidx}));
            antiptr->replaceAllUsesWith(ret2);
            antiptr->eraseFromParent();
            tapeidx++;
        }
        return std::pair<Instruction*,Instruction*>(ret, ret2);
    } else {
      assert(malloc);
      assert(!isa<PHINode>(malloc));
      addedMallocs.push_back(malloc);
      if (antiptr) {
        assert(!isa<PHINode>(antiptr));
        addedMallocs.push_back(antiptr);
      }
      return std::pair<Instruction*,Instruction*>(malloc, antiptr);
    }
  }

  const SmallVectorImpl<Value*> & getMallocs() const {
    return addedMallocs;
  }
  SmallPtrSet<Value*,2> nonconstant_values;
protected:
  GradientUtils(Function* newFunc_, TargetLibraryInfo &TLI, ValueToValueMapTy& invertedPointers_, const SmallPtrSetImpl<Value*> &constants_, const SmallPtrSetImpl<Value*> &nonconstant_, const SmallPtrSetImpl<Value*> &returnvals_, ValueToValueMapTy& originalToNewFn_) :
      newFunc(newFunc_), invertedPointers(), constants(constants_.begin(), constants_.end()), nonconstant(nonconstant_.begin(), nonconstant_.end()), nonconstant_values(returnvals_.begin(), returnvals_.end()), DT(*newFunc_), LI(DT), AC(*newFunc_), SE(*newFunc_, TLI, AC, DT, LI), inversionAllocs(nullptr) {
        invertedPointers.insert(invertedPointers_.begin(), invertedPointers_.end());  
        originalToNewFn.insert(originalToNewFn_.begin(), originalToNewFn_.end());  
          for (BasicBlock &BB: *newFunc) {
            originalBlocks.emplace_back(&BB);
            for(Instruction &I : BB) {
                originalInstructions.insert(&I);
            }
          }
        tape = nullptr;
        tapeidx = 0;
        assert(originalBlocks.size() > 0);
    }

public:
  static GradientUtils* CreateFromClone(Function *todiff, TargetLibraryInfo &TLI, const SmallSet<unsigned,4> & constant_args, ReturnType returnValue, bool differentialReturn, llvm::Type* additionalArg=nullptr) {
    assert(!todiff->empty());
    ValueToValueMapTy invertedPointers;
    SmallPtrSet<Value*,4> constants;
    SmallPtrSet<Value*,20> nonconstant;
    SmallPtrSet<Value*,2> returnvals;
    ValueToValueMapTy originalToNew;
    auto newFunc = CloneFunctionWithReturns(todiff, invertedPointers, constant_args, constants, nonconstant, returnvals, /*returnValue*/returnValue, /*differentialReturn*/differentialReturn, "fakeaugmented_"+todiff->getName(), &originalToNew, /*diffeReturnArg*/false, additionalArg);
    return new GradientUtils(newFunc, TLI, invertedPointers, constants, nonconstant, returnvals, originalToNew);
  }

  void prepareForReverse() {
    assert(reverseBlocks.size() == 0);
    for (BasicBlock *BB: originalBlocks) {
      reverseBlocks[BB] = BasicBlock::Create(BB->getContext(), "invert" + BB->getName(), newFunc);
    }
    assert(reverseBlocks.size() != 0);
  }

  BasicBlock* originalForReverseBlock(BasicBlock& BB2) const {
    assert(reverseBlocks.size() != 0);
    for(auto BB : originalBlocks) {
        auto it = reverseBlocks.find(BB);
        assert(it != reverseBlocks.end());
        if (it->second == &BB2) {
            return BB;
        }
    }
    report_fatal_error("could not find original block for given reverse block");
  }
 
  bool getContext(BasicBlock* BB, LoopContext& loopContext) {
    return getContextM(BB, loopContext, this->loopContexts, this->LI, this->SE, this->DT);
  }

  bool isOriginalBlock(const BasicBlock &BB) const {
    for(auto A : originalBlocks) {
        if (A == &BB) return true;
    }
    return false;
  }

  bool isConstantValue(Value* val) {
    return isconstantValueM(val, constants, nonconstant, nonconstant_values, originalInstructions);
  };
 
  bool isConstantInstruction(Instruction* val) {
    return isconstantM(val, constants, nonconstant, nonconstant_values, originalInstructions);
  }

  SmallPtrSet<Instruction*,4> replaceableCalls; 
  void eraseStructuralStoresAndCalls() { 

      for(BasicBlock* BB: this->originalBlocks) { 
        auto term = BB->getTerminator();
        if (isa<UnreachableInst>(term)) continue;
      
        for (auto I = BB->begin(), E = BB->end(); I != E;) {
          Instruction* inst = &*I;
          assert(inst);
          I++;

          if (originalInstructions.find(inst) == originalInstructions.end()) continue;

          if (isa<StoreInst>(inst)) {
            inst->eraseFromParent();
            continue;
          }
        }
      }

      for(BasicBlock* BB: this->originalBlocks) { 
        auto term = BB->getTerminator();
        if (isa<UnreachableInst>(term)) continue;
      
        for (auto I = BB->begin(), E = BB->end(); I != E;) {
          Instruction* inst = &*I;
          assert(inst);
          I++;

          if (originalInstructions.find(inst) == originalInstructions.end()) continue;

          if (!isa<TerminatorInst>(inst) && this->isConstantInstruction(inst)) {
            if (inst->getNumUses() == 0) {
                inst->eraseFromParent();
			    continue;
            }
          } else {
            if (auto inti = dyn_cast<IntrinsicInst>(inst)) {
                if (inti->getIntrinsicID() == Intrinsic::memcpy || inti->getIntrinsicID() == Intrinsic::memcpy) {
                    inst->eraseFromParent();
                    continue;
                }
            }
            if (replaceableCalls.find(inst) != replaceableCalls.end()) {
                if (inst->getNumUses() != 0) {
                    //inst->getParent()->getParent()->dump();
                    //inst->dump();
                } else {
                    inst->eraseFromParent();
                    continue;
                }
            }
          }
        }
      }
  }

  void forceAugmentedReturns() { 
      for(BasicBlock* BB: this->originalBlocks) {
        LoopContext loopContext;
        this->getContext(BB, loopContext);
      
        auto term = BB->getTerminator();
        if (isa<UnreachableInst>(term)) continue;
      
        for (auto I = BB->begin(), E = BB->end(); I != E;) {
          Instruction* inst = &*I;
          assert(inst);
          I++;

          if (!isa<CallInst>(inst)) continue;
          CallInst* op = dyn_cast<CallInst>(inst);

          Function *called = op->getCalledFunction();

          if(!called) continue;
          if (called->empty()) continue;
          if (this->isConstantValue(op)) continue;

          if (isCertainPrintMallocOrFree(called)) continue;

          if (!called->getReturnType()->isPointerTy() && !called->getReturnType()->isIntegerTy()) continue;

          if (this->invertedPointers.find(called) != this->invertedPointers.end()) continue;
            IRBuilder<> BuilderZ(op->getNextNonDebugInstruction());
            BuilderZ.setFastMathFlags(FastMathFlags::getFast());
            this->invertedPointers[op] = BuilderZ.CreatePHI(called->getReturnType(), 1);
        }
      }
  }
  
  Value* unwrapM(Value* val, IRBuilder<>& BuilderM, const ValueToValueMapTy& available, bool lookupIfAble) {
          assert(val);
          if (available.count(val)) {
            return available.lookup(val);
          } 

          if (isa<Argument>(val) || isa<Constant>(val)) {
            return val;
          } else if (isa<AllocaInst>(val)) {
            return val;
          } else if (auto op = dyn_cast<CastInst>(val)) {
            auto op0 = unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble);
            if (op0 == nullptr) goto endCheck;
            return BuilderM.CreateCast(op->getOpcode(), op0, op->getDestTy(), op->getName()+"_unwrap");
          } else if (auto op = dyn_cast<BinaryOperator>(val)) {
            auto op0 = unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble);
            if (op0 == nullptr) goto endCheck;
            auto op1 = unwrapM(op->getOperand(1), BuilderM, available, lookupIfAble);
            if (op1 == nullptr) goto endCheck;
            return BuilderM.CreateBinOp(op->getOpcode(), op0, op1);
          } else if (auto op = dyn_cast<ICmpInst>(val)) {
            auto op0 = unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble);
            if (op0 == nullptr) goto endCheck;
            auto op1 = unwrapM(op->getOperand(1), BuilderM, available, lookupIfAble);
            if (op1 == nullptr) goto endCheck;
            return BuilderM.CreateICmp(op->getPredicate(), op0, op1);
          } else if (auto op = dyn_cast<FCmpInst>(val)) {
            auto op0 = unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble);
            if (op0 == nullptr) goto endCheck;
            auto op1 = unwrapM(op->getOperand(1), BuilderM, available, lookupIfAble);
            if (op1 == nullptr) goto endCheck;
            return BuilderM.CreateFCmp(op->getPredicate(), op0, op1);
          } else if (auto op = dyn_cast<SelectInst>(val)) {
            auto op0 = unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble);
            if (op0 == nullptr) goto endCheck;
            auto op1 = unwrapM(op->getOperand(1), BuilderM, available, lookupIfAble);
            if (op1 == nullptr) goto endCheck;
            auto op2 = unwrapM(op->getOperand(2), BuilderM, available, lookupIfAble);
            if (op2 == nullptr) goto endCheck;
            return BuilderM.CreateSelect(op0, op1, op2);
          } else if (auto inst = dyn_cast<GetElementPtrInst>(val)) {
              auto ptr = unwrapM(inst->getPointerOperand(), BuilderM, available, lookupIfAble);
              if (ptr == nullptr) goto endCheck;
              SmallVector<Value*,4> ind;
              for(auto& a : inst->indices()) {
                auto op = unwrapM(a, BuilderM,available, lookupIfAble);
                if (op == nullptr) goto endCheck;
                ind.push_back(op);
              }
              return BuilderM.CreateGEP(ptr, ind);
          } else if (auto load = dyn_cast<LoadInst>(val)) {
                Value* idx = unwrapM(load->getOperand(0), BuilderM, available, lookupIfAble);
                if (idx == nullptr) goto endCheck;
                return BuilderM.CreateLoad(idx);
          } else if (auto op = dyn_cast<IntrinsicInst>(val)) {
            switch(op->getIntrinsicID()) {
                case Intrinsic::sin: {
                  Value *args[] = {unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble)};
                  if (args[0] == nullptr) goto endCheck;
                  Type *tys[] = {op->getOperand(0)->getType()};
                  return BuilderM.CreateCall(Intrinsic::getDeclaration(op->getParent()->getParent()->getParent(), Intrinsic::sin, tys), args);
                }
                case Intrinsic::cos: {
                  Value *args[] = {unwrapM(op->getOperand(0), BuilderM, available, lookupIfAble)};
                  if (args[0] == nullptr) goto endCheck;
                  Type *tys[] = {op->getOperand(0)->getType()};
                  return BuilderM.CreateCall(Intrinsic::getDeclaration(op->getParent()->getParent()->getParent(), Intrinsic::cos, tys), args);
                }
                default:;

            }
          } else if (auto phi = dyn_cast<PHINode>(val)) {
            if (phi->getNumIncomingValues () == 1) {
                return unwrapM(phi->getIncomingValue(0), BuilderM, available, lookupIfAble);
            }
          }


endCheck:
            assert(val);
            llvm::errs() << "cannot unwrap following " << *val << "\n";
            if (lookupIfAble)
                return lookupM(val, BuilderM);
          
          if (auto inst = dyn_cast<Instruction>(val)) {
            //LoopContext lc;
            // if (BuilderM.GetInsertBlock() != inversionAllocs && !( (reverseBlocks.find(BuilderM.GetInsertBlock()) != reverseBlocks.end())  && /*inLoop*/getContext(inst->getParent(), lc)) ) {
            if (isOriginalBlock(*BuilderM.GetInsertBlock())) {
                if (BuilderM.GetInsertBlock()->size() && BuilderM.GetInsertPoint() != BuilderM.GetInsertBlock()->end()) {
                    if (DT.dominates(inst, &*BuilderM.GetInsertPoint())) {
                        //llvm::errs() << "allowed " << *inst << "from domination\n";
                        return inst;
                    }
                } else {
                    if (DT.dominates(inst, BuilderM.GetInsertBlock())) {
                        //llvm::errs() << "allowed " << *inst << "from block domination\n";
                        return inst;
                    }
                }
            }
          }
            return nullptr;
            report_fatal_error("unable to unwrap");
    }
    Value* lookupM(Value* val, IRBuilder<>& BuilderM) {
        if (isa<Constant>(val)) return val;
        auto M = BuilderM.GetInsertBlock()->getParent()->getParent();
        if (auto inst = dyn_cast<Instruction>(val)) {
            if (this->inversionAllocs && inst->getParent() == this->inversionAllocs) {
                return val;
            }
            
            if (this->isOriginalBlock(*BuilderM.GetInsertBlock())) {
                if (BuilderM.GetInsertBlock()->size() && BuilderM.GetInsertPoint() != BuilderM.GetInsertBlock()->end()) {
                    if (this->DT.dominates(inst, &*BuilderM.GetInsertPoint())) {
                        //llvm::errs() << "allowed " << *inst << "from domination\n";
                        return inst;
                    }
                } else {
                    if (this->DT.dominates(inst, BuilderM.GetInsertBlock())) {
                        //llvm::errs() << "allowed " << *inst << "from block domination\n";
                        return inst;
                    }
                }
            }

            assert(!this->isOriginalBlock(*BuilderM.GetInsertBlock()));
            LoopContext lc;
            bool inLoop = getContext(inst->getParent(), lc);

            ValueToValueMapTy emptyMap;
            
            ValueToValueMapTy available;
            if (inLoop) {
                for(LoopContext idx = lc; ; getContext(idx.parent->getHeader(), idx)) {
                  available[idx.var] = idx.antivar;
                  if (idx.parent == nullptr) break;
                }
                
                bool isChildLoop = false;

                auto builderLoop = LI.getLoopFor(originalForReverseBlock(*BuilderM.GetInsertBlock()));
                while (builderLoop) {
                  if (builderLoop->getHeader() == lc.header) {
                    isChildLoop = true;
                    break;
                  }
                  builderLoop = builderLoop->getParentLoop();
                }
                if (!isChildLoop) {
                    llvm::errs() << "manually performing lcssa for instruction" << *inst << " in block " << BuilderM.GetInsertBlock()->getName() << "\n";
                    if (!DT.dominates(inst, originalForReverseBlock(*BuilderM.GetInsertBlock()))) {
                        this->newFunc->dump();
                        originalForReverseBlock(*BuilderM.GetInsertBlock())->dump();
                        BuilderM.GetInsertBlock()->dump();
                        inst->dump();
                    }
                    assert(DT.dominates(inst, originalForReverseBlock(*BuilderM.GetInsertBlock())));
                    IRBuilder<> lcssa(&lc.exit->front());
                    auto lcssaPHI = lcssa.CreatePHI(inst->getType(), 1, inst->getName()+"!manual_lcssa");
                    for(auto pred : predecessors(lc.exit))
                        lcssaPHI->addIncoming(inst, pred);
                    return lookupM(lcssaPHI, BuilderM);
                }
            }
            if (!shouldRecompute(inst, available)) {
                auto op = unwrapM(inst, BuilderM, available, /*lookupIfAble*/true);
                assert(op);
                return op;
            }

            assert(inversionAllocs && "must be able to allocate inverted caches");
            IRBuilder<> entryBuilder(inversionAllocs);
            entryBuilder.setFastMathFlags(FastMathFlags::getFast());

            if (!inLoop) {
                if (scopeMap.find(val) == scopeMap.end()) {
                    scopeMap[val] = entryBuilder.CreateAlloca(val->getType(), nullptr, val->getName()+"_cache");
                    auto pn = dyn_cast<PHINode>(inst);
                    Instruction* putafter = ( pn && pn->getNumIncomingValues()>0 )? (inst->getParent()->getFirstNonPHI() ): inst->getNextNonDebugInstruction();
                    IRBuilder <> v(putafter);
                    v.setFastMathFlags(FastMathFlags::getFast());
                    v.CreateStore(val, scopeMap[val]);
                }
                auto result = BuilderM.CreateLoad(scopeMap[val]);
                return result;
            } else {
                if (scopeMap.find(val) == scopeMap.end()) {

                    ValueToValueMapTy valmap;
                    Value* size = nullptr;

                    BasicBlock* outermostPreheader = nullptr;

                    for(LoopContext idx = lc; ; getContext(idx.parent->getHeader(), idx) ) {
                        if (idx.parent == nullptr) {
                            outermostPreheader = idx.preheader;
                        }
                        if (idx.parent == nullptr) break;
                    }
                    assert(outermostPreheader);

                    IRBuilder <> allocationBuilder(&outermostPreheader->back());

                    for(LoopContext idx = lc; ; getContext(idx.parent->getHeader(), idx) ) {
					  //TODO handle allocations for dynamic loops
					  if (idx.dynamic && idx.parent != nullptr) {
                        assert(idx.var);
                        assert(idx.var->getParent());
                        assert(idx.var->getParent()->getParent());
                        llvm::errs() << *idx.var->getParent()->getParent() << "\n"
                            << "idx.var=" <<*idx.var << "\n"
                            << "idx.limit=" <<*idx.limit << "\n";
                        llvm::errs() << "cannot handle non-outermost dynamic loop\n";
						assert(0 && "cannot handle non-outermost dynamic loop");
					  }
                      Value* ns = nullptr;
					  if (idx.dynamic) {
						ns = ConstantInt::get(idx.limit->getType(), 1);
					  } else {
                        Value* limitm1 = nullptr;
                        limitm1 = unwrapM(idx.limit, allocationBuilder, emptyMap, /*lookupIfAble*/false);
                        if (limitm1 == nullptr) {
                            assert(outermostPreheader);
                            assert(outermostPreheader->getParent());
                            llvm::errs() << *outermostPreheader->getParent() << "\n";
                            llvm::errs() << "needed value " << *idx.limit << " at " << allocationBuilder.GetInsertBlock()->getName() << "\n";
                        }
                        assert(limitm1);
						ns = allocationBuilder.CreateNUWAdd(limitm1, ConstantInt::get(idx.limit->getType(), 1));
					  }
                      if (size == nullptr) size = ns;
                      else size = allocationBuilder.CreateNUWMul(size, ns);
                      if (idx.parent == nullptr) break;
                    }

                    auto firstallocation = CallInst::CreateMalloc(
                            &allocationBuilder.GetInsertBlock()->back(),
                            size->getType(),
                            val->getType(),
                            ConstantInt::get(size->getType(), allocationBuilder.GetInsertBlock()->getParent()->getParent()->getDataLayout().getTypeAllocSizeInBits(val->getType())/8), size, nullptr, val->getName()+"_malloccache");
                    //allocationBuilder.GetInsertBlock()->getInstList().push_back(cast<Instruction>(allocation));
                    cast<Instruction>(firstallocation)->moveBefore(allocationBuilder.GetInsertBlock()->getTerminator());
                    scopeMap[val] = entryBuilder.CreateAlloca(firstallocation->getType(), nullptr, val->getName()+"_mdyncache");
                    allocationBuilder.CreateStore(firstallocation, scopeMap[val]);	

                      if (reverseBlocks.size() != 0) {
                        IRBuilder<> tbuild(reverseBlocks[outermostPreheader]);
                        tbuild.setFastMathFlags(FastMathFlags::getFast());

                        // ensure we are before the terminator if it exists
                        if (tbuild.GetInsertBlock()->size()) {
                              tbuild.SetInsertPoint(tbuild.GetInsertBlock()->getFirstNonPHI());
                        }
                      	
                        auto ci = CallInst::CreateFree(tbuild.CreatePointerCast(tbuild.CreateLoad(scopeMap[val]), Type::getInt8PtrTy(outermostPreheader->getContext())), tbuild.GetInsertBlock());
                        if (ci->getParent()==nullptr) {
                            tbuild.Insert(ci);
                        }
                      } else {
                          llvm::errs() << "warning not freeing lookupM allocation\n";
                          report_fatal_error("not freeing lookupM allocation");
                      }

                    Instruction* putafter = isa<PHINode>(inst) ? (inst->getParent()->getFirstNonPHI() ): inst->getNextNonDebugInstruction();
                    IRBuilder <> v(putafter);
                    v.setFastMathFlags(FastMathFlags::getFast());

                    SmallVector<Value*,3> indices;
                    SmallVector<Value*,3> limits;
					PHINode* dynamicPHI = nullptr;

                    for(LoopContext idx = lc; ; getContext(idx.parent->getHeader(), idx) ) {
                      indices.push_back(idx.var);
                        
					  if (idx.dynamic) {
						dynamicPHI = idx.var;
                        assert(dynamicPHI);
						llvm::errs() << "saw idx.dynamic:" << *dynamicPHI << "\n";
						assert(idx.parent == nullptr);
						break;
					  }

                      if (idx.parent == nullptr) break;
                      auto limitm1 = unwrapM(idx.limit, v, emptyMap, /*lookupIfAble*/false);
                      assert(limitm1);
                      auto lim = v.CreateNUWAdd(limitm1, ConstantInt::get(idx.limit->getType(), 1));
                      if (limits.size() != 0) {
                        lim = v.CreateNUWMul(lim, limits.back());
                      }
                      limits.push_back(lim);
                    }

                    Value* idx = nullptr;
                    for(unsigned i=0; i<indices.size(); i++) {
                      if (i == 0) {
                        idx = indices[i];
                      } else {
                        auto mul = v.CreateNUWMul(indices[i], limits[i-1]);
                        idx = v.CreateNUWAdd(idx, mul);
                      }
                    }

					Value* allocation = nullptr;
					if (dynamicPHI == nullptr) {
						//allocation = scopeMap[val];
						IRBuilder<> outerBuilder(&outermostPreheader->back());
						allocation = outerBuilder.CreateLoad(scopeMap[val]);
					} else {
						Type *BPTy = Type::getInt8PtrTy(v.GetInsertBlock()->getContext());
						auto realloc = M->getOrInsertFunction("realloc", BPTy, BPTy, size->getType());
						allocation = v.CreateLoad(scopeMap[val]);
						auto foo = v.CreateNUWAdd(dynamicPHI, ConstantInt::get(dynamicPHI->getType(), 1));
						Value* idxs[2] = {
							v.CreatePointerCast(allocation, BPTy),
							v.CreateNUWMul(
								ConstantInt::get(size->getType(), M->getDataLayout().getTypeAllocSizeInBits(val->getType())/8), 
								v.CreateNUWMul(
									size, foo
								) 
							)
						};

                        Value* realloccall = nullptr;
						allocation = v.CreatePointerCast(realloccall = v.CreateCall(realloc, idxs, val->getName()+"_realloccache"), allocation->getType());
						v.CreateStore(allocation, scopeMap[val]);
					}

                    Value* idxs[] = {idx};
                    auto gep = v.CreateGEP(allocation, idxs);
					v.CreateStore(val, gep);
                }
                assert(inLoop);

                SmallVector<Value*,3> indices;
                SmallVector<Value*,3> limits;
                for(LoopContext idx = lc; ; getContext(idx.parent->getHeader(), idx) ) {
                  indices.push_back(idx.antivar);
                  if (idx.parent == nullptr) break;

                  auto limitm1 = unwrapM(idx.limit, BuilderM, available, /*lookupIfAble*/true);
                  assert(limitm1);
                  auto lim = BuilderM.CreateNUWAdd(limitm1, ConstantInt::get(idx.limit->getType(), 1));
                  if (limits.size() != 0) {
                    lim = BuilderM.CreateNUWMul(lim, limits.back());
                  }
                  limits.push_back(lim);
                }

                Value* idx = nullptr;
                for(unsigned i=0; i<indices.size(); i++) {
                  if (i == 0) {
                    idx = indices[i];
                  } else {
                    idx = BuilderM.CreateNUWAdd(idx, BuilderM.CreateNUWMul(indices[i], limits[i-1]));
                  }
                }

                Value* idxs[] = {idx};
				Value* tolookup = BuilderM.CreateLoad(scopeMap[val]);
                return BuilderM.CreateLoad(BuilderM.CreateGEP(tolookup, idxs));
            }
        }

        return val;
    };
    
    Value* invertPointerM(Value* val, IRBuilder<>& BuilderM) {
      if (isa<ConstantPointerNull>(val)) {
         return val;
      } else if (isa<UndefValue>(val)) {
         return val;
      } else if (auto cint = dyn_cast<ConstantInt>(val)) {
		 if (cint->isZero()) return cint;
         //this is extra
		 if (cint->isOne()) return cint;
	  }

      if(isConstantValue(val)) {
        if (auto arg = dyn_cast<Instruction>(val)) {
            arg->getParent()->getParent()->dump();
        }
        val->dump();
      }
      assert(!isConstantValue(val));
      auto M = BuilderM.GetInsertBlock()->getParent()->getParent();
      assert(val);

      if (invertedPointers.find(val) != invertedPointers.end()) {
        return lookupM(invertedPointers[val], BuilderM);
      }

      if (auto arg = dyn_cast<CastInst>(val)) {
        auto result = BuilderM.CreateCast(arg->getOpcode(), invertPointerM(arg->getOperand(0), BuilderM), arg->getDestTy(), arg->getName()+"'ipc");
        return result;
      } else if (auto arg = dyn_cast<ExtractValueInst>(val)) {
        IRBuilder<> bb(arg);
        auto result = bb.CreateExtractValue(invertPointerM(arg->getOperand(0), bb), arg->getIndices(), arg->getName()+"'ipev");
        invertedPointers[arg] = result;
        return lookupM(invertedPointers[arg], BuilderM);
      } else if (auto arg = dyn_cast<InsertValueInst>(val)) {
        IRBuilder<> bb(arg);
        auto result = bb.CreateInsertValue(invertPointerM(arg->getOperand(0), bb), invertPointerM(arg->getOperand(1), bb), arg->getIndices(), arg->getName()+"'ipiv");
        invertedPointers[arg] = result;
        return lookupM(invertedPointers[arg], BuilderM);
      } else if (auto arg = dyn_cast<LoadInst>(val)) {
        auto li = BuilderM.CreateLoad(invertPointerM(arg->getOperand(0), BuilderM), arg->getName()+"'ipl");
        li->setAlignment(arg->getAlignment());
        return li;
      } else if (auto arg = dyn_cast<GetElementPtrInst>(val)) {
        SmallVector<Value*,4> invertargs;
        for(auto &a: arg->indices()) {
            auto b = lookupM(a, BuilderM);
            invertargs.push_back(b);
        }
        auto result = BuilderM.CreateGEP(invertPointerM(arg->getPointerOperand(), BuilderM), invertargs, arg->getName()+"'ipg");
        return result;
      } else if (auto inst = dyn_cast<AllocaInst>(val)) {
            IRBuilder<> bb(inst);
            AllocaInst* antialloca = bb.CreateAlloca(inst->getAllocatedType(), inst->getType()->getPointerAddressSpace(), inst->getArraySize(), inst->getName()+"'ipa");
            invertedPointers[val] = antialloca;
            antialloca->setAlignment(inst->getAlignment());
            Value *args[] = {bb.CreateBitCast(antialloca,Type::getInt8PtrTy(val->getContext())), ConstantInt::get(Type::getInt8Ty(val->getContext()), 0), bb.CreateNUWMul(
            bb.CreateZExtOrTrunc(inst->getArraySize(),Type::getInt64Ty(val->getContext())),
                ConstantInt::get(Type::getInt64Ty(val->getContext()), M->getDataLayout().getTypeAllocSizeInBits(inst->getAllocatedType())/8 ) ), ConstantInt::getFalse(val->getContext()) };
            Type *tys[] = {args[0]->getType(), args[2]->getType()};
            auto memset = cast<CallInst>(bb.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memset, tys), args));
            memset->addParamAttr(0, Attribute::getWithAlignment(inst->getContext(), inst->getAlignment()));
            memset->addParamAttr(0, Attribute::NonNull);
            return lookupM(invertedPointers[inst], BuilderM);
      } else if (auto call = dyn_cast<CallInst>(val)) {
        if (call->getCalledFunction() && (call->getCalledFunction()->getName() == "malloc" || call->getCalledFunction()->getName() == "_Znwm") ) {
                IRBuilder<> bb(call);
                {
                SmallVector<Value*, 8> args;
                for(unsigned i=0;i < call->getNumArgOperands(); i++) {
                    args.push_back(call->getArgOperand(i));
                }
                invertedPointers[val] = bb.CreateCall(call->getCalledFunction(), args, call->getName()+"'mi");
                }

                cast<CallInst>(invertedPointers[val])->setAttributes(call->getAttributes());

                {
            Value *nargs[] = {
                bb.CreateBitCast(invertedPointers[val],Type::getInt8PtrTy(val->getContext())),
                ConstantInt::get(Type::getInt8Ty(val->getContext()), 0),
                bb.CreateZExtOrTrunc(call->getArgOperand(0), Type::getInt64Ty(val->getContext())), ConstantInt::getFalse(val->getContext())
            };
            Type *tys[] = {nargs[0]->getType(), nargs[2]->getType()};

            auto memset = cast<CallInst>(bb.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memset, tys), nargs));
            //memset->addParamAttr(0, Attribute::getWithAlignment(Context, inst->getAlignment()));
            memset->addParamAttr(0, Attribute::NonNull);
            invertedPointers[val] = addMallocAndMemset(bb, cast<Instruction>(invertedPointers[val]), cast<Instruction>(memset));
                }

                if (reverseBlocks.size()) {
                    IRBuilder<> freeBuilder(reverseBlocks[call->getParent()]);
                    if (auto term = freeBuilder.GetInsertBlock()->getTerminator()) {
                        freeBuilder.SetInsertPoint(term);
                    }
                    freeBuilder.setFastMathFlags(FastMathFlags::getFast());
                    Instruction* ci;
                    if (call->getCalledFunction()->getName() == "malloc")
                      ci = CallInst::CreateFree(freeBuilder.CreatePointerCast(lookupM(invertedPointers[val], freeBuilder), Type::getInt8PtrTy(call->getContext())), freeBuilder.GetInsertBlock());
                    else {
                      Type *VoidTy = Type::getVoidTy(M->getContext());
                      Type *IntPtrTy = Type::getInt8PtrTy(M->getContext());
                      // or should do _ZdlPv vs _ZdlPvm
                      auto FreeFunc = M->getOrInsertFunction("_ZdlPv", VoidTy, IntPtrTy);
                      ci = CallInst::Create(FreeFunc, {freeBuilder.CreatePointerCast(lookupM(invertedPointers[val], freeBuilder), Type::getInt8PtrTy(call->getContext()))}, "", freeBuilder.GetInsertBlock());
                      cast<CallInst>(ci)->setTailCall();
                      if (Function *F = dyn_cast<Function>(FreeFunc))
                        cast<CallInst>(ci)->setCallingConv(F->getCallingConv());
                    }
                    if (ci->getParent()==nullptr) {
                      freeBuilder.Insert(ci);
                    }
                    addedFrees.push_back(ci);
                } else {
                }

            return lookupM(invertedPointers[val], BuilderM);
        }
      
      } else if (auto phi = dyn_cast<PHINode>(val)) {
		 std::map<Value*,std::set<BasicBlock*>> mapped;
		 for(unsigned int i=0; i<phi->getNumIncomingValues(); i++) {
			mapped[phi->getIncomingValue(i)].insert(phi->getIncomingBlock(i));
		 }

		 if (false && mapped.size() == 1) {
         	return invertPointerM(phi->getIncomingValue(0), BuilderM);
		 }    
#if 0
         else if (false && mapped.size() == 2) {
			 IRBuilder <> bb(phi);
			 auto which = bb.CreatePHI(Type::getInt1Ty(phi->getContext()), phi->getNumIncomingValues());
             //TODO this is not recursive

			 int cnt = 0;
			 Value* vals[2];
			 for(auto v : mapped) {
				assert( cnt <= 1 );
				vals[cnt] = v.first;
				for (auto b : v.second) {
					which->addIncoming(ConstantInt::get(which->getType(), cnt), b);
				}
				cnt++;
			 }
			 
			 auto which2 = lookupM(which, BuilderM);
			 auto result = BuilderM.CreateSelect(which2, invertPointerM(vals[1], BuilderM), invertPointerM(vals[0], BuilderM));
             return result;
		 } 
#endif
         
         else {
			 IRBuilder <> bb(phi);
			 auto which = bb.CreatePHI(phi->getType(), phi->getNumIncomingValues());
             invertedPointers[val] = which;
		 
             for(unsigned int i=0; i<phi->getNumIncomingValues(); i++) {
				IRBuilder <>pre(phi->getIncomingBlock(i)->getTerminator());
				which->addIncoming(invertPointerM(phi->getIncomingValue(i), pre), phi->getIncomingBlock(i));
             }

			 return lookupM(which, BuilderM);
		 }
        }
        assert(BuilderM.GetInsertBlock());
        assert(BuilderM.GetInsertBlock()->getParent());
        assert(val);
        llvm::errs() << "fn:" << *BuilderM.GetInsertBlock()->getParent() << "\nval=" << *val << "\n";
        for(auto z : invertedPointers) {
          llvm::errs() << "available inversion for " << *z.first << " of " << *z.second << "\n"; 
        }
        assert(0 && "cannot find deal with ptr that isnt arg");
        report_fatal_error("cannot find deal with ptr that isnt arg");
      
    };
};
  
class DiffeGradientUtils : public GradientUtils {
  DiffeGradientUtils(Function* newFunc_, TargetLibraryInfo &TLI, ValueToValueMapTy& invertedPointers_, const SmallPtrSetImpl<Value*> &constants_, const SmallPtrSetImpl<Value*> &nonconstant_, const SmallPtrSetImpl<Value*> &returnvals_, ValueToValueMapTy &origToNew_)
      : GradientUtils(newFunc_, TLI, invertedPointers_, constants_, nonconstant_, returnvals_, origToNew_) {
        prepareForReverse();
        inversionAllocs = BasicBlock::Create(newFunc_->getContext(), "allocsForInversion", newFunc);
    }

public:
  ValueToValueMapTy differentials;
  static DiffeGradientUtils* CreateFromClone(Function *todiff, TargetLibraryInfo &TLI, const SmallSet<unsigned,4> & constant_args, ReturnType returnValue, bool differentialReturn, Type* additionalArg) {
    assert(!todiff->empty());
    ValueToValueMapTy invertedPointers;
    SmallPtrSet<Value*,4> constants;
    SmallPtrSet<Value*,20> nonconstant;
    SmallPtrSet<Value*,2> returnvals;
    ValueToValueMapTy originalToNew;
    auto newFunc = CloneFunctionWithReturns(todiff, invertedPointers, constant_args, constants, nonconstant, returnvals, returnValue, differentialReturn, "diffe"+todiff->getName(), &originalToNew, /*diffeReturnArg*/true, additionalArg);
    return new DiffeGradientUtils(newFunc, TLI, invertedPointers, constants, nonconstant, returnvals, originalToNew);
  }

private:
  Value* getDifferential(Value *val) {
    assert(val);
    assert(inversionAllocs);
    if (differentials.find(val) == differentials.end()) {
        IRBuilder<> entryBuilder(inversionAllocs);
        entryBuilder.setFastMathFlags(FastMathFlags::getFast());
        differentials[val] = entryBuilder.CreateAlloca(val->getType(), nullptr, val->getName()+"'de");
        entryBuilder.CreateStore(Constant::getNullValue(val->getType()), differentials[val]);
    }
    return differentials[val];
  }

public:
  Value* diffe(Value* val, IRBuilder<> &BuilderM) {
      if (val->getType()->isPointerTy()) {
        newFunc->dump();
        val->dump();
      }
      if (isConstantValue(val)) {
        newFunc->dump();
        val->dump();
      }
      assert(!val->getType()->isPointerTy());
      assert(!val->getType()->isVoidTy());
      return BuilderM.CreateLoad(getDifferential(val));
  }

  void addToDiffe(Value* val, Value* dif, IRBuilder<> &BuilderM) {
      if (val->getType()->isPointerTy()) {
        newFunc->dump();
        val->dump();
      }
      if (isConstantValue(val)) {
        newFunc->dump();
        val->dump();
      }
      assert(!val->getType()->isPointerTy());
      assert(!isConstantValue(val));
      assert(val->getType() == dif->getType());
      auto old = diffe(val, BuilderM);
      assert(val->getType() == old->getType());
      Value* res;
      if (val->getType()->isIntOrIntVectorTy()) {
        res = BuilderM.CreateFAdd(BuilderM.CreateBitCast(old, IntToFloatTy(old->getType())), BuilderM.CreateBitCast(dif, IntToFloatTy(dif->getType())));
        res = BuilderM.CreateBitCast(res, val->getType());
        BuilderM.CreateStore(res, getDifferential(val));
      } else if (val->getType()->isFPOrFPVectorTy()) {
        res = BuilderM.CreateFAdd(old, dif);
        BuilderM.CreateStore(res, getDifferential(val));
      } else if (val->getType()->isStructTy()) {
        auto st = cast<StructType>(val->getType());
        for(unsigned i=0; i<st->getNumElements(); i++) {
            Value* v = ConstantInt::get(Type::getInt32Ty(st->getContext()), i);
            addToDiffeIndexed(val, BuilderM.CreateExtractValue(dif,{i}), {v}, BuilderM);
        }
      } else {
        assert(0 && "lol");
        exit(1);
      }
  }

  void setDiffe(Value* val, Value* toset, IRBuilder<> &BuilderM) {
      assert(!isConstantValue(val));
      BuilderM.CreateStore(toset, getDifferential(val));
  }

  void addToDiffeIndexed(Value* val, Value* dif, ArrayRef<Value*> idxs, IRBuilder<> &BuilderM) {
      assert(!isConstantValue(val));
      SmallVector<Value*,4> sv;
      sv.push_back(ConstantInt::get(Type::getInt32Ty(val->getContext()), 0));
      for(auto i : idxs)
        sv.push_back(i);
      auto ptr = BuilderM.CreateGEP(getDifferential(val), sv);
      BuilderM.CreateStore(BuilderM.CreateFAdd(BuilderM.CreateLoad(ptr), dif), ptr);
  }

  void addToPtrDiffe(Value* val, Value* dif, IRBuilder<> &BuilderM) {
      auto ptr = invertPointerM(val, BuilderM);
      Value* res;
      Value* old = BuilderM.CreateLoad(ptr);
      if (old->getType()->isIntOrIntVectorTy()) {
        res = BuilderM.CreateFAdd(BuilderM.CreateBitCast(old, IntToFloatTy(old->getType())), BuilderM.CreateBitCast(dif, IntToFloatTy(dif->getType())));
        res = BuilderM.CreateBitCast(res, old->getType());
      } else if(old->getType()->isFPOrFPVectorTy()) {
        res = BuilderM.CreateFAdd(old, dif);
      } else {
        assert(old);
        assert(dif);
        llvm::errs() << *newFunc << "\n" << "cannot handle type " << *old << "\n" << *dif;
        report_fatal_error("cannot handle type");
      }
      BuilderM.CreateStore(res, ptr);
  }
  
  void setPtrDiffe(Value* ptr, Value* newval, IRBuilder<> &BuilderM) {
      ptr = invertPointerM(ptr, BuilderM);
      BuilderM.CreateStore(newval, ptr);
  }
};

static cl::opt<bool> autodiff_optimize(
            "autodiff_optimize", cl::init(false), cl::Hidden,
                cl::desc("Force inlining of autodiff"));
void optimizeIntermediate(GradientUtils* gutils, bool topLevel, Function *F) {
    if (!autodiff_optimize) return;

    {
        DominatorTree DT(*F);
        AssumptionCache AC(*F);
        promoteMemoryToRegister(*F, DT, AC);
    }

    FunctionAnalysisManager AM;
     AM.registerPass([] { return AAManager(); });
     AM.registerPass([] { return ScalarEvolutionAnalysis(); });
     AM.registerPass([] { return AssumptionAnalysis(); });
     AM.registerPass([] { return TargetLibraryAnalysis(); });
     AM.registerPass([] { return TargetIRAnalysis(); });
     AM.registerPass([] { return MemorySSAAnalysis(); });
     AM.registerPass([] { return DominatorTreeAnalysis(); });
     AM.registerPass([] { return MemoryDependenceAnalysis(); });
     AM.registerPass([] { return LoopAnalysis(); });
     AM.registerPass([] { return OptimizationRemarkEmitterAnalysis(); });
     AM.registerPass([] { return PhiValuesAnalysis(); });
     AM.registerPass([] { return LazyValueAnalysis(); });
     LoopAnalysisManager LAM;
     AM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(LAM); });
     LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(AM); });
    //LoopSimplifyPass().run(*F, AM);

 //TODO function attributes
 //PostOrderFunctionAttrsPass().run(*F, AM);
 GVN().run(*F, AM);
 SROA().run(*F, AM);
 EarlyCSEPass(/*memoryssa*/true).run(*F, AM);
 InstSimplifyPass().run(*F, AM);
 CorrelatedValuePropagationPass().run(*F, AM);

 DCEPass().run(*F, AM);
 DSEPass().run(*F, AM);

 createFunctionToLoopPassAdaptor(LoopDeletionPass()).run(*F, AM);
 
 SimplifyCFGOptions scfgo(/*unsigned BonusThreshold=*/1, /*bool ForwardSwitchCond=*/false, /*bool SwitchToLookup=*/false, /*bool CanonicalLoops=*/true, /*bool SinkCommon=*/true, /*AssumptionCache *AssumpCache=*/nullptr);
 SimplifyCFGPass(scfgo).run(*F, AM);
    
 if (!topLevel) {
 for(BasicBlock& BB: *F) { 
      
        for (auto I = BB.begin(), E = BB.end(); I != E;) {
          Instruction* inst = &*I;
          assert(inst);
          I++;

          if (gutils->originalInstructions.find(inst) == gutils->originalInstructions.end()) continue;

          if (gutils->replaceableCalls.find(inst) != gutils->replaceableCalls.end()) {
            if (inst->getNumUses() != 0 && !cast<CallInst>(inst)->getCalledFunction()->hasFnAttribute(Attribute::ReadNone) ) {
                llvm::errs() << "found call ripe for replacement " << *inst;
            } else {
                    inst->eraseFromParent();
                    continue;
            }
          }
        }
      }
 }
 //LCSSAPass().run(*NewF, AM);
}

Function* CreateAugmentedPrimal(Function* todiff, const SmallSet<unsigned,4>& constant_args, TargetLibraryInfo &TLI, GradientUtils** oututils, bool differentialReturn) {
  assert(!todiff->empty());
  
#if 0
  static std::map<std::tuple<Function*,std::set<unsigned>, bool/*differentialReturn*/>, Function*> cachedfunctions;
  auto tup = std::make_tuple(todiff, std::set<unsigned>(constant_args.begin(), constant_args.end()),  differentialReturn);
  if (cachedfunctions.find(tup) != cachedfunctions.end()) {
    return cachedfunctions[tup];
  }
#endif

  GradientUtils *gutils = GradientUtils::CreateFromClone(todiff, TLI, constant_args, /*returnValue*/ReturnType::Normal, /*differentialReturn*/differentialReturn);
  llvm::errs() << "function with differential return " << todiff->getName() << " " << differentialReturn << "\n";
  gutils->forceAugmentedReturns();

  for(BasicBlock* BB: gutils->originalBlocks) {
      auto term = BB->getTerminator();
      assert(term);
      if(isa<ReturnInst>(term)) {
      } else if (isa<BranchInst>(term) || isa<SwitchInst>(term)) {

      } else if (isa<UnreachableInst>(term)) {
      
      } else {
        assert(BB);
        assert(BB->getParent());
        assert(term);
        llvm::errs() << *BB->getParent() << "\n";
        llvm::errs() << "unknown terminator instance " << *term << "\n";
        assert(0 && "unknown terminator inst");
      }

      if (!isa<UnreachableInst>(term))
      for (auto I = BB->begin(), E = BB->end(); I != E;) {
        Instruction* inst = &*I;
        assert(inst);
        I++;
        if (gutils->originalInstructions.find(inst) == gutils->originalInstructions.end()) continue;

        if(auto op = dyn_cast_or_null<IntrinsicInst>(inst)) {
          switch(op->getIntrinsicID()) {
            case Intrinsic::memcpy: {
                if (gutils->isConstantInstruction(inst)) continue;
                /*
                SmallVector<Value*, 4> args;
                args.push_back(invertPointer(op->getOperand(0)));
                args.push_back(invertPointer(op->getOperand(1)));
                args.push_back(lookup(op->getOperand(2)));
                args.push_back(lookup(op->getOperand(3)));

                Type *tys[] = {args[0]->getType(), args[1]->getType(), args[2]->getType()};
                auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memcpy, tys), args);
                cal->setAttributes(op->getAttributes());
                */
                break;
            }
            case Intrinsic::memset: {
                if (gutils->isConstantInstruction(inst)) continue;
                /*
                if (!gutils->isConstantValue(op->getOperand(1))) {
                    assert(inst);
                    llvm::errs() << "couldn't handle non constant inst in memset to propagate differential to\n" << *inst;
                    report_fatal_error("non constant in memset");
                }
                auto ptx = invertPointer(op->getOperand(0));
                SmallVector<Value*, 4> args;
                args.push_back(ptx);
                args.push_back(lookup(op->getOperand(1)));
                args.push_back(lookup(op->getOperand(2)));
                args.push_back(lookup(op->getOperand(3)));

                Type *tys[] = {args[0]->getType(), args[2]->getType()};
                auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memset, tys), args);
                cal->setAttributes(op->getAttributes());
                */
                break;
            }
            case Intrinsic::stacksave:
            case Intrinsic::stackrestore:
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_label:
            case Intrinsic::dbg_addr:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
        case Intrinsic::fabs:
        case Intrinsic::log:
        case Intrinsic::log2:
        case Intrinsic::log10:
        case Intrinsic::exp:
        case Intrinsic::exp2:
        case Intrinsic::pow:
        case Intrinsic::sin:
        case Intrinsic::cos:
                break;
            default:
              assert(inst);
              llvm::errs() << "cannot handle unknown intrinsic\n" << *inst;
              report_fatal_error("unknown intrinsic");
          }

        } else if(auto op = dyn_cast_or_null<CallInst>(inst)) {

            Function *called = op->getCalledFunction();
            
            if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
                if (castinst->isCast())
                if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                    if (fn->getName() == "malloc" || fn->getName() == "free" || fn->getName() == "_Znwm" || fn->getName() == "_ZdlPv" || fn->getName() == "_ZdlPvm") {
                        called = fn;
                    }
                }
            }

            if(called) {
                if (called->getName() == "printf" || called->getName() == "puts") {
                } else if(called->getName()=="malloc") {
                } else if(called->getName()=="free") {
                } else if(called->getName()=="_Znwm") {
                } else if(called->getName()=="_ZdlPv") {
                } else if(called->getName()=="_ZdlPvm") {
                } else if (!op->getCalledFunction()->empty()) {
                    if (gutils->isConstantInstruction(op))
                        continue;
                  SmallSet<unsigned,4> subconstant_args;

                  SmallVector<Value*, 8> args;
                  SmallVector<DIFFE_TYPE, 8> argsInverted;
                  bool modifyPrimal = !called->hasFnAttribute(Attribute::ReadNone);
                  IRBuilder<> BuilderZ(op);
                  BuilderZ.setFastMathFlags(FastMathFlags::getFast());

                  if ( (called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) && !gutils->isConstantValue(op) ) {
                     modifyPrimal = true;
                     //llvm::errs() << "primal modified " << called->getName() << " modified via return" << "\n";
                  }
                  for(unsigned i=0;i<op->getNumArgOperands(); i++) {
                    args.push_back(op->getArgOperand(i));

                    if (gutils->isConstantValue(op->getArgOperand(i))) {
                        subconstant_args.insert(i);
                        argsInverted.push_back(DIFFE_TYPE::CONSTANT);
                        continue;
                    }

                    auto argType = op->getArgOperand(i)->getType();

                    if (argType->isPointerTy() || argType->isIntegerTy()) {
                        argsInverted.push_back(DIFFE_TYPE::DUP_ARG);
                        args.push_back(gutils->invertPointerM(op->getArgOperand(i), BuilderZ));

                        if (! ( called->hasParamAttribute(i, Attribute::ReadOnly) || called->hasParamAttribute(i, Attribute::ReadNone)) ) {
                            modifyPrimal = true;
                            //llvm::errs() << "primal modified " << called->getName() << " modified via arg " << i << "\n";
                        }
                        //Note sometimes whattype mistakenly says something should be constant [because composed of integer pointers alone]
                        assert(whatType(argType) == DIFFE_TYPE::DUP_ARG || whatType(argType) == DIFFE_TYPE::CONSTANT);
                    } else {
                        argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
                        assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF || whatType(argType) == DIFFE_TYPE::CONSTANT);
                    }
                  }
                  if (subconstant_args.size() == args.size()) break;

                  //TODO create augmented primal
                  if (modifyPrimal) {
                    auto newcalled = CreateAugmentedPrimal(dyn_cast<Function>(called), subconstant_args, TLI, nullptr, /*differentialReturn*/!gutils->isConstantValue(op));
                    auto augmentcall = BuilderZ.CreateCall(newcalled, args);
                    augmentcall->setCallingConv(op->getCallingConv());
                    augmentcall->setDebugLoc(inst->getDebugLoc());
                    Instruction* antiptr = nullptr;
                    if (!called->getReturnType()->isVoidTy()) {
                      auto rv = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {1}));
                      gutils->originalInstructions.insert(rv);
                      gutils->nonconstant.insert(rv);
                      if (!gutils->isConstantValue(op))
                        gutils->nonconstant_values.insert(rv);
                      assert(op->getType() == rv->getType());
                      llvm::errs() << "augmented considering differential ip of " << called->getName() << " " << *called->getReturnType() << " " << gutils->isConstantValue(op) << "\n";
                      if ((called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) && !gutils->isConstantValue(op)) {
                        auto newip = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {2}));
                        auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
                        if (I != E && placeholder == &*I) I++;
                        gutils->invertedPointers.erase(op);
                        placeholder->replaceAllUsesWith(newip);
                        placeholder->eraseFromParent();
                        gutils->invertedPointers[rv] = newip;
                        antiptr = newip;
                        gutils->addMalloc(BuilderZ, antiptr);
                      }
                      op->replaceAllUsesWith(rv);
                    }
                    //todo consider how to propagate antiptr
                    Value* tp = BuilderZ.CreateExtractValue(augmentcall, {0});
                    if (tp->getType()->isEmptyTy()) {
                        auto tpt = tp->getType();
                        cast<Instruction>(tp)->eraseFromParent();
                        tp = UndefValue::get(tpt);
                    }
                    gutils->addMalloc(BuilderZ, tp);
                    op->eraseFromParent();
                  }
                } else {
                 if (gutils->isConstantInstruction(op))
                    continue;
                  assert(op);
                  llvm::errs() << "cannot handle non invertible function\n" << *op << "\n";
                  report_fatal_error("unknown noninvertible function");
                }
            } else {
                if (gutils->isConstantInstruction(op))
                    continue;
                assert(op);
                llvm::errs() << "cannot handle non const function in" << *op << "\n";
                report_fatal_error("unknown non constant function");
            }

        
        } else if(isa<LoadInst>(inst)) {
          if (gutils->isConstantInstruction(inst)) continue;

           //TODO IF OP IS POINTER
        } else if(auto op = dyn_cast<StoreInst>(inst)) {
          if (gutils->isConstantInstruction(inst)) continue;

          //TODO const
           //TODO IF OP IS POINTER
          if (!op->getValueOperand()->getType()->isPointerTy()) {
          } else {
            IRBuilder <> storeBuilder(op);
            llvm::errs() << "a op value: " << *op->getValueOperand() << "\n";
            Value* valueop = gutils->invertPointerM(op->getValueOperand(), storeBuilder);
            llvm::errs() << "a op pointer: " << *op->getPointerOperand() << "\n";
            Value* pointerop = gutils->invertPointerM(op->getPointerOperand(), storeBuilder);
            storeBuilder.CreateStore(valueop, pointerop);
            //llvm::errs() << "ignoring store bc pointer of " << *op << "\n";
          }
        }
     }
  }

  assert(gutils->addedFrees.size() == 0);

  auto nf = gutils->newFunc;
  
  ValueToValueMapTy invertedRetPs;
  if ((nf->getReturnType()->isPointerTy() || nf->getReturnType()->isIntegerTy()) && differentialReturn) {
    nf->dump();
    for (inst_iterator I = inst_begin(nf), E = inst_end(nf); I != E; ++I) {
      if (ReturnInst* ri = dyn_cast<ReturnInst>(&*I)) {
        IRBuilder <>builder(ri);
        ri->getReturnValue()->dump();
        invertedRetPs[ri] = gutils->invertPointerM(ri->getReturnValue(), builder);
        assert(invertedRetPs[ri]);
      }
    }
  }

  if (llvm::verifyFunction(*gutils->newFunc, &llvm::errs())) {
    gutils->newFunc->dump();
    report_fatal_error("function failed verification");
  }

  std::vector<Type*> RetTypes;

  std::vector<Type*> MallocTypes;

  for(auto a:gutils->getMallocs()) MallocTypes.push_back(a->getType());

  RetTypes.push_back(StructType::get(nf->getContext(), MallocTypes));
  
  if (!nf->getReturnType()->isVoidTy()) {
    RetTypes.push_back(nf->getReturnType());
    if (nf->getReturnType()->isPointerTy() || nf->getReturnType()->isIntegerTy())
      RetTypes.push_back(nf->getReturnType());
  }

  Type* RetType = StructType::get(nf->getContext(), RetTypes);
  
 ValueToValueMapTy VMap;
 std::vector<Type*> ArgTypes;
 for (const Argument &I : nf->args()) {
     ArgTypes.push_back(I.getType());
 }

 // Create a new function type...
 FunctionType *FTy = FunctionType::get(RetType,
                                   ArgTypes, nf->getFunctionType()->isVarArg());

 // Create the new function...
 Function *NewF = Function::Create(FTy, nf->getLinkage(), "augmented_"+todiff->getName(), nf->getParent());

 unsigned ii = 0, jj = 0;
 for (auto i=nf->arg_begin(), j=NewF->arg_begin(); i != nf->arg_end(); ) {
    VMap[i] = j;
    if (nf->hasParamAttribute(ii, Attribute::NoCapture)) {
      NewF->addParamAttr(jj, Attribute::NoCapture);
    }
    if (nf->hasParamAttribute(ii, Attribute::NoAlias)) {
       NewF->addParamAttr(jj, Attribute::NoAlias);
    }

     j->setName(i->getName());
     j++;
     jj++;
     
     i++;
     ii++;
 }

  SmallVector <ReturnInst*,4> Returns;
  CloneFunctionInto(NewF, nf, VMap, nf->getSubprogram() != nullptr, Returns, "",
                   nullptr);

  IRBuilder<> ib(NewF->getEntryBlock().getFirstNonPHI());
  auto ret = ib.CreateAlloca(RetType);
  
  unsigned i=0;
  for (auto v: gutils->getMallocs()) {
      if (!isa<UndefValue>(v)) {
          IRBuilder <>ib(cast<Instruction>(VMap[v])->getNextNode());
          Value *Idxs[] = {
            ib.getInt32(0),
            ib.getInt32(0),
            ib.getInt32(i)
          };
          auto gep = ib.CreateGEP(RetType, ret, Idxs, "");
          ib.CreateStore(VMap[v], gep);
      }
      i++;
  }

  for (inst_iterator I = inst_begin(nf), E = inst_end(nf); I != E; ++I) {
      if (ReturnInst* ri = dyn_cast<ReturnInst>(&*I)) {
          IRBuilder <>ib(cast<Instruction>(VMap[ri]));
          if (!nf->getReturnType()->isVoidTy()) {
            ib.CreateStore(cast<ReturnInst>(VMap[ri])->getReturnValue(), ib.CreateConstGEP2_32(RetType, ret, 0, 1, ""));
            
            if ((nf->getReturnType()->isPointerTy() || nf->getReturnType()->isIntegerTy()) && differentialReturn) {
              assert(invertedRetPs[ri]);
              assert(VMap[invertedRetPs[ri]]);
              ib.CreateStore( VMap[invertedRetPs[ri]], ib.CreateConstGEP2_32(RetType, ret, 0, 2, ""));
            }
            i++;
          }
          ib.CreateRet(ib.CreateLoad(ret));
          cast<Instruction>(VMap[ri])->eraseFromParent();
      }
  }

  for (Argument &Arg : NewF->args()) {
      if (Arg.hasAttribute(Attribute::Returned))
          Arg.removeAttr(Attribute::Returned);
      if (Arg.hasAttribute(Attribute::StructRet))
          Arg.removeAttr(Attribute::StructRet);
  }
  
  if (auto bytes = NewF->getDereferenceableBytes(llvm::AttributeList::ReturnIndex)) {
    AttrBuilder ab;
    ab.addDereferenceableAttr(bytes);
    NewF->removeAttributes(llvm::AttributeList::ReturnIndex, ab);
  }
  if (NewF->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias)) {
    NewF->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias);
  }

  if (llvm::verifyFunction(*NewF, &llvm::errs())) {
    NewF->dump();
    report_fatal_error("augmented function failed verification");
  }

  gutils->newFunc->eraseFromParent();
  
  if (oututils)
      *oututils = gutils;
  else
      delete gutils;
  return NewF;
}
  
void createInvertedTerminator(DiffeGradientUtils* gutils, BasicBlock *BB, AllocaInst* retAlloca, unsigned extraArgs) { 
    LoopContext loopContext;
    bool inLoop = gutils->getContext(BB, loopContext);
    BasicBlock* BB2 = gutils->reverseBlocks[BB];
    assert(BB2);
    IRBuilder<> Builder(BB2);
    Builder.setFastMathFlags(FastMathFlags::getFast());

      SmallVector<BasicBlock*,4> preds;
      for(auto B : predecessors(BB)) {
        preds.push_back(B);
      }

      if (preds.size() == 0) {
        SmallVector<Value *,4> retargs;

        if (retAlloca) {
          retargs.push_back(Builder.CreateLoad(retAlloca));
          assert(retargs[0]);
        }

        auto endidx = gutils->newFunc->arg_end();
        for(unsigned i=0; i<extraArgs; i++)
            endidx--;

        for (auto& I: gutils->newFunc->args()) {
          if (&I == endidx) {
              break;
          }
          if (!gutils->isConstantValue(&I) && whatType(I.getType()) == DIFFE_TYPE::OUT_DIFF ) {
            retargs.push_back(gutils->diffe((Value*)&I, Builder));
          }
        }

        Value* toret = UndefValue::get(gutils->newFunc->getReturnType());
        for(unsigned i=0; i<retargs.size(); i++) {
          unsigned idx[] = { i };
          toret = Builder.CreateInsertValue(toret, retargs[i], idx);
        }
        Builder.SetInsertPoint(Builder.GetInsertBlock());
        Builder.CreateRet(toret);
      } else if (preds.size() == 1) {
        for (auto I = BB->begin(), E = BB->end(); I != E; I++) {
            if(auto PN = dyn_cast<PHINode>(&*I)) {
                if (gutils->isConstantValue(PN)) continue;
                //TODO consider whether indeed we can skip differential phi pointers
                if (PN->getType()->isPointerTy()) continue;
                auto prediff = gutils->diffe(PN, Builder);
                gutils->setDiffe(PN, Constant::getNullValue(PN->getType()), Builder);
                if (!gutils->isConstantValue(PN->getIncomingValueForBlock(preds[0]))) {
                    gutils->addToDiffe(PN->getIncomingValueForBlock(preds[0]), prediff, Builder);
                }
            } else break;
        }

        Builder.SetInsertPoint(Builder.GetInsertBlock());
        Builder.CreateBr(gutils->reverseBlocks[preds[0]]);

      } else if (preds.size() == 2) {
        IRBuilder <> pbuilder(&BB->front());
        pbuilder.setFastMathFlags(FastMathFlags::getFast());
        Value* phi = nullptr;

        if (inLoop && BB2 == gutils->reverseBlocks[loopContext.var->getParent()]) {
          assert( ((preds[0] == loopContext.latch) && (preds[1] == loopContext.preheader)) || ((preds[1] == loopContext.latch) && (preds[0] == loopContext.preheader)) );
          if (preds[0] == loopContext.latch)
            phi = Builder.CreateICmpNE(loopContext.antivar, Constant::getNullValue(loopContext.antivar->getType()));
          else if(preds[1] == loopContext.latch)
            phi = Builder.CreateICmpEQ(loopContext.antivar, Constant::getNullValue(loopContext.antivar->getType()));
          else {
            llvm::errs() << "weird behavior for loopContext\n";
            assert(0 && "illegal loopcontext behavior");
          }
        } else {
          std::map<BasicBlock*,std::set<unsigned>> seen;
          std::map<BasicBlock*,std::set<BasicBlock*>> done;
          std::deque<std::tuple<BasicBlock*,unsigned,BasicBlock*>> Q; // newblock, prednum, pred
          Q.push_back(std::tuple<BasicBlock*,unsigned,BasicBlock*>(preds[0], 0, BB));
          Q.push_back(std::tuple<BasicBlock*,unsigned,BasicBlock*>(preds[1], 1, BB));
          //done.insert(BB);

          while(Q.size()) {
                auto trace = Q.front();
                auto block = std::get<0>(trace);
                auto num = std::get<1>(trace);
                auto predblock = std::get<2>(trace);
                Q.pop_front();

                if (seen[block].count(num) && done[block].count(predblock)) {
                  continue;
                }

                seen[block].insert(num);
                done[block].insert(predblock);

                if (seen[block].size() == 1) {
                  for (BasicBlock *Pred : predecessors(block)) {
                    Q.push_back(std::tuple<BasicBlock*,unsigned,BasicBlock*>(Pred, (*seen[block].begin()), block ));
                  }
                }

                SmallVector<BasicBlock*,4> succs;
                bool allDone = true;
                for (BasicBlock *Succ : successors(block)) {
                    succs.push_back(Succ);
                    if (done[block].count(Succ) == 0) {
                      allDone = false;
                    }
                }

                if (!allDone) {
                  continue;
                }

                if (seen[block].size() == preds.size() && succs.size() == preds.size()) {
                  //TODO below doesnt generalize past 2
                  bool hasSingle = false;
                  for(auto a : succs) {
                    if (seen[a].size() == 1) {
                      hasSingle = true;
                    }
                  }
                  if (!hasSingle)
                      goto continueloop;
                  if (auto branch = dyn_cast<BranchInst>(block->getTerminator())) {
                    assert(branch->getCondition());
                    phi = gutils->lookupM(branch->getCondition(), Builder);
                    for(unsigned i=0; i<preds.size(); i++) {
                        auto s = branch->getSuccessor(i);
                        assert(s == succs[i]);
                        if (seen[s].size() == 1) {
                            if ( (*seen[s].begin()) != i) {
                                phi = Builder.CreateNot(phi);
                                break;
                            } else {
                                break;
                            }
                        }
                    }
                    goto endPHI;
                  }

                  break;
                }
                continueloop:;
          }

          phi = pbuilder.CreatePHI(Type::getInt1Ty(Builder.getContext()), 2);
          cast<PHINode>(phi)->addIncoming(ConstantInt::getTrue(phi->getType()), preds[0]);
          cast<PHINode>(phi)->addIncoming(ConstantInt::getFalse(phi->getType()), preds[1]);
          phi = gutils->lookupM(phi, Builder);
          goto endPHI;

          endPHI:;
        }

        for (auto I = BB->begin(), E = BB->end(); I != E; I++) {
            if(auto PN = dyn_cast<PHINode>(&*I)) {

                // POINTER TYPE THINGS
                if (PN->getType()->isPointerTy()) continue;
                if (gutils->isConstantValue(PN)) continue; 
                auto prediff = gutils->diffe(PN, Builder);
                gutils->setDiffe(PN, Constant::getNullValue(PN->getType()), Builder);
                if (!gutils->isConstantValue(PN->getIncomingValueForBlock(preds[0]))) {
                    auto dif = Builder.CreateSelect(phi, prediff, Constant::getNullValue(prediff->getType()));
                    gutils->addToDiffe(PN->getIncomingValueForBlock(preds[0]), dif, Builder);
                }
                if (!gutils->isConstantValue(PN->getIncomingValueForBlock(preds[1]))) {
                    auto dif = Builder.CreateSelect(phi, Constant::getNullValue(prediff->getType()), prediff);
                    gutils->addToDiffe(PN->getIncomingValueForBlock(preds[1]), dif, Builder);
                }
            } else break;
        }
        BasicBlock* f0 = cast<BasicBlock>(gutils->reverseBlocks[preds[0]]);
        BasicBlock* f1 = cast<BasicBlock>(gutils->reverseBlocks[preds[1]]);
        while (auto bo = dyn_cast<BinaryOperator>(phi)) {
            if (bo->getOpcode() == BinaryOperator::Xor) {
                if (auto ci = dyn_cast<ConstantInt>(bo->getOperand(1))) {
                    if (ci->isOne()) {
                        phi = bo->getOperand(0);
                        auto ft = f0;
                        f0 = f1;
                        f1 = ft;
                        continue;
                    }
                }

                if (auto ci = dyn_cast<ConstantInt>(bo->getOperand(0))) {
                    if (ci->isOne()) {
                        phi = bo->getOperand(1);
                        auto ft = f0;
                        f0 = f1;
                        f1 = ft;
                        continue;
                    }
                }
                break;
            } else break;
        }
        Builder.SetInsertPoint(Builder.GetInsertBlock());
        Builder.CreateCondBr(phi, f0, f1);
      } else {
        IRBuilder <> pbuilder(&BB->front());
        pbuilder.setFastMathFlags(FastMathFlags::getFast());
        Value* phi = nullptr;

        if (true) {
          phi = pbuilder.CreatePHI(Type::getInt8Ty(Builder.getContext()), preds.size());
          for(unsigned i=0; i<preds.size(); i++) {
            cast<PHINode>(phi)->addIncoming(ConstantInt::get(phi->getType(), i), preds[i]);
          }
          phi = gutils->lookupM(phi, Builder);
        }

        for (auto I = BB->begin(), E = BB->end(); I != E; I++) {
            if(auto PN = dyn_cast<PHINode>(&*I)) {
              if (gutils->isConstantValue(PN)) continue;

              // POINTER TYPE THINGS
              if (PN->getType()->isPointerTy()) continue;
              auto prediff = gutils->diffe(PN, Builder);
              gutils->setDiffe(PN, Constant::getNullValue(PN->getType()), Builder);
              for(unsigned i=0; i<preds.size(); i++) {
                if (!gutils->isConstantValue(PN->getIncomingValueForBlock(preds[i]))) {
                    auto cond = Builder.CreateICmpEQ(phi, ConstantInt::get(phi->getType(), i));
                    auto dif = Builder.CreateSelect(cond, prediff, Constant::getNullValue(prediff->getType()));
                    gutils->addToDiffe(PN->getIncomingValueForBlock(preds[i]), dif, Builder);
                }
              }
            } else break;
        }

        Builder.SetInsertPoint(Builder.GetInsertBlock());
        auto swit = Builder.CreateSwitch(phi, gutils->reverseBlocks[preds.back()], preds.size()-1);
        for(unsigned i=0; i<preds.size()-1; i++) {
          swit->addCase(ConstantInt::get(cast<IntegerType>(phi->getType()), i), gutils->reverseBlocks[preds[i]]);
        }
      }
}

Function* CreatePrimalAndGradient(Function* todiff, const SmallSet<unsigned,4>& constant_args, TargetLibraryInfo &TLI, AAResults &AA, bool returnValue, bool differentialReturn, bool topLevel, GradientUtils** oututils, llvm::Type* additionalArg) {
  static std::map<std::tuple<Function*,std::set<unsigned>, bool/*retval*/, bool/*differentialReturn*/, bool/*topLevel*/, llvm::Type*>, Function*> cachedfunctions;
  auto tup = std::make_tuple(todiff, std::set<unsigned>(constant_args.begin(), constant_args.end()), returnValue, differentialReturn, topLevel, additionalArg);
  if (cachedfunctions.find(tup) != cachedfunctions.end()) {
    if (oututils) *oututils = nullptr;
    return cachedfunctions[tup];
  }

  assert(!todiff->empty());
  auto M = todiff->getParent();
  auto& Context = M->getContext();

  DiffeGradientUtils *gutils = DiffeGradientUtils::CreateFromClone(todiff, TLI, constant_args, returnValue ? ReturnType::ArgsWithReturn : ReturnType::Args, differentialReturn, additionalArg);
  cachedfunctions[tup] = gutils->newFunc;

  Argument* additionalValue = nullptr;
  if (additionalArg) {
    auto v = gutils->newFunc->arg_end();
    v--;
    additionalValue = v;
    gutils->setTape(additionalValue);
  }

  Argument* differetval = nullptr;
  if (differentialReturn) {
    auto endarg = gutils->newFunc->arg_end();
    endarg--;
    if (additionalArg) endarg--;
    differetval = endarg;
  }

  llvm::AllocaInst* retAlloca = nullptr;
  if (returnValue && differentialReturn) {
	retAlloca = IRBuilder<>(&gutils->newFunc->getEntryBlock().front()).CreateAlloca(todiff->getReturnType(), nullptr, "toreturn");
  }
  

  // Force loop canonicalization everywhere
  for(BasicBlock* BB: gutils->originalBlocks) {
    LoopContext loopContext;
    gutils->getContext(BB, loopContext);
  }

  //if (topLevel)
    gutils->forceAugmentedReturns();

  for(BasicBlock* BB: gutils->originalBlocks) {

    LoopContext loopContext;
    bool inLoop = gutils->getContext(BB, loopContext);

    auto BB2 = gutils->reverseBlocks[BB];
    assert(BB2);

    IRBuilder<> Builder2(BB2);
    if (BB2->size() > 0) {
        Builder2.SetInsertPoint(BB2->getFirstNonPHI());
    }
    Builder2.setFastMathFlags(FastMathFlags::getFast());

    std::map<Value*,Value*> alreadyLoaded;

    std::function<Value*(Value*)> lookup = [&](Value* val) -> Value* {
      if (alreadyLoaded.find(val) != alreadyLoaded.end()) {
        return alreadyLoaded[val];
      }
      return alreadyLoaded[val] = gutils->lookupM(val, Builder2);
    };

    auto diffe = [&Builder2,&gutils](Value* val) -> Value* {
        return gutils->diffe(val, Builder2);
    };

    auto addToDiffe = [&Builder2,&gutils](Value* val, Value* dif) -> void {
      gutils->addToDiffe(val, dif, Builder2);
    };

    auto setDiffe = [&](Value* val, Value* toset) -> void {
      if (gutils->isConstantValue(val)) {
        gutils->newFunc->dump();
        val->dump();
      }
      gutils->setDiffe(val, toset, Builder2);
    };

    auto addToDiffeIndexed = [&](Value* val, Value* dif, ArrayRef<Value*> idxs) -> void{
      gutils->addToDiffeIndexed(val, dif, idxs, Builder2);
    };

    auto invertPointer = [&](Value* val) -> Value* {
        return gutils->invertPointerM(val, Builder2);
    };

    auto addToPtrDiffe = [&](Value* val, Value* dif) {
      gutils->addToPtrDiffe(val, dif, Builder2);
    };
    
    auto setPtrDiffe = [&](Value* val, Value* dif) {
      gutils->setPtrDiffe(val, dif, Builder2);
    };

  auto term = BB->getTerminator();
  assert(term);
  bool unreachableTerminator = false;
  if(auto op = dyn_cast<ReturnInst>(term)) {
      auto retval = op->getReturnValue();
      IRBuilder<> rb(op);
      rb.setFastMathFlags(FastMathFlags::getFast());
	  if (retAlloca)
		rb.CreateStore(retval, retAlloca);
	 
      rb.CreateBr(BB2);

      op->eraseFromParent();

      if (differentialReturn && !gutils->isConstantValue(retval)) {
        setDiffe(retval, differetval);
      } else {
		assert (retAlloca == nullptr);
      }
  } else if (isa<BranchInst>(term) || isa<SwitchInst>(term)) {

  } else if (isa<UnreachableInst>(term)) {
    unreachableTerminator = true;
    continue;
  } else {
    assert(BB);
    assert(BB->getParent());
    assert(term);
    llvm::errs() << *BB->getParent() << "\n";
    llvm::errs() << "unknown terminator instance " << *term << "\n";
    assert(0 && "unknown terminator inst");
  }

  if (inLoop && loopContext.latch==BB) {
    BB2->getInstList().push_front(loopContext.antivar);

    IRBuilder<> tbuild(gutils->reverseBlocks[loopContext.exit]);
    tbuild.setFastMathFlags(FastMathFlags::getFast());

    // ensure we are before the terminator if it exists
    if (gutils->reverseBlocks[loopContext.exit]->size()) {
      tbuild.SetInsertPoint(&gutils->reverseBlocks[loopContext.exit]->back());
    }

    loopContext.antivar->addIncoming(gutils->lookupM(loopContext.limit, tbuild), gutils->reverseBlocks[loopContext.exit]);
    auto sub = Builder2.CreateSub(loopContext.antivar, ConstantInt::get(loopContext.antivar->getType(), 1));
    for(BasicBlock* in: successors(loopContext.latch) ) {
        if (gutils->LI.getLoopFor(in) == gutils->LI.getLoopFor(BB)) {
            loopContext.antivar->addIncoming(sub, gutils->reverseBlocks[in]);
        }
    }
  }

  if (!unreachableTerminator)
  for (auto I = BB->rbegin(), E = BB->rend(); I != E;) {
    Instruction* inst = &*I;
    assert(inst);
    I++;
    if (gutils->originalInstructions.find(inst) == gutils->originalInstructions.end()) continue;

    if (auto op = dyn_cast<BinaryOperator>(inst)) {
      if (gutils->isConstantInstruction(inst)) continue;
      Value* dif0 = nullptr;
      Value* dif1 = nullptr;
      switch(op->getOpcode()) {
        case Instruction::FMul:
          if (!gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFMul(diffe(inst), lookup(op->getOperand(1)), "diffe"+op->getOperand(0)->getName());
          if (!gutils->isConstantValue(op->getOperand(1)))
            dif1 = Builder2.CreateFMul(diffe(inst), lookup(op->getOperand(0)), "diffe"+op->getOperand(1)->getName());
          break;
        case Instruction::FAdd:{
          auto idiff = diffe(inst);
          if (!gutils->isConstantValue(op->getOperand(0)))
            dif0 = idiff;
          if (!gutils->isConstantValue(op->getOperand(1)))
            dif1 = idiff;
          break;
        }
        case Instruction::FSub:
          if (!gutils->isConstantValue(op->getOperand(0)))
            dif0 = diffe(inst);
          if (!gutils->isConstantValue(op->getOperand(1)))
            dif1 = Builder2.CreateFNeg(diffe(inst));
          break;
        case Instruction::FDiv:
          if (!gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFDiv(diffe(inst), lookup(op->getOperand(1)), "diffe"+op->getOperand(0)->getName());
          if (!gutils->isConstantValue(op->getOperand(1)))
            dif1 = Builder2.CreateFNeg(
              Builder2.CreateFDiv(
                Builder2.CreateFMul(diffe(inst), lookup(op)),
                lookup(op->getOperand(1)))
            );
          break;

        default:
          assert(op);
          llvm::errs() << *gutils->newFunc << "\n";
          llvm::errs() << "cannot handle unknown binary operator: " << *op << "\n";
          report_fatal_error("unknown binary operator");
      }

      setDiffe(inst, Constant::getNullValue(inst->getType()));
      if (dif0) addToDiffe(op->getOperand(0), dif0);
      if (dif1) addToDiffe(op->getOperand(1), dif1);
    } else if(auto op = dyn_cast_or_null<IntrinsicInst>(inst)) {
      Value* dif0 = nullptr;
      Value* dif1 = nullptr;
      switch(op->getIntrinsicID()) {
        case Intrinsic::memcpy: {
            if (gutils->isConstantInstruction(inst)) continue;
            SmallVector<Value*, 4> args;
            args.push_back(invertPointer(op->getOperand(0)));
            args.push_back(invertPointer(op->getOperand(1)));
            args.push_back(lookup(op->getOperand(2)));
            args.push_back(lookup(op->getOperand(3)));

            Type *tys[] = {args[0]->getType(), args[1]->getType(), args[2]->getType()};
            auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memcpy, tys), args);
            cal->setAttributes(op->getAttributes());

            break;
        }
        case Intrinsic::memset: {
            if (gutils->isConstantInstruction(inst)) continue;
            if (!gutils->isConstantValue(op->getOperand(1))) {
                assert(inst);
                llvm::errs() << "couldn't handle non constant inst in memset to propagate differential to\n" << *inst;
                report_fatal_error("non constant in memset");
            }
            auto ptx = invertPointer(op->getOperand(0));
            SmallVector<Value*, 4> args;
            args.push_back(ptx);
            args.push_back(lookup(op->getOperand(1)));
            args.push_back(lookup(op->getOperand(2)));
            args.push_back(lookup(op->getOperand(3)));

            Type *tys[] = {args[0]->getType(), args[2]->getType()};
            auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::memset, tys), args);
            cal->setAttributes(op->getAttributes());
            
            break;
        }
        case Intrinsic::stacksave:
        case Intrinsic::stackrestore:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::dbg_label:
        case Intrinsic::dbg_addr:
            break;
        case Intrinsic::lifetime_start:{
            if (gutils->isConstantInstruction(inst)) continue;
            SmallVector<Value*, 2> args = {lookup(op->getOperand(0)), lookup(op->getOperand(1))};
            Type *tys[] = {args[1]->getType()};
            auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::lifetime_end, tys), args);
            cal->setAttributes(op->getAttributes());
            break;
        }
        case Intrinsic::lifetime_end:
            op->eraseFromParent();
            break;
        case Intrinsic::sqrt: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateBinOp(Instruction::FDiv, diffe(inst),
              Builder2.CreateFMul(ConstantFP::get(op->getType(), 2.0), lookup(op))
            );
          break;
        }
        case Intrinsic::fabs: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0))) {
            auto cmp = Builder2.CreateFCmpOLT(lookup(op->getOperand(0)), ConstantFP::get(op->getOperand(0)->getType(), 0));
            dif0 = Builder2.CreateSelect(cmp, ConstantFP::get(op->getOperand(0)->getType(), -1), ConstantFP::get(op->getOperand(0)->getType(), 1));
          }
          break;
        }
        case Intrinsic::log: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFDiv(diffe(inst), lookup(op->getOperand(0)));
          break;
        }
        case Intrinsic::log2: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFDiv(diffe(inst),
              Builder2.CreateFMul(ConstantFP::get(op->getType(), 0.6931471805599453), lookup(op->getOperand(0)))
            );
          break;
        }
        case Intrinsic::log10: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFDiv(diffe(inst),
              Builder2.CreateFMul(ConstantFP::get(op->getType(), 2.302585092994046), lookup(op->getOperand(0)))
            );
          break;
        }
        case Intrinsic::exp: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFMul(diffe(inst), lookup(op));
          break;
        }
        case Intrinsic::exp2: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0)))
            dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(diffe(inst), lookup(op)), ConstantFP::get(op->getType(), 0.6931471805599453)
            );
          break;
        }
        case Intrinsic::pow: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0))) {

            /*
            dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(diffe(inst),
                Builder2.CreateFDiv(lookup(op), lookup(op->getOperand(0)))), lookup(op->getOperand(1))
            );
            */
            SmallVector<Value*, 2> args = {lookup(op->getOperand(0)), Builder2.CreateFSub(lookup(op->getOperand(1)), ConstantFP::get(op->getType(), 1.0))};
            Type *tys[] = {args[1]->getType()};
            auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::pow, tys), args);
            cal->setAttributes(op->getAttributes());
            dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(diffe(inst), cal)
              , lookup(op->getOperand(1))
            );
          }

          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(1))) {
            Value *args[] = {lookup(op->getOperand(1))};
            Type *tys[] = {op->getOperand(1)->getType()};

            dif1 = Builder2.CreateFMul(
              Builder2.CreateFMul(diffe(inst), lookup(op)),
              Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::log, tys), args)
            );
          }

          break;
        }
        case Intrinsic::sin: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0))) {
            Value *args[] = {lookup(op->getOperand(0))};
            Type *tys[] = {op->getOperand(0)->getType()};
            dif0 = Builder2.CreateFMul(diffe(inst),
              Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::cos, tys), args) );
          }
          break;
        }
        case Intrinsic::cos: {
          if (!gutils->isConstantInstruction(op) && !gutils->isConstantValue(op->getOperand(0))) {
            Value *args[] = {lookup(op->getOperand(0))};
            Type *tys[] = {op->getOperand(0)->getType()};
            dif0 = Builder2.CreateFMul(diffe(inst),
              Builder2.CreateFNeg(
                Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::sin, tys), args) )
            );
          }
          break;
        }
        default:
          assert(inst);
          llvm::errs() << "cannot handle unknown intrinsic\n" << *inst;
          report_fatal_error("unknown intrinsic");
      }

      if (dif0 || dif1) setDiffe(inst, Constant::getNullValue(inst->getType()));
      if (dif0) addToDiffe(op->getOperand(0), dif0);
      if (dif1) addToDiffe(op->getOperand(1), dif1);
    } else if(auto op = dyn_cast_or_null<CallInst>(inst)) {

        Function *called = op->getCalledFunction();
        
        if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
            if (castinst->isCast())
            if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                if (fn->getName() == "malloc" || fn->getName() == "free" || fn->getName() == "_Znwm" || fn->getName() == "_ZdlPv" || fn->getName() == "_ZdlPvm") {
                    called = fn;
                }
            }
        }

        if(called) {
            if (called->getName() == "printf" || called->getName() == "puts") {
                SmallVector<Value*, 4> args;
                for(unsigned i=0; i<op->getNumArgOperands(); i++) {
                    args.push_back(lookup(op->getArgOperand(i)));
                }
                auto cal = Builder2.CreateCall(called, args);
                cal->setAttributes(op->getAttributes());
            } else if(called->getName()=="malloc") {
              if (true) {
                 auto ci = CallInst::CreateFree(Builder2.CreatePointerCast(lookup(inst), Type::getInt8PtrTy(Context)), Builder2.GetInsertBlock());
                 if (ci->getParent()==nullptr) {
                   Builder2.Insert(ci);
                 }
              }

            } else if(called->getName()=="_Znwm") {
              if (true) {
                  Type *VoidTy = Type::getVoidTy(M->getContext());
                  Type *IntPtrTy = Type::getInt8PtrTy(M->getContext());
                  //TODO _ZdlPv or _ZdlPvm
                  auto FreeFunc = M->getOrInsertFunction("_ZdlPv", VoidTy, IntPtrTy);
                  auto ci = CallInst::Create(FreeFunc, {Builder2.CreatePointerCast(lookup(inst), Type::getInt8PtrTy(Context))}, "", Builder2.GetInsertBlock());
                  cast<CallInst>(ci)->setTailCall();
                  if (Function *F = dyn_cast<Function>(FreeFunc))
                    cast<CallInst>(ci)->setCallingConv(F->getCallingConv());

                  if (ci->getParent()==nullptr) {
                    Builder2.Insert(ci);
                  }
              }

            } else if(called->getName()=="free") {
                llvm::Value* val = op->getArgOperand(0);
                while(auto cast = dyn_cast<CastInst>(val)) val = cast->getOperand(0);
                if (auto dc = dyn_cast<CallInst>(val)) {
                    if (dc->getCalledFunction()->getName() == "malloc") {
                        op->eraseFromParent();
                        continue;
                    }
                }
                if (isa<ConstantPointerNull>(val)) {
                    op->eraseFromParent();
                    llvm::errs() << "removing free of null pointer\n";
                    continue;
                }
                llvm::errs() << "freeing without malloc " << *val << "\n";
                op->eraseFromParent();
                continue;
                //assert(0 && "free not freeing a malloc");
                //TODO HANDLE FREE
                //
            } else if(called->getName()=="_ZdlPv" || called->getName()=="_ZdlPvm") {
                llvm::Value* val = op->getArgOperand(0);
                while(auto cast = dyn_cast<CastInst>(val)) val = cast->getOperand(0);
                if (auto dc = dyn_cast<CallInst>(val)) {
                    if (dc->getCalledFunction()->getName() == "_Znwm") {
                        op->eraseFromParent();
                        continue;
                    }
                }
                llvm::errs() << "deleting without new " << *val << "\n";
                op->eraseFromParent();
                continue;
                //assert(0 && "free not freeing a malloc");
                //TODO HANDLE FREE/DELETE
                //

            } else if (!op->getCalledFunction()->empty()) {
              if (gutils->isConstantInstruction(op)) {
			    continue;
              }
              SmallSet<unsigned,4> subconstant_args;

              SmallVector<Value*, 8> args;
              SmallVector<Value*, 8> pre_args;
              SmallVector<DIFFE_TYPE, 8> argsInverted;
              bool modifyPrimal = !called->hasFnAttribute(Attribute::ReadNone);
              IRBuilder<> BuilderZ(op);
              std::vector<Instruction*> postCreate;
              BuilderZ.setFastMathFlags(FastMathFlags::getFast());

              if ( (called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) && !gutils->isConstantValue(op)) {
                  //llvm::errs() << "augmented modified " << called->getName() << " modified via return" << "\n";
                  modifyPrimal = true;
              }
              for(unsigned i=0;i<op->getNumArgOperands(); i++) {
                args.push_back(lookup(op->getArgOperand(i)));
                pre_args.push_back(op->getArgOperand(i));

                if (gutils->isConstantValue(op->getArgOperand(i))) {
                    subconstant_args.insert(i);
                    argsInverted.push_back(DIFFE_TYPE::CONSTANT);
                    continue;
                }

				auto argType = op->getArgOperand(i)->getType();

				if ( (argType->isPointerTy() || argType->isIntegerTy()) && !gutils->isConstantValue(op->getArgOperand(i)) ) {
					argsInverted.push_back(DIFFE_TYPE::DUP_ARG);
					args.push_back(invertPointer(op->getArgOperand(i)));
					pre_args.push_back(gutils->invertPointerM(op->getArgOperand(i), BuilderZ));

                    //TODO this check should consider whether this pointer has allocation/etc modifications and so on
                    if (! ( called->hasParamAttribute(i, Attribute::ReadOnly) || called->hasParamAttribute(i, Attribute::ReadNone)) ) {
                        //llvm::errs() << "augmented modified " << called->getName() << " modified via arg " << i << "\n";
					    modifyPrimal = true;
                    }

					//Note sometimes whattype mistakenly says something should be constant [because composed of integer pointers alone]
					assert(whatType(argType) == DIFFE_TYPE::DUP_ARG || whatType(argType) == DIFFE_TYPE::CONSTANT);
				} else {
					argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
					assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF || whatType(argType) == DIFFE_TYPE::CONSTANT);
				}
              }
              if (subconstant_args.size() == args.size()) break;

			  bool retUsed = false;
              for (const User *U : inst->users()) {
                if (auto si = dyn_cast<StoreInst>(U)) {
					if (si->getPointerOperand() == retAlloca && si->getValueOperand() == inst) {
						retUsed = true;
						continue;
					}
				}
				retUsed = false;
				break;
              }

              bool replaceFunction = false;

              if (topLevel && BB->getSingleSuccessor() == BB2) {
                  auto origop = cast<CallInst>(gutils->getOriginal(op));
                  auto OBB = cast<BasicBlock>(gutils->getOriginal(BB));
                  auto iter = OBB->rbegin();
                  while(iter != OBB->rend() && &*iter != origop) {
                    if (auto call = dyn_cast<CallInst>(&*iter)) {
                        if (isCertainMallocOrFree(call->getCalledFunction())) {
                            iter++;
                            continue;
                        }
                    }
                    
                    if (isa<ReturnInst>(&*iter)) {
                        iter++;
                        continue;
                    }
                        
                    bool usesInst = false;
                    for(auto &operand : iter->operands()) {
                        if (operand.get() == gutils->getOriginal(op)) { usesInst = true; break; }
                    }
                    if (usesInst) {
                        break;
                    }

                    if (!iter->mayReadOrWriteMemory() || isa<BinaryOperator>(&*iter)) {
                        iter++;
                        continue;
                    }

                    if (AA.getModRefInfo(&*iter, origop) == ModRefInfo::NoModRef) {
                        iter++;
                        continue;
                    }

                    //load that follows the original
                    if (auto li = dyn_cast<LoadInst>(&*iter)) {
                        bool modref = false;
                            for(Instruction* it = li; it != nullptr; it = it->getNextNode()) {
                                if (auto call = dyn_cast<CallInst>(it)) {
                                     if (isCertainMallocOrFree(call->getCalledFunction())) {
                                         continue;
                                     }
                                }
                                if (AA.canInstructionRangeModRef(*it, *it, MemoryLocation::get(li), ModRefInfo::Mod)) {
                                    modref = true;
                            llvm::errs() << " inst  found mod " << *iter << " " << *it << "\n";
                                }
                            }

                        if (modref)
                            break;
                        postCreate.push_back(cast<Instruction>(gutils->getNewFromOriginal(&*iter)));
                        iter++;
                        continue;
                    }
                    
                    break;
                  }
                  if (&*iter == gutils->getOriginal(op)) {
                      llvm::errs() << " choosing to replace function " << called->getName() << " and do both forward/reverse\n";
                      replaceFunction = true;
                      modifyPrimal = false;
                  } else {
                      llvm::errs() << " failed to replace function " << called->getName() << " due to " << *iter << "\n";
                  }
              }

              Value* tape = nullptr;
              GradientUtils *augmentedutils = nullptr;
              CallInst* augmentcall = nullptr;
              if (modifyPrimal) {
                if (topLevel) {
                  auto newcalled = CreateAugmentedPrimal(dyn_cast<Function>(called), subconstant_args, TLI, &augmentedutils, /*differentialReturns*/!gutils->isConstantValue(op));
                  augmentcall = BuilderZ.CreateCall(newcalled, pre_args);
                  augmentcall->setCallingConv(op->getCallingConv());
                  augmentcall->setDebugLoc(inst->getDebugLoc());
                  tape = BuilderZ.CreateExtractValue(augmentcall, {0});
                  if (tape->getType()->isEmptyTy()) {
                      auto tt = tape->getType();
                      cast<Instruction>(tape)->eraseFromParent();
                      tape = UndefValue::get(tt);
                  }

                  llvm::errs() << "primal considering differential ip of " << called->getName() << " " << *called->getReturnType() << " " << gutils->isConstantValue(op) << "\n";
                  if( (called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) && !gutils->isConstantValue(op) ) {
                    auto newip = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {2}));
                    auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
                    if (I != E && placeholder == &*I) I++;
                    placeholder->replaceAllUsesWith(newip);
                    placeholder->eraseFromParent();
                    gutils->invertedPointers[op] = newip;
                  }
                } else {
                  assert(additionalValue);
                  if( (called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) && !gutils->isConstantValue(op) ) {
                    IRBuilder<> bb(op);
                    auto newip = gutils->addMalloc(bb, nullptr);
                    auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
                    if (I != E && placeholder == &*I) I++;
                    placeholder->replaceAllUsesWith(newip);
                    placeholder->eraseFromParent();
                    gutils->invertedPointers[op] = newip;
                  }
                }
                IRBuilder<> builder(op);
                tape = gutils->addMalloc(builder, tape);

              }
              auto newcalled = CreatePrimalAndGradient(dyn_cast<Function>(called), subconstant_args, TLI, AA, retUsed, !gutils->isConstantValue(inst) && !inst->getType()->isPointerTy(), /*topLevel*/replaceFunction, nullptr, tape ? tape->getType() : nullptr);//, LI, DT);

              if (!gutils->isConstantValue(inst) && !inst->getType()->isPointerTy()) {
                args.push_back(diffe(inst));
              }

              if (tape) args.push_back(lookup(tape));

              auto diffes = Builder2.CreateCall(newcalled, args);
              diffes->setCallingConv(op->getCallingConv());
              diffes->setDebugLoc(inst->getDebugLoc());
              unsigned structidx = retUsed ? 1 : 0;

              for(unsigned i=0;i<op->getNumArgOperands(); i++) {
                if (argsInverted[i] == DIFFE_TYPE::OUT_DIFF) {
                  Value* diffeadd = Builder2.CreateExtractValue(diffes, {structidx});
                  structidx++;
                  addToDiffe(op->getArgOperand(i), diffeadd);
                }
              }

              if (retUsed) {
                auto retval = cast<Instruction>(Builder2.CreateExtractValue(diffes, {0}));
                gutils->originalInstructions.insert(retval);
                gutils->nonconstant.insert(retval);
                if (!gutils->isConstantValue(op))
                  gutils->nonconstant_values.insert(retval);
				Builder2.CreateStore(retval, retAlloca);

				startremove:
              	for (User *U : inst->users()) {
                	if (auto si = dyn_cast<StoreInst>(U)) {
						if (si->getPointerOperand() == retAlloca && si->getValueOperand() == inst) {
							si->eraseFromParent();
							goto startremove;
						}
					}
                }
              }

              if (replaceFunction) {
                ValueToValueMapTy mapp;
                if (op->getNumUses() != 0) {
                  auto retval = cast<Instruction>(Builder2.CreateExtractValue(diffes, {0}));
                  gutils->originalInstructions.insert(retval);
                  gutils->nonconstant.insert(retval);
                  if (!gutils->isConstantValue(op))
                    gutils->nonconstant_values.insert(retval);
                  op->replaceAllUsesWith(retval);
                  mapp[op] = retval;
                }
                for(auto a : postCreate) {
                    gutils->unwrapM(a, Builder2, mapp, true);
                }
                op->eraseFromParent();
              }

              //TODO this shouldn't matter because this can't use itself, but setting null should be done before other sets but after load of diffe
			  if (inst->getNumUses() != 0 && !gutils->isConstantValue(inst))
              	setDiffe(inst, Constant::getNullValue(inst->getType()));
              
              if (augmentcall) {

                if (!called->getReturnType()->isVoidTy()) {
                  auto dcall = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {1}));
                  gutils->originalInstructions.insert(dcall);
                  gutils->nonconstant.insert(dcall);
                  if (!gutils->isConstantValue(op))
                    gutils->nonconstant_values.insert(dcall);

                  llvm::errs() << "augmented considering differential ip of " << called->getName() << " " << *called->getReturnType() << " " << gutils->isConstantValue(op) << "\n";
                  if (!gutils->isConstantValue(op)) {
                      if (called->getReturnType()->isPointerTy() || called->getReturnType()->isIntegerTy()) {
                        gutils->invertedPointers[dcall] = gutils->invertedPointers[op];
                        gutils->invertedPointers.erase(op);
                      } else {
                        gutils->differentials[dcall] = gutils->differentials[op];
                        gutils->differentials.erase(op);
                      }
                  }
                  op->replaceAllUsesWith(dcall);
                }

                gutils->originalInstructions.insert(diffes);
                gutils->nonconstant.insert(diffes);
                if (!gutils->isConstantValue(op))
                  gutils->nonconstant_values.insert(diffes);
                op->eraseFromParent();
                gutils->replaceableCalls.insert(augmentcall);
              } else {
                gutils->replaceableCalls.insert(op);
              }
            } else {
              if (gutils->isConstantInstruction(op)) {
			    continue;
              }
              assert(op);
              llvm::errs() << "cannot handle non invertible function\n" << *op << "\n";
              report_fatal_error("unknown noninvertible function");
            }
        } else {
            if (gutils->isConstantInstruction(op)) {
			  continue;
            }
            assert(op);
            llvm::errs() << "cannot handle non const function in" << *op << "\n";
            report_fatal_error("unknown non constant function");
        }

    } else if(auto op = dyn_cast_or_null<SelectInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      Value* dif1 = nullptr;
      Value* dif2 = nullptr;

      if (!gutils->isConstantValue(op->getOperand(1)))
        dif1 = Builder2.CreateSelect(lookup(op->getOperand(0)), diffe(inst), Constant::getNullValue(op->getOperand(1)->getType()), "diffe"+op->getOperand(1)->getName());
      if (!gutils->isConstantValue(op->getOperand(2)))
        dif2 = Builder2.CreateSelect(lookup(op->getOperand(0)), Constant::getNullValue(op->getOperand(2)->getType()), diffe(inst), "diffe"+op->getOperand(2)->getName());

      setDiffe(inst, Constant::getNullValue(inst->getType()));
      if (dif1) addToDiffe(op->getOperand(1), dif1);
      if (dif2) addToDiffe(op->getOperand(2), dif2);
    } else if(auto op = dyn_cast<LoadInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

       //TODO IF OP IS POINTER
      if (!op->getType()->isPointerTy()) {
        auto prediff = diffe(inst);
        setDiffe(inst, Constant::getNullValue(inst->getType()));
        addToPtrDiffe(op->getOperand(0), prediff);
      } else {
        //Builder2.CreateStore(diffe(inst), invertPointer(op->getOperand(0)));//, op->getName()+"'psweird");
        //addToNPtrDiffe(op->getOperand(0), diffe(inst));
        //assert(0 && "cannot handle non const pointer load inversion");
        assert(op);
		llvm::errs() << "ignoring load bc pointer of " << *op << "\n";
      }
    } else if(auto op = dyn_cast<StoreInst>(inst)) {
      if (gutils->isConstantInstruction(inst)) continue;

      //TODO const
       //TODO IF OP IS POINTER
      if (!op->getValueOperand()->getType()->isPointerTy()) {
		  if (!gutils->isConstantValue(op->getValueOperand())) {
			auto dif1 = Builder2.CreateLoad(invertPointer(op->getPointerOperand()));
			addToDiffe(op->getValueOperand(), dif1);
            setPtrDiffe(op->getPointerOperand(), Constant::getNullValue(op->getValueOperand()->getType()));
		  }
	  } else if (topLevel) {
        IRBuilder <> storeBuilder(op);
        llvm::errs() << "op value: " << *op->getValueOperand() << "\n";
        Value* valueop = gutils->invertPointerM(op->getValueOperand(), storeBuilder);
        llvm::errs() << "op pointer: " << *op->getPointerOperand() << "\n";
        Value* pointerop = gutils->invertPointerM(op->getPointerOperand(), storeBuilder);
        storeBuilder.CreateStore(valueop, pointerop);
		//llvm::errs() << "ignoring store bc pointer of " << *op << "\n";
	  }
      //?necessary if pointer is readwrite
      /*
      IRBuilder<> BuilderZ(inst);
      Builder2.CreateStore(
        lookup(BuilderZ.CreateLoad(op->getPointerOperand())), lookup(op->getPointerOperand()));
      */
    } else if(auto op = dyn_cast<ExtractValueInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      if (op->getType()->isPointerTy()) continue;

      auto prediff = diffe(inst);
      //todo const
      if (!gutils->isConstantValue(op->getOperand(0))) {
		SmallVector<Value*,4> sv;
      	for(auto i : op->getIndices())
        	sv.push_back(ConstantInt::get(Type::getInt32Ty(Context), i));
        addToDiffeIndexed(op->getOperand(0), prediff, sv);
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<InsertValueInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      auto st = cast<StructType>(op->getType());
      bool hasNonPointer = false;
      for(unsigned i=0; i<st->getNumElements(); i++) {
        if (!st->getElementType(i)->isPointerTy()) {
           hasNonPointer = true; 
        }
      }
      if (!hasNonPointer) continue;

      if (!gutils->isConstantValue(op->getInsertedValueOperand()) && !op->getInsertedValueOperand()->getType()->isPointerTy()) {
        auto prediff = gutils->diffe(inst, Builder2);
        auto dindex = Builder2.CreateExtractValue(prediff, op->getIndices());
        gutils->addToDiffe(op->getOperand(1), dindex, Builder2);
      }
      
      if (!gutils->isConstantValue(op->getAggregateOperand()) && !op->getAggregateOperand()->getType()->isPointerTy()) {
        auto prediff = gutils->diffe(inst, Builder2);
        auto dindex = Builder2.CreateInsertValue(prediff, Constant::getNullValue(op->getInsertedValueOperand()->getType()), op->getIndices());
        gutils->addToDiffe(op->getAggregateOperand(), dindex, Builder2);
      }

      gutils->setDiffe(inst, Constant::getNullValue(inst->getType()), Builder2);
    } else if (auto op = dyn_cast<ShuffleVectorInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      auto loaded = diffe(inst);
      size_t l1 = cast<VectorType>(op->getOperand(0)->getType())->getNumElements();
      uint64_t instidx = 0;
      for( size_t idx : op->getShuffleMask()) {
        auto opnum = (idx < l1) ? 0 : 1;
        auto opidx = (idx < l1) ? idx : (idx-l1);
        SmallVector<Value*,4> sv;
        sv.push_back(ConstantInt::get(Type::getInt32Ty(Context), opidx));
		if (!gutils->isConstantValue(op->getOperand(opnum)))
          addToDiffeIndexed(op->getOperand(opnum), Builder2.CreateExtractElement(loaded, instidx), sv);
        instidx++;
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<ExtractElementInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

	  if (!gutils->isConstantValue(op->getVectorOperand())) {
        SmallVector<Value*,4> sv;
        sv.push_back(op->getIndexOperand());
        addToDiffeIndexed(op->getVectorOperand(), diffe(inst), sv);
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<InsertElementInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      auto dif1 = diffe(inst);

      if (!gutils->isConstantValue(op->getOperand(0)))
        addToDiffe(op->getOperand(0), Builder2.CreateInsertElement(dif1, Constant::getNullValue(op->getOperand(1)->getType()), lookup(op->getOperand(2)) ));

      if (!gutils->isConstantValue(op->getOperand(1)))
        addToDiffe(op->getOperand(1), Builder2.CreateExtractElement(dif1, lookup(op->getOperand(2))));

      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<CastInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

	  if (!gutils->isConstantValue(op->getOperand(0))) {
        if (op->getOpcode()==CastInst::CastOps::FPTrunc || op->getOpcode()==CastInst::CastOps::FPExt) {
          addToDiffe(op->getOperand(0), Builder2.CreateFPCast(diffe(inst), op->getOperand(0)->getType()));
        }
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(isa<CmpInst>(inst) || isa<PHINode>(inst) || isa<BranchInst>(inst) || isa<SwitchInst>(inst) || isa<AllocaInst>(inst) || isa<CastInst>(inst) || isa<GetElementPtrInst>(inst)) {
        continue;
    } else {
      assert(inst);
      assert(inst->getParent());
      assert(inst->getParent()->getParent());
      llvm::errs() << *inst->getParent()->getParent() << "\n" << *inst->getParent() << "\n";
      llvm::errs() << "cannot handle above inst " << *inst << "\n";
      report_fatal_error("unknown instruction");
    }
  }
 
    createInvertedTerminator(gutils, BB, retAlloca, 0 + (additionalArg ? 1 : 0) + (differentialReturn ? 1 : 0));

  }
  
  if (!topLevel)
    gutils->eraseStructuralStoresAndCalls();

  for(auto ci:gutils->addedFrees) {
    ci->moveBefore(ci->getParent()->getTerminator());
  }

  while(gutils->inversionAllocs->size() > 0) {
    gutils->inversionAllocs->back().moveBefore(gutils->newFunc->getEntryBlock().getFirstNonPHIOrDbgOrLifetimeOrAlloca());
  }

  (IRBuilder <>(gutils->inversionAllocs)).CreateUnreachable();
  DeleteDeadBlock(gutils->inversionAllocs);
  for(auto BBs : gutils->reverseBlocks) {
    if (pred_begin(BBs.second) == pred_end(BBs.second)) {
        (IRBuilder <>(BBs.second)).CreateUnreachable();
        DeleteDeadBlock(BBs.second);
    }
  }
  
  for (Argument &Arg : gutils->newFunc->args()) {
      if (Arg.hasAttribute(Attribute::Returned))
          Arg.removeAttr(Attribute::Returned);
      if (Arg.hasAttribute(Attribute::StructRet))
          Arg.removeAttr(Attribute::StructRet);
  }
  if (auto bytes = gutils->newFunc->getDereferenceableBytes(llvm::AttributeList::ReturnIndex)) {
    AttrBuilder ab;
    ab.addDereferenceableAttr(bytes);
    gutils->newFunc->removeAttributes(llvm::AttributeList::ReturnIndex, ab);
  }
  if (gutils->newFunc->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias)) {
    gutils->newFunc->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias);
  }

  if (llvm::verifyFunction(*gutils->newFunc, &llvm::errs())) {
    gutils->newFunc->dump();
    report_fatal_error("function failed verification");
  }

  optimizeIntermediate(gutils, topLevel, gutils->newFunc);

  auto nf = gutils->newFunc;
  if (oututils)
      *oututils = gutils;
  else
      delete gutils;

  return nf;
}

void HandleAutoDiff(CallInst *CI, TargetLibraryInfo &TLI, AAResults &AA) {//, LoopInfo& LI, DominatorTree& DT) {
  Value* fn = CI->getArgOperand(0);

  while (auto ci = dyn_cast<CastInst>(fn)) {
    fn = ci->getOperand(0);
  }
  while (auto ci = dyn_cast<BlockAddress>(fn)) {
    fn = ci->getFunction();
  }
  while (auto ci = dyn_cast<ConstantExpr>(fn)) {
    fn = ci->getOperand(0);
  }
  auto FT = cast<Function>(fn)->getFunctionType();
  assert(fn);
  
  if (autodiff_print)
      llvm::errs() << "prefn:\n" << *fn << "\n";

  SmallSet<unsigned,4> constants;
  SmallVector<Value*,2> args;

  unsigned truei = 0;
  IRBuilder<> Builder(CI);

  for(unsigned i=1; i<CI->getNumArgOperands(); i++) {
    Value* res = CI->getArgOperand(i);

    auto PTy = FT->getParamType(truei);
    DIFFE_TYPE ty = DIFFE_TYPE::CONSTANT;

    if (auto av = dyn_cast<MetadataAsValue>(res)) {
        auto MS = cast<MDString>(av->getMetadata())->getString();
        if (MS == "diffe_dup") {
            ty = DIFFE_TYPE::DUP_ARG;
        } else if(MS == "diffe_out") {
            ty = DIFFE_TYPE::OUT_DIFF;
        } else if (MS == "diffe_const") {
            ty = DIFFE_TYPE::CONSTANT;
        } else {
            assert(0 && "illegal diffe metadata string");
        }
        i++;
        res = CI->getArgOperand(i);
    } else 
      ty = whatType(PTy);

    if (ty == DIFFE_TYPE::CONSTANT)
      constants.insert(truei);

    assert(truei < FT->getNumParams());
    if (PTy != res->getType()) {
        if (auto ptr = dyn_cast<PointerType>(res->getType())) {
            if (auto PT = dyn_cast<PointerType>(PTy)) {
                if (ptr->getAddressSpace() != PT->getAddressSpace()) {
                    res = Builder.CreateAddrSpaceCast(res, PointerType::get(ptr->getElementType(), PT->getAddressSpace()));
                    assert(res);
                    assert(PTy);
                    assert(FT);
                    llvm::errs() << "Warning cast(1) __builtin_autodiff argument " << i << " " << *res <<"|" << *res->getType()<< " to argument " << truei << " " << *PTy << "\n" << "orig: " << *FT << "\n";
                }
            }
        }
      if (!res->getType()->canLosslesslyBitCastTo(PTy)) {
        llvm::errs() << "Cannot cast(1) __builtin_autodiff argument " << i << " " << *res << "|"<< *res->getType() << " to argument " << truei << " " << *PTy << "\n" << "orig: " << *FT << "\n";
        report_fatal_error("Illegal cast(1)");
      }
      res = Builder.CreateBitCast(res, PTy);
    }

    args.push_back(res);
    if (ty == DIFFE_TYPE::DUP_ARG) {
      i++;

      Value* res = CI->getArgOperand(i);
      if (PTy != res->getType()) {
        if (auto ptr = dyn_cast<PointerType>(res->getType())) {
            if (auto PT = dyn_cast<PointerType>(PTy)) {
                if (ptr->getAddressSpace() != PT->getAddressSpace()) {
                    res = Builder.CreateAddrSpaceCast(res, PointerType::get(ptr->getElementType(), PT->getAddressSpace()));
                    assert(res);
                    assert(PTy);
                    assert(FT);
                    llvm::errs() << "Warning cast(2) __builtin_autodiff argument " << i << " " << *res <<"|" << *res->getType()<< " to argument " << truei << " " << *PTy << "\n" << "orig: " << *FT << "\n";
                }
            }
        }
        if (!res->getType()->canLosslesslyBitCastTo(PTy)) {
          assert(res);
          assert(res->getType());
          assert(PTy);
          assert(FT);
          llvm::errs() << "Cannot cast(2) __builtin_autodiff argument " << i << " " << *res <<"|" << *res->getType()<< " to argument " << truei << " " << *PTy << "\n" << "orig: " << *FT << "\n";
          report_fatal_error("Illegal cast(2)");
        }
        res = Builder.CreateBitCast(res, PTy);
      }
      args.push_back(res);
    }

    truei++;
  }

  bool differentialReturn = cast<Function>(fn)->getReturnType()->isFPOrFPVectorTy();
  auto newFunc = CreatePrimalAndGradient(cast<Function>(fn), constants, TLI, AA, /*should return*/false, differentialReturn, /*topLevel*/true, /*outUtils*/nullptr, /*addedType*/nullptr);//, LI, DT);
  
  if (differentialReturn)
    args.push_back(ConstantFP::get(cast<Function>(fn)->getReturnType(), 1.0));
  assert(newFunc);
  if (autodiff_print)
    llvm::errs() << "postfn:\n" << *newFunc << "\n";
  Builder.setFastMathFlags(FastMathFlags::getFast());

  CallInst* diffret = cast<CallInst>(Builder.CreateCall(newFunc, args));
  diffret->setCallingConv(CI->getCallingConv());
  diffret->setDebugLoc(CI->getDebugLoc());
  if (!diffret->getType()->isEmptyTy()) {
    unsigned idxs[] = {0};
    auto diffreti = Builder.CreateExtractValue(diffret, idxs);
    CI->replaceAllUsesWith(diffreti);
  } else {
    CI->replaceAllUsesWith(UndefValue::get(CI->getType()));
  }
  CI->eraseFromParent();
}

static bool lowerAutodiffIntrinsic(Function &F, TargetLibraryInfo &TLI, AAResults &AA) {//, LoopInfo& LI, DominatorTree& DT) {
  bool Changed = false;

  for (BasicBlock &BB : F) {

    for (auto BI = BB.rbegin(), BE = BB.rend(); BI != BE;) {
      Instruction *Inst = &*BI++;
      CallInst *CI = dyn_cast_or_null<CallInst>(Inst);
      if (!CI) continue;

      Function *Fn = CI->getCalledFunction();
      if (Fn && Fn->getIntrinsicID() == Intrinsic::autodiff) {
        HandleAutoDiff(CI, TLI, AA);//, LI, DT);
        Changed = true;
      }
    }
  }

  return Changed;
}

PreservedAnalyses LowerAutodiffIntrinsicPass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  if (lowerAutodiffIntrinsic(F, AM.getResult<TargetLibraryAnalysis>(F), AM.getResult<AAManager>(F)))
    return PreservedAnalyses::none();

  return PreservedAnalyses::all();
}

namespace {
/// Legacy pass for lowering expect intrinsics out of the IR.
///
/// When this pass is run over a function it uses expect intrinsics which feed
/// branches and switches to provide branch weight metadata for those
/// terminators. It then removes the expect intrinsics from the IR so the rest
/// of the optimizer can ignore them.
class LowerAutodiffIntrinsic : public FunctionPass {
public:
  static char ID;
  LowerAutodiffIntrinsic() : FunctionPass(ID) {
    initializeLowerAutodiffIntrinsicPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<GlobalsAAWrapperPass>();
    AU.addRequiredID(LoopSimplifyID);
    AU.addRequiredID(LCSSAID);
  }

  bool runOnFunction(Function &F) override {
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
    return lowerAutodiffIntrinsic(F, TLI, AA);
  }
};
}

char LowerAutodiffIntrinsic::ID = 0;
INITIALIZE_PASS_BEGIN(LowerAutodiffIntrinsic, "lower-autodiff",
                "Lower 'autodiff' Intrinsics", false, false)

INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_END(LowerAutodiffIntrinsic, "lower-autodiff",
                "Lower 'autodiff' Intrinsics", false, false)

FunctionPass *llvm::createLowerAutodiffIntrinsicPass() {
  return new LowerAutodiffIntrinsic();
}
