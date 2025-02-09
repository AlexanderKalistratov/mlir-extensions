// SPDX-FileCopyrightText: 2021 - 2022 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "imex/Transforms/CanonicalizeReductions.hpp"

#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/BlockAndValueMapping.h>

static bool checkMemrefType(mlir::Value value) {
  if (auto type = value.getType().dyn_cast<mlir::MemRefType>()) {
    //        auto shape = type.getShape();
    //        return shape.empty() || (1 == shape.size() && 1 == shape[0]);
    return true;
  }
  return false;
}

static bool isOutsideBlock(mlir::ValueRange values, mlir::Block &block) {
  auto blockArgs = block.getArguments();
  for (auto val : values) {
    if (llvm::is_contained(blockArgs, val))
      return false;

    auto op = val.getDefiningOp();
    if (op && block.findAncestorOpInBlock(*op))
      return false;
  }
  return true;
}

static bool checkForPotentialAliases(mlir::Value value,
                                     mlir::Operation *parent) {
  assert(parent->getRegions().size() == 1);
  assert(llvm::hasNItems(parent->getRegions().front(), 1));
  if (auto effects = mlir::dyn_cast_or_null<mlir::MemoryEffectOpInterface>(
          value.getDefiningOp())) {
    if (!effects.onlyHasEffect<mlir::MemoryEffects::Allocate>())
      return false;
  } else {
    return false;
  }

  mlir::memref::LoadOp load;
  mlir::memref::StoreOp store;
  auto &parentBlock = parent->getRegions().front().front();
  for (auto user : value.getUsers()) {
    if (mlir::isa<mlir::ViewLikeOpInterface>(user))
      return false; // TODO: very conservative

    if (!parent->isProperAncestor(user))
      continue;

    if (auto effects =
            mlir::dyn_cast_or_null<mlir::MemoryEffectOpInterface>(user)) {
      if (user->getBlock() != &parentBlock)
        return false;

      if (effects.hasEffect<mlir::MemoryEffects::Read>()) {
        if (load || !mlir::isa<mlir::memref::LoadOp>(user))
          return false;

        load = mlir::cast<mlir::memref::LoadOp>(user);
      }
      if (effects.hasEffect<mlir::MemoryEffects::Write>()) {
        if (store || !mlir::isa<mlir::memref::StoreOp>(user))
          return false;

        store = mlir::cast<mlir::memref::StoreOp>(user);
      }
    }
  }
  if (!load || !store || !load->isBeforeInBlock(store) ||
      load.getIndices() != store.getIndices() ||
      !isOutsideBlock(load.getIndices(), parentBlock)) {
    return false;
  }
  return true;
}

static bool checkSupportedOps(mlir::Value value, mlir::Operation *parent) {
  for (auto user : value.getUsers()) {
    if (user->getParentOp() == parent &&
        !mlir::isa<mlir::memref::LoadOp, mlir::memref::StoreOp>(user))
      return false;
  }
  return true;
}

static bool checkMemref(mlir::Value value, mlir::Operation *parent) {
  return checkMemrefType(value) && checkForPotentialAliases(value, parent) &&
         checkSupportedOps(value, parent);
}

static mlir::Value createScalarLoad(mlir::PatternRewriter &builder,
                                    mlir::Location loc, mlir::Value memref,
                                    mlir::ValueRange indices) {
  return builder.create<mlir::memref::LoadOp>(loc, memref, indices);
}

static void createScalarStore(mlir::PatternRewriter &builder,
                              mlir::Location loc, mlir::Value val,
                              mlir::Value memref, mlir::ValueRange indices) {
  builder.create<mlir::memref::StoreOp>(loc, val, memref, indices);
}

mlir::LogicalResult imex::CanonicalizeReduction::matchAndRewrite(
    mlir::scf::ForOp op, mlir::PatternRewriter &rewriter) const {
  llvm::SmallVector<std::pair<mlir::Value, mlir::ValueRange>> toProcess;
  for (auto &current : op.getLoopBody().front()) {
    if (auto load = mlir::dyn_cast<mlir::memref::LoadOp>(current)) {
      auto memref = load.getMemref();
      if (checkMemref(memref, op))
        toProcess.push_back({memref, load.getIndices()});
    }
  }

  if (!toProcess.empty()) {
    auto loc = op.getLoc();
    auto initArgs = llvm::to_vector<8>(op.getInitArgs());
    for (auto it : toProcess) {
      initArgs.emplace_back(
          createScalarLoad(rewriter, loc, it.first, it.second));
    }
    auto prevArgsOffset = op.getInitArgs().size();
    auto body = [&](mlir::OpBuilder &builder, mlir::Location loc,
                    mlir::Value iter, mlir::ValueRange iterVals) {
      auto &oldBody = op.getLoopBody().front();
      mlir::BlockAndValueMapping mapping;
      mapping.map(oldBody.getArguments().front(), iter);
      mapping.map(oldBody.getArguments().drop_front(), iterVals);
      auto yieldArgs = llvm::to_vector(iterVals);
      for (auto &bodyOp : oldBody.without_terminator()) {
        auto invalidIndex = static_cast<unsigned>(-1);
        auto getIterIndex = [&](auto op) -> unsigned {
          auto arg = op.getMemref();
          for (auto it : llvm::enumerate(llvm::make_first_range(toProcess))) {
            if (arg == it.value()) {
              return static_cast<unsigned>(it.index() + prevArgsOffset);
            }
          }
          return invalidIndex;
        };
        if (auto load = mlir::dyn_cast<mlir::memref::LoadOp>(bodyOp)) {
          auto index = getIterIndex(load);
          if (index != invalidIndex) {
            mapping.map(bodyOp.getResults().front(), yieldArgs[index]);
          } else {
            builder.clone(bodyOp, mapping);
          }
        } else if (auto store = mlir::dyn_cast<mlir::memref::StoreOp>(bodyOp)) {
          auto index = getIterIndex(store);
          if (index != invalidIndex) {
            yieldArgs[index] = mapping.lookup(store.getValue());
          } else {
            builder.clone(bodyOp, mapping);
          }
        } else {
          builder.clone(bodyOp, mapping);
        }
      }
      auto yield = mlir::cast<mlir::scf::YieldOp>(oldBody.getTerminator());
      llvm::copy(yield.getResults(), yieldArgs.begin());
      builder.create<mlir::scf::YieldOp>(loc, yieldArgs);
    };
    auto results = rewriter
                       .create<mlir::scf::ForOp>(loc, op.getLowerBound(),
                                                 op.getUpperBound(),
                                                 op.getStep(), initArgs, body)
                       .getResults();
    for (auto it : llvm::enumerate(toProcess)) {
      auto index = prevArgsOffset + it.index();
      auto result = results[static_cast<unsigned>(index)];
      createScalarStore(rewriter, loc, result, it.value().first,
                        it.value().second);
    }
    rewriter.replaceOp(op, results.take_front(prevArgsOffset));
    return mlir::success();
  }

  return mlir::failure();
}
