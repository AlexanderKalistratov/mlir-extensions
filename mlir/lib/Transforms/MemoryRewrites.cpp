// SPDX-FileCopyrightText: 2021 - 2022 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "imex/Transforms/MemoryRewrites.hpp"

#include "imex/Analysis/MemorySsaAnalysis.hpp"

#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/Vector/IR/VectorOps.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Dominance.h>
#include <mlir/IR/FunctionInterfaces.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>

namespace {
struct Meminfo {
  mlir::Value memref;
  mlir::ValueRange indices;

  bool operator==(const Meminfo &other) const {
    return memref == other.memref && indices == other.indices;
  }
};

static llvm::Optional<Meminfo> getMeminfo(mlir::Operation *op) {
  assert(nullptr != op);
  if (auto load = mlir::dyn_cast<mlir::memref::LoadOp>(op))
    return Meminfo{load.getMemref(), load.getIndices()};

  if (auto store = mlir::dyn_cast<mlir::memref::StoreOp>(op))
    return Meminfo{store.getMemref(), store.getIndices()};

  if (auto load = mlir::dyn_cast<mlir::vector::LoadOp>(op))
    return Meminfo{load.getBase(), load.getIndices()};

  if (auto store = mlir::dyn_cast<mlir::vector::StoreOp>(op))
    return Meminfo{store.getBase(), store.getIndices()};

  return {};
}

static mlir::Value getStoreValue(mlir::Operation *op) {
  assert(op);
  if (auto store = mlir::dyn_cast<mlir::memref::StoreOp>(op))
    return store.getValue();

  if (auto store = mlir::dyn_cast<mlir::vector::StoreOp>(op))
    return store.getValueToStore();

  llvm_unreachable("Invalid store op");
}

struct MustAlias {
  bool operator()(mlir::Operation *op1, mlir::Operation *op2) const {
    auto meminfo1 = getMeminfo(op1);
    if (!meminfo1)
      return false;

    auto meminfo2 = getMeminfo(op2);
    if (!meminfo2)
      return false;

    return *meminfo1 == *meminfo2;
  }
};

static mlir::LogicalResult
optimizeUses(imex::MemorySSAAnalysis &memSSAAnalysis) {
  return memSSAAnalysis.optimizeUses();
}

static mlir::LogicalResult foldLoads(imex::MemorySSAAnalysis &memSSAAnalysis) {
  assert(memSSAAnalysis.memssa);
  auto &memSSA = *memSSAAnalysis.memssa;
  using NodeType = imex::MemorySSA::NodeType;
  bool changed = false;
  for (auto &node : llvm::make_early_inc_range(memSSA.getNodes())) {
    if (NodeType::Use == memSSA.getNodeType(&node)) {
      auto op1 = memSSA.getNodeOperation(&node);
      assert(nullptr != op1);
      if (op1->getNumResults() != 1)
        continue;

      auto def = memSSA.getNodeDef(&node);
      assert(nullptr != def);
      if (NodeType::Def != memSSA.getNodeType(def))
        continue;

      auto op2 = memSSA.getNodeOperation(def);
      assert(nullptr != op2);
      if (MustAlias()(op1, op2)) {
        auto val = getStoreValue(op2);
        auto res = op1->getResult(0);
        if (val.getType() == res.getType()) {
          res.replaceAllUsesWith(val);
          op1->erase();
          memSSA.eraseNode(&node);
          changed = true;
        }
      }
    }
  }
  return mlir::success(changed);
}

static mlir::LogicalResult
deadStoreElemination(imex::MemorySSAAnalysis &memSSAAnalysis) {
  assert(memSSAAnalysis.memssa);
  auto &memSSA = *memSSAAnalysis.memssa;
  using NodeType = imex::MemorySSA::NodeType;
  auto getNextDef =
      [&](imex::MemorySSA::Node *node) -> imex::MemorySSA::Node * {
    imex::MemorySSA::Node *def = nullptr;
    for (auto user : memSSA.getUsers(node)) {
      auto type = memSSA.getNodeType(user);
      if (NodeType::Def == type) {
        if (def != nullptr)
          return nullptr;

        def = user;
      } else {
        return nullptr;
      }
    }
    return def;
  };
  bool changed = false;
  for (auto &node : llvm::make_early_inc_range(memSSA.getNodes())) {
    if (NodeType::Def == memSSA.getNodeType(&node)) {
      if (auto nextDef = getNextDef(&node)) {
        auto op1 = memSSA.getNodeOperation(&node);
        auto op2 = memSSA.getNodeOperation(nextDef);
        assert(nullptr != op1);
        assert(nullptr != op2);
        if (MustAlias()(op1, op2)) {
          op1->erase();
          memSSA.eraseNode(&node);
          changed = true;
        }
      }
    }
  }
  return mlir::success(changed);
}

struct SimpleOperationInfo : public llvm::DenseMapInfo<mlir::Operation *> {
  static unsigned getHashValue(const mlir::Operation *opC) {
    return static_cast<unsigned>(mlir::OperationEquivalence::computeHash(
        const_cast<mlir::Operation *>(opC),
        mlir::OperationEquivalence::directHashValue,
        mlir::OperationEquivalence::ignoreHashValue,
        mlir::OperationEquivalence::IgnoreLocations));
  }
  static bool isEqual(const mlir::Operation *lhsC,
                      const mlir::Operation *rhsC) {
    auto *lhs = const_cast<mlir::Operation *>(lhsC);
    auto *rhs = const_cast<mlir::Operation *>(rhsC);
    if (lhs == rhs)
      return true;
    if (lhs == getTombstoneKey() || lhs == getEmptyKey() ||
        rhs == getTombstoneKey() || rhs == getEmptyKey())
      return false;
    return mlir::OperationEquivalence::isEquivalentTo(
        const_cast<mlir::Operation *>(lhsC),
        const_cast<mlir::Operation *>(rhsC),
        mlir::OperationEquivalence::exactValueMatch,
        mlir::OperationEquivalence::ignoreValueEquivalence,
        mlir::OperationEquivalence::IgnoreLocations);
  }
};

static mlir::LogicalResult loadCSE(imex::MemorySSAAnalysis &memSSAAnalysis) {
  mlir::DominanceInfo dom;
  assert(memSSAAnalysis.memssa);
  auto &memSSA = *memSSAAnalysis.memssa;
  using NodeType = imex::MemorySSA::NodeType;
  bool changed = false;
  llvm::SmallDenseMap<mlir::Operation *, mlir::Operation *, 4,
                      SimpleOperationInfo>
      opsMap;
  for (auto &node : memSSA.getNodes()) {
    auto nodeType = memSSA.getNodeType(&node);
    if (NodeType::Def != nodeType && NodeType::Phi != nodeType &&
        NodeType::Root != nodeType)
      continue;

    opsMap.clear();
    for (auto user : memSSA.getUsers(&node)) {
      if (memSSA.getNodeType(user) != NodeType::Use)
        continue;

      auto op = memSSA.getNodeOperation(user);
      if (!op->getRegions().empty())
        continue;

      auto it = opsMap.find(op);
      if (it == opsMap.end()) {
        opsMap.insert({op, op});
      } else {
        auto firstUser = it->second;
        if (!MustAlias()(op, firstUser))
          continue;

        if (dom.properlyDominates(op, firstUser)) {
          firstUser->replaceAllUsesWith(op);
          opsMap[firstUser] = op;
          auto firstUserNode = memSSA.getNode(firstUser);
          assert(firstUserNode);
          memSSA.eraseNode(firstUserNode);
          firstUser->erase();
          changed = true;
        } else if (dom.properlyDominates(firstUser, op)) {
          op->replaceAllUsesWith(firstUser);
          op->erase();
          memSSA.eraseNode(user);
          changed = true;
        }
      }
    }
  }
  return mlir::success(changed);
}

} // namespace

llvm::Optional<mlir::LogicalResult>
imex::optimizeMemoryOps(mlir::AnalysisManager &am) {
  auto &memSSAAnalysis = am.getAnalysis<MemorySSAAnalysis>();
  if (!memSSAAnalysis.memssa)
    return {};

  using fptr_t = mlir::LogicalResult (*)(MemorySSAAnalysis &);
  const fptr_t funcs[] = {
      &optimizeUses,
      &foldLoads,
      &deadStoreElemination,
      &loadCSE,
  };

  bool changed = false;
  bool repeat = false;

  do {
    repeat = false;
    for (auto func : funcs) {
      if (mlir::succeeded(func(memSSAAnalysis))) {
        changed = true;
        repeat = true;
      }
    }
  } while (repeat);

  return mlir::success(changed);
}

namespace {
struct RemoveDeadAllocs
    : public mlir::OpInterfaceRewritePattern<mlir::MemoryEffectOpInterface> {
  using OpInterfaceRewritePattern::OpInterfaceRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::MemoryEffectOpInterface op,
                  mlir::PatternRewriter &rewriter) const override {
    if (!op.onlyHasEffect<mlir::MemoryEffects::Allocate>() ||
        op->getNumResults() != 1)
      return mlir::failure();

    auto res = op->getResult(0);
    for (auto user : op->getUsers()) {
      if (user->getNumResults() != 0)
        return mlir::failure();

      auto memInterface = mlir::dyn_cast<mlir::MemoryEffectOpInterface>(user);
      if (!memInterface)
        return mlir::failure();

      if (!memInterface.getEffectOnValue<mlir::MemoryEffects::Free>(res) &&
          !memInterface.getEffectOnValue<mlir::MemoryEffects::Write>(res))
        return mlir::failure();

      if (memInterface.getEffectOnValue<mlir::MemoryEffects::Read>(res))
        return mlir::failure();
    }

    for (auto user : llvm::make_early_inc_range(op->getUsers()))
      rewriter.eraseOp(user);

    rewriter.eraseOp(op);
    return mlir::success();
  }
};

struct MemoryOptPass
    : public mlir::PassWrapper<MemoryOptPass,
                               mlir::InterfacePass<mlir::FunctionOpInterface>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MemoryOptPass)

  virtual void
  getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::memref::MemRefDialect>();
    registry.insert<mlir::vector::VectorDialect>();
  }

  void runOnOperation() override {
    auto *ctx = &getContext();
    mlir::RewritePatternSet patterns(ctx);

    patterns.insert<RemoveDeadAllocs>(ctx);

    mlir::FrozenRewritePatternSet fPatterns(std::move(patterns));
    auto am = getAnalysisManager();
    while (true) {
      (void)mlir::applyPatternsAndFoldGreedily(getOperation(), fPatterns);
      am.invalidate({});
      auto res = imex::optimizeMemoryOps(am);
      if (!res) {
        getOperation()->emitError("Failed to build memory SSA analysis");
        return signalPassFailure();
      }
      if (mlir::failed(*res))
        break;
    }
  }
};
} // namespace

std::unique_ptr<mlir::Pass> imex::createMemoryOptPass() {
  return std::make_unique<MemoryOptPass>();
}
