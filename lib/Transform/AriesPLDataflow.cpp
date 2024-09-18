#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Analysis/Liveness.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Affine/Analysis/LoopAnalysis.h"
#include "llvm/Support/Debug.h"
#include "aries/Transform/Passes.h"
#include "aries/Transform/Utils.h"
#include "aries/Dialect/ADF/ADFDialect.h"

using namespace mlir;
using namespace aries;
using namespace adf;
using namespace mlir::memref;
using namespace mlir::func;
using namespace mlir::affine;

namespace {

struct AriesPLDataflow 
       : public AriesPLDataflowBase<AriesPLDataflow> {
public:
  void runOnOperation() override {
    auto mod = dyn_cast<ModuleOp>(getOperation());
  
    if (!PLDataflow(mod))
      signalPassFailure();
  }

private:
  // Tranverse all the forOps marked by "load,store,send,receive" and split
  // them into new functions marked by adf.pl
  void PLFuncSplit(OpBuilder builder, FuncOp plFunc){
    auto loc = builder.getUnknownLoc();
    for (auto forOp: llvm::make_early_inc_range(plFunc.getOps<AffineForOp>())){
      SmallVector<Operation*, 4> Ops;
      SmallVector<Value> inputs(forOp.getOperands());
      auto liveness = Liveness(forOp);
      for (auto livein: liveness.getLiveIn(forOp.getBody()))
        if (!forOp->isProperAncestor(livein.getParentBlock()->getParentOp())){
          auto definedOp = livein.getDefiningOp();
          if(definedOp){
            if(auto allocOp = dyn_cast<AllocOp>(definedOp)){
              auto type = dyn_cast<MemRefType>(livein.getType());
              if(auto memorySpace = type.getMemorySpace()){
                auto intAttr = dyn_cast<IntegerAttr>(memorySpace);
                if(intAttr && intAttr.getInt() == (int)MemorySpace::L2){
                  Ops.push_back(definedOp);
                  continue;
                }
              }
            }else if(auto constOp = dyn_cast<arith::ConstantOp>(definedOp)){
              Ops.push_back(definedOp);
              continue;
            }
          }
          inputs.push_back(livein);
        }
      
      builder.setInsertionPoint(plFunc);
      std::string funcName;
      std::string funcAttr;
      if(auto Attr = forOp->getAttrOfType<IntegerAttr>("load")){
        if(auto cntAttr = forOp->getAttrOfType<IntegerAttr>("func")){
          funcName = "load" + std::to_string(Attr.getInt()) + "_" 
                             + std::to_string(cntAttr.getInt());
          forOp->removeAttr("func");
        }else{
          funcName = "load" + std::to_string(Attr.getInt());
        }
        forOp->removeAttr("load");
        funcAttr = "load";
      }else if(auto Attr = forOp->getAttrOfType<IntegerAttr>("store")){
        if(auto cntAttr = forOp->getAttrOfType<IntegerAttr>("func")){
          funcName = "store" + std::to_string(Attr.getInt()) + "_" 
                             + std::to_string(cntAttr.getInt());
          forOp->removeAttr("func");
        }else{
          funcName = "store" + std::to_string(Attr.getInt());
        }
        forOp->removeAttr("store");
        funcAttr = "store";
      }else if(auto Attr = forOp->getAttrOfType<IntegerAttr>("send")){
        funcName = "send" + std::to_string(Attr.getInt());
        funcAttr = "send";
        forOp->removeAttr("send");
      }else if(auto Attr = forOp->getAttrOfType<IntegerAttr>("receive")){
        funcName = "receive" + std::to_string(Attr.getInt());
        forOp->removeAttr("receive");
        funcAttr = "receive";
      }else{
       continue; 
      }
      auto funcType = builder.getFunctionType(ValueRange(inputs),TypeRange({}));
      auto newfunc = builder.create<FuncOp>(
                                  builder.getUnknownLoc(), funcName, funcType);
      newfunc->setAttr("adf.pl",builder.getUnitAttr());
      newfunc->setAttr("inline",builder.getBoolAttr(false));
      newfunc->setAttr(funcAttr, builder.getUnitAttr());
      auto destBlock = newfunc.addEntryBlock();
      builder.setInsertionPointToEnd(destBlock);
      auto returnOp = builder.create<ReturnOp>(builder.getUnknownLoc());

      // Move L2 buffer/Constant definition inside each function
      builder.setInsertionPointToStart(destBlock);
      SmallVector<Operation*, 4> newOps;
      for(auto *op : Ops){
        auto newOp = op->clone();
        builder.insert(newOp);
        newOps.push_back(newOp);
      }

      // Move the entire block of outerPointLoop before the returnOp
      builder.setInsertionPointToEnd(destBlock);
      forOp->moveBefore(returnOp);

      // Create the function CallOp in plFunc
      auto topReturnOp = plFunc.getBody().front().getTerminator();
      builder.setInsertionPoint(topReturnOp);
      builder.create<CallOp>(loc, newfunc, inputs);

      // Update the references in the newfunc after move
      auto numArg = destBlock->getNumArguments();
      for (unsigned i = 0; i < numArg; ++i) {
        auto sourceArg = inputs[i];
        auto destArg = destBlock->getArgument(i);
        sourceArg.replaceUsesWithIf(destArg,[&](OpOperand &use){
            return newfunc->isProperAncestor(use.getOwner());
        });
      }
      auto opSize = newOps.size();
      for (unsigned i = 0; i < opSize; ++i) {
        auto oldVal = Ops[i]->getResult(0);
        auto newVal = newOps[i]->getResult(0);
        oldVal.replaceUsesWithIf(newVal,[&](OpOperand &use){
            return newfunc->isProperAncestor(use.getOwner());
        });
      }
    }
  }

  // Hoist the loops beyond loops marked by reduction, 
  // this is to implement the output stationary dataflow
  // If has reduction, then need to initialize L2 buffer marked by init
  void hoistBufferStore(OpBuilder builder, FuncOp plFunc){
    auto loc = builder.getUnknownLoc();
    auto indexType = builder.getIndexType();
    auto oneAttr = builder.getIntegerAttr(indexType, 1);
    
    AffineForOp plforOp;
    plFunc.walk([&](AffineForOp op){
      if(op->hasAttr("Array_Partition")){
        plforOp = op;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    if(!plforOp)
      return;
    SmallVector<AffineForOp, 6> tileBand;
    getLoopBandFromInnermost(plforOp, tileBand);
    auto innerloop = tileBand[tileBand.size()-1];
    auto reverseBand = tileBand;
    std::reverse(reverseBand.begin(), reverseBand.end());
    // Check forOps marked by hoist
    SmallVector<AffineForOp, 2> forOps;
    for(auto forOp : innerloop.getOps<AffineForOp>())
      if(forOp->hasAttr("hoist")){
        forOp->removeAttr("hoist");
        forOps.push_back(forOp);
      }
    if(!forOps.size())
      return;
    // Check and find the outmost reduction loop
    AffineForOp finalLoop;
    unsigned index = 0;
    unsigned indexRed = 0;
    for(auto loop : reverseBand){
      if(loop->hasAttr("reduction")){
        finalLoop = loop;
        if(index>indexRed)
          assert("Detected loop doesn't support output stationary");
        indexRed++;
      }
      index++;
    }
    for(auto forOp : forOps)
      forOp->moveAfter(finalLoop);
    // Initialize output buffer as zero
    for(auto alloc : plFunc.getOps<AllocOp>()){
      if(alloc->hasAttr("init")){
        alloc->removeAttr("init");
        auto memref = alloc.getResult();
        auto type = dyn_cast<MemRefType>(memref.getType());
        auto eleType = type.getElementType();
        auto rank = type.getRank();
        SmallVector<Value> sizes;
        SmallVector<AffineForOp, 4> newLoops;
        builder.setInsertionPointAfter(alloc);
        SmallVector<Value, 4> ivs;
        for(unsigned i = 0; i < rank; i++){
          auto size = type.getDimSize(i);
          auto newForOp = builder.create<AffineForOp>(loc, 0, size, 1);
          newLoops.push_back(newForOp);
          ivs.push_back(newForOp.getInductionVar());
          builder.setInsertionPointToStart(newForOp.getBody());
        }
        auto newInnerLoop = newLoops[newLoops.size()-1];
        newInnerLoop->setAttr("pipeline_ii", oneAttr);
        auto newInnerYiled = newInnerLoop.getBody()->getTerminator();
        builder.setInsertionPoint(newInnerYiled);
        Value value;
        if (eleType.isa<IntegerType>()) {
          auto zeroAttr = builder.getIntegerAttr(eleType, 0);
          value = builder.create<arith::ConstantOp>(loc, eleType, zeroAttr);
        }else{
          auto floatType = builder.getF32Type();
          auto floatAttr = builder.getF32FloatAttr(0.0);
          value = builder.create<arith::ConstantOp>(loc, floatType, floatAttr);
        }
        builder.create<AffineStoreOp>(loc, value, memref, ivs);
      }
    }
  }

  bool PLDataflow (ModuleOp mod) {
    auto builder = OpBuilder(mod);

    mod.walk([&](FuncOp func){
      if(!func->hasAttr("adf.pl"))
        return WalkResult::advance();
      auto attr = func->getAttr("adf.pl");
      if(!dyn_cast<BoolAttr>(attr))
        return WalkResult::advance();
      func->setAttr("dataflow",builder.getUnitAttr());
      // For each loop in adf.pl, create a new func marked by adf.pl
      PLFuncSplit(builder, func);
      return WalkResult::advance();
    });

    mod.walk([&](FuncOp func){
      if(!func->hasAttr("adf.pl"))
        return WalkResult::advance();
      hoistBufferStore(builder, func);
      return WalkResult::advance();
    });
    return true;
  }

};
} // namespace



namespace mlir {
namespace aries {

std::unique_ptr<Pass> createAriesPLDataflowPass() {
  return std::make_unique<AriesPLDataflow>();
}

} // namespace aries
} // namespace mlir