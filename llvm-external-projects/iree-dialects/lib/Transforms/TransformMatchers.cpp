// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Transforms/TransformMatchers.h"

#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/StructuredOpsUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Debug.h"

using namespace mlir;

#define DEBUG_TYPE "transform-matchers"
#define DBGS() llvm::dbgs() << "[" DEBUG_TYPE "] "

//===---------------------------------------------------------------------===//
// StructuredOpMatcher and friends.
//===---------------------------------------------------------------------===//

void transform_ext::StructuredOpMatcher::debugOutputForCreate(
    ArrayRef<StringRef> opNames) {
  LLVM_DEBUG(DBGS() << "operation type is one of {";
             llvm::interleaveComma(opNames, llvm::dbgs()); llvm::dbgs() << "}");
}

bool transform_ext::StructuredOpMatcher::match(Operation *op) {
  auto debugRAII =
      llvm::make_scope_exit([] { LLVM_DEBUG(DBGS() << "-------\n"); });
  LLVM_DEBUG(DBGS() << "matching: " << *op << "\n");
  auto linalgOp = dyn_cast<linalg::LinalgOp>(op);
  if (!linalgOp) {
    LLVM_DEBUG(DBGS() << "not a structured op\n");
    return false;
  }

  if (!llvm::all_of(predicates, [linalgOp](const PredicateFn &fn) {
        bool result = fn(linalgOp);
        LLVM_DEBUG(llvm::dbgs() << ": " << result << "\n");
        return result;
      })) {
    return false;
  }

  captured = linalgOp;
  return true;
}

//===---------------------------------------------------------------------===//
// Constraints on op rank and dims.
//===---------------------------------------------------------------------===//

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::rank(NumGreaterEqualTo minRank) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "rank >= " << minRank.value);
    return linalgOp.getNumLoops() >= minRank.value;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::rank(NumLowerEqualTo maxRank) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "rank <= " << maxRank.value);
    return linalgOp.getNumLoops() <= maxRank.value;
  });
  return *this;
}

StringRef stringifyShapeKind(transform_ext::ShapeKind kind) {
  switch (kind) {
  case transform_ext::ShapeKind::Static:
    return "static";
  case transform_ext::ShapeKind::Dynamic:
    return "dynamic";
  }
  llvm_unreachable("unhandled shape kind");
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(SmallVector<int64_t> &&dimensions,
                                        ShapeKind kind) {
  predicates.push_back([dimensions = std::move(dimensions),
                        kind](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "dimensions [";
               llvm::interleaveComma(dimensions, llvm::dbgs());
               llvm::dbgs() << "] are " << stringifyShapeKind(kind));
    SmallVector<int64_t> shape = linalgOp.getStaticLoopRanges();
    for (auto dimension : dimensions) {
      int64_t transformedDimension =
          dimension >= 0 ? dimension : shape.size() + dimension;
      if (transformedDimension < 0 || transformedDimension >= shape.size())
        return false;
      if (ShapedType::isDynamic(shape[transformedDimension]) ^
          (kind == ShapeKind::Static))
        continue;
      return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(AllDims tag, ShapeKind kind) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all dimensions are " << stringifyShapeKind(kind));
    SmallVector<int64_t> shape = linalgOp.getStaticLoopRanges();
    return llvm::all_of(shape, [=](int64_t dimension) {
      return ShapedType::isDynamic(dimension) ^ (kind == ShapeKind::Static);
    });
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(SmallVector<int64_t> &&dimensions,
                                        utils::IteratorType kind) {
  predicates.push_back([dimensions = std::move(dimensions),
                        kind](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "dimensions [";
               llvm::interleaveComma(dimensions, llvm::dbgs());
               llvm::dbgs() << "] are " << utils::stringifyIteratorType(kind));
    unsigned rank = linalgOp.getNumLoops();
    for (auto dimension : dimensions) {
      int64_t transformedDimension =
          dimension >= 0 ? dimension : rank + dimension;
      if (transformedDimension < 0 || transformedDimension >= rank)
        return false;
      utils::IteratorType iteratorKind =
          linalgOp.getIteratorTypesArray()[transformedDimension];
      if (iteratorKind == kind)
        continue;
      return false;
    }
    return true;
  });
  return *this;
}
transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(AllDims tag, utils::IteratorType kind) {
  return dim(AllDimsExcept({}), kind);
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(AllDimsExcept &&dims,
                                        utils::IteratorType kind) {
  predicates.push_back([dimensions = std::move(dims),
                        kind](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all dimensions except [";
               llvm::interleaveComma(dimensions.getExcluded(), llvm::dbgs());
               llvm::dbgs() << "] are " << utils::stringifyIteratorType(kind));
    int64_t rank = linalgOp.getNumLoops();
    llvm::SmallDenseSet<int64_t> excludedDims;
    for (int64_t dim : dimensions.getExcluded()) {
      excludedDims.insert(dim >= 0 ? dim : rank + dim);
    }

    for (auto [index, type] :
         llvm::enumerate(linalgOp.getIteratorTypesArray())) {
      if (excludedDims.contains(index))
        continue;
      if (type == kind)
        continue;
      return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(int64_t dimension,
                                        DivisibleBy divisibleBy) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "dimension " << dimension << " is divisible by "
                      << divisibleBy.value);
    unsigned rank = linalgOp.getNumLoops();
    int64_t transformedDimension =
        dimension >= 0 ? dimension : rank + dimension;
    if (transformedDimension >= rank)
      return false;

    int64_t size = linalgOp.getStaticLoopRanges()[transformedDimension];
    return !ShapedType::isDynamic(size) && (size % divisibleBy.value == 0);
  });
  return *this;
}

//===---------------------------------------------------------------------===//
// Capture directives.
//===---------------------------------------------------------------------===//
transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::rank(CaptureStaticValue<int64_t> capture) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "capture rank");
    capture.value = linalgOp.getNumLoops();
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::dim(int64_t dimension,
                                        CaptureStaticValue<int64_t> capture) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "capture dimension");
    unsigned rank = linalgOp.getNumLoops();
    int64_t transformedDimension =
        dimension >= 0 ? dimension : rank + dimension;
    if (transformedDimension >= rank)
      return false;

    capture.value = linalgOp.getStaticLoopRanges()[transformedDimension];
    return true;
  });
  return *this;
}

//===---------------------------------------------------------------------===//
// Constraints on input operands.
//===---------------------------------------------------------------------===//

void transform_ext::StructuredOpMatcher::addInputMatcher(
    int64_t position, std::function<bool(Operation *)> matcher,
    OptionalMatch optional) {
  predicates.push_back([position, optional, matcher = std::move(matcher)](
                           linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "input operand #" << position
                      << (optional.value ? " (optional match) " : " ")
                      << "produced by\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInputs() + position;
    if (transformedPosition >= linalgOp.getNumDpsInputs())
      return false;

    Operation *definingOp =
        linalgOp.getDpsInputOperand(transformedPosition)->get().getDefiningOp();
    if (!definingOp)
      return optional.value;
    // We MUST run the matcher at this point, even if the match is optional,
    // to allow for capture.
    LLVM_DEBUG(DBGS() << "start recursive match {\n");
    auto debugRAII = llvm::make_scope_exit(
        [] { LLVM_DEBUG(DBGS() << "} end recursive match"); });
    if (matcher(definingOp))
      return true;
    return optional.value;
  });
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::input(AllOperands tag, IsPermutation) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all input operands have permutation maps");
    // all_of with a lambda requires const-casting dance, so using a loop.
    for (OpOperand *operand : linalgOp.getDpsInputOperands()) {
      if (!linalgOp.getMatchingIndexingMap(operand).isPermutation())
        return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::input(AllOperands tag,
                                          IsProjectedPermutation) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all input operands have projected permutation maps");
    // all_of with a lambda requires const-casting dance, so using a loop.
    for (OpOperand *operand : linalgOp.getDpsInputOperands()) {
      if (!linalgOp.getMatchingIndexingMap(operand).isProjectedPermutation())
        return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::input(NumEqualsTo num) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "number of input operands == " << num.value);
    return linalgOp.getNumDpsInputs() == num.value;
  });
  return *this;
}

/// Traverses the transitive sources of `val` until it reaches an operation that
/// is not a known "subset-like" operation, i.e. `extract_slice` or
/// `foreach_thread`.
static Operation *traverseSubsetsBackwards(Value val) {
  do {
    Operation *op = val.getDefiningOp();
    if (!op) {
      // TODO: This should likely be done via RegionBranchOpInterface as a sort
      // of data flow analysis.
      auto bbArg = val.cast<BlockArgument>();
      Operation *blockOp = bbArg.getOwner()->getParentOp();
      assert(blockOp && "detached block");
      if (auto loop = dyn_cast<scf::ForeachThreadOp>(blockOp)) {
        val = loop.getTiedOpOperand(bbArg)->get();
        continue;
      }
      return blockOp;
    }

    // TODO: We may eventually want a "subset-like" interface that we can use to
    // traverse ops here and in post-canonicalization replacement
    // identification.
    if (auto extractSlice = dyn_cast<tensor::ExtractSliceOp>(op)) {
      val = extractSlice.getSource();
      continue;
    }
    return op;
  } while (true);
}

/// Greedily traverses the transitive uses of `val` until it reaches an
/// operation that is not a known "subset-like" operation, i.e. `extract_slice`
/// or `foreach_thread`.
static Operation *traverseSubsetsForwardAnyUse(Value val) {
  do {
    for (OpOperand &use : val.getUses()) {
      Operation *user = use.getOwner();
      if (auto loop = dyn_cast<scf::ForeachThreadOp>(user)) {
        auto range = loop.getOutputBlockArguments();
        auto it = llvm::find_if(range, [&](BlockArgument bbarg) {
          return loop.getTiedOpOperand(bbarg) != &use;
        });
        if (it == range.end())
          return user;
        val = *it;
        continue;
      }
      if (auto slice = dyn_cast<tensor::ExtractSliceOp>(user)) {
        val = slice.getResult();
        continue;
      }
      return user;
    }
  } while (true);
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::input(int64_t position, SubsetOf subset) {
  // Implementation note: SubsetOf must *not* be passed by-reference because
  // it is typically a temporary constructed within the argument of a function
  // call, but it will be used in the lambda that outlives the temporary. The
  // lambda itself must capture by value for the same reason.
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "operand #" << position << " is a subset of\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInputs() + position;
    if (transformedPosition >= linalgOp.getNumDpsInputs())
      return false;

    LLVM_DEBUG(DBGS() << "start recursive match {\n");
    Operation *producer = traverseSubsetsBackwards(
        linalgOp.getDpsInputOperand(transformedPosition)->get());
    bool result = subset.matcher.match(producer);
    LLVM_DEBUG(DBGS() << "} end recursive match");
    return result;
  });
  recordNestedMatcher(subset.matcher);
  return *this;
}

//===---------------------------------------------------------------------===//
// Constraints on output operands.
//===---------------------------------------------------------------------===//

void transform_ext::StructuredOpMatcher::addOutputMatcher(
    int64_t position, std::function<bool(Operation *)> matcher,
    OptionalMatch optional) {
  predicates.push_back([position, optional, matcher = std::move(matcher)](
                           linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "output operand #" << position
                      << (optional.value ? " (optional match) " : " ")
                      << "is produced by\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInits() + position;
    if (transformedPosition >= linalgOp.getNumDpsInits())
      return false;

    Operation *definingOp =
        linalgOp.getDpsInitOperand(transformedPosition)->get().getDefiningOp();
    if (!definingOp)
      return optional.value;
    // We MUST run the matcher at this point, even if the match is optional,
    // to allow for capture.
    LLVM_DEBUG(DBGS() << "start recursive match {\n");
    auto debugRAII = llvm::make_scope_exit(
        [] { LLVM_DEBUG(DBGS() << "} end recursive match"); });
    if (matcher(definingOp))
      return true;
    return optional.value;
  });
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(AllOperands tag, IsPermutation) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all output operands have permutation maps");
    for (OpOperand *operand : linalgOp.getDpsInitOperands()) {
      if (!linalgOp.getMatchingIndexingMap(operand).isPermutation())
        return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(AllOperands tag,
                                           IsProjectedPermutation) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "all output operands have projected permutation maps");
    for (OpOperand *operand : linalgOp.getDpsInitOperands()) {
      if (!linalgOp.getMatchingIndexingMap(operand).isProjectedPermutation())
        return false;
    }
    return true;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(int64_t position,
                                           ElementTypeBitWidth width) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "output operand #" << position
                      << " has elemental type with bit width " << width.value);
    int64_t updatedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInits() + position;
    if (updatedPosition >= linalgOp.getNumDpsInits())
      return false;
    auto shapedType = linalgOp.getDpsInitOperand(updatedPosition)
                          ->get()
                          .getType()
                          .dyn_cast<ShapedType>();
    return shapedType && shapedType.getElementType().isIntOrFloat() &&
           shapedType.getElementType().getIntOrFloatBitWidth() == width.value;
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(int64_t position,
                                           SingleCombinerReduction tag) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "output operand #" << position
                      << " is populated by a single-combiner reduction");
    int64_t updatedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInits() + position;
    if (updatedPosition >= linalgOp.getNumDpsInits())
      return false;
    SmallVector<Operation *> combinerOps;
    return matchReduction(linalgOp.getRegionOutputArgs(), updatedPosition,
                          combinerOps) &&
           llvm::hasSingleElement(combinerOps);
  });
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(int64_t position, SubsetOf subset) {
  // Implementation note: SubsetOf must *not* be passed by-reference because
  // it is typically a temporary constructed within the argument of a function
  // call, but it will be used in the lambda that outlives the temporary. The
  // lambda itself must capture by value for the same reason.
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "output operand #" << position
                      << " is produced by a subset of\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp.getNumDpsInputs() + position;
    if (transformedPosition >= linalgOp.getNumDpsInputs())
      return false;

    LLVM_DEBUG(llvm::dbgs() << "start recursive match {\n");
    Operation *producer = traverseSubsetsBackwards(
        linalgOp.getDpsInitOperand(transformedPosition)->get());
    bool result = subset.matcher.match(producer);
    LLVM_DEBUG(DBGS() << "} end recursive match");
    return result;
  });
  recordNestedMatcher(subset.matcher);
  return *this;
}

transform_ext::StructuredOpMatcher &
transform_ext::StructuredOpMatcher::output(NumEqualsTo num) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "number of output operands == " << num.value);
    return linalgOp.getNumDpsInits() == num.value;
  });
  return *this;
}

//===---------------------------------------------------------------------===//
// Constraints on results.
//===---------------------------------------------------------------------===//

void transform_ext::StructuredOpMatcher::addResultMatcher(
    int64_t position, HasAnyUse tag, std::function<bool(Operation *)> matcher,
    OptionalMatch optional) {
  predicates.push_back([matcher = std::move(matcher), optional,
                        position](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "result #" << position
                      << (optional.value ? " (optional match) " : " ")
                      << "has a use\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp->getNumResults() + position;
    if (transformedPosition >= linalgOp->getNumResults())
      return false;

    // We MUST run the matcher at this point, even if the match is optional,
    // to allow for capture.
    LLVM_DEBUG(DBGS() << "start recursive match {\n");
    auto debugRAII = llvm::make_scope_exit(
        [] { LLVM_DEBUG(DBGS() << "} end recursive match"); });
    if (llvm::any_of(linalgOp->getResult(transformedPosition).getUsers(),
                     [&matcher](Operation *op) { return matcher(op); })) {
      return true;
    }
    return optional.value;
  });
}

transform_ext::StructuredOpMatcher &transform_ext::StructuredOpMatcher::result(
    int64_t position, HasAnyUse tag, SubsetOf subset, OptionalMatch optional) {
  predicates.push_back([=](linalg::LinalgOp linalgOp) -> bool {
    LLVM_DEBUG(DBGS() << "result #" << position
                      << "has a use that is a subset of\n");
    int64_t transformedPosition =
        position >= 0 ? position : linalgOp->getNumResults() + position;
    if (transformedPosition >= linalgOp->getNumResults())
      return false;

    LLVM_DEBUG(DBGS() << "start recursive match {\n");
    Operation *user =
        traverseSubsetsForwardAnyUse(linalgOp->getResult(transformedPosition));
    bool result = subset.matcher.match(user) || optional.value;
    LLVM_DEBUG(DBGS() << "} end recursive match");
    return result;
  });
  recordNestedMatcher(subset.matcher);
  return *this;
}

bool transform_ext::StructuredOpMatcher::checkAllTilableMatched(
    Operation *parent, linalg::LinalgOp linalgOp,
    ArrayRef<transform_ext::CapturingOpMatcher *> matchers) {
  LLVM_DEBUG(DBGS() << "all tilable ops captured");
  int64_t numTilableOps = 0;
  if (!parent)
    return false;
  parent->walk([&](TilingInterface Op) { ++numTilableOps; });

  llvm::SmallPtrSet<Operation *, 6> matched;
  for (CapturingOpMatcher *nested : matchers) {
    if (Operation *captured = nested->getCaptured()) {
      matched.insert(captured);
    }
  }

  // Don't forget to include the root matcher.
  matched.insert(linalgOp);
  return numTilableOps == matched.size();
}

//===---------------------------------------------------------------------===//
// MatchCallbackResult.
//===---------------------------------------------------------------------===//

ArrayRef<Operation *>
transform_ext::MatchCallbackResult::getPayloadGroup(unsigned position) const {
  assert(position < payloadGroupLengths.size());
  int64_t start = 0;
  for (unsigned i = 0; i < position; ++i) {
    start += payloadGroupLengths[i];
  }
  return llvm::makeArrayRef(payloadOperations)
      .slice(start, payloadGroupLengths[position]);
}

//===---------------------------------------------------------------------===//
// Case-specific matcher builders.
//===---------------------------------------------------------------------===//

static constexpr unsigned kCudaWarpSize = 32;

void transform_ext::makeReductionMatcher(
    transform_ext::StructuredOpMatcher &reduction,
    transform_ext::StructuredOpMatcher &fill,
    transform_ext::StructuredOpMatcher &leading,
    transform_ext::StructuredOpMatcher &trailing,
    MatchedReductionCaptures &captures) {
  // The core part of the matcher is anchored on a particular reduction op.
  reduction =
      m_StructuredOp()
          // Op has at least a parallel a reduction dimension and at
          // most 3 parallel dimensions.
          // TODO: relax once we have global collapse/expand_shape.
          //
          .rank(NumGreaterEqualTo(2))
          .rank(NumLowerEqualTo(4))
          .rank(CaptureStaticValue<int64_t>(captures.reductionRank))
          // Op has a single most-minor reduction that we capture.
          .dim(-1, utils::IteratorType::reduction)
          .dim(-2, CaptureStaticValue<int64_t>(
                       captures.mostMinorParallelDimensionSize))
          .dim(-1, CaptureStaticValue<int64_t>(captures.reductionDimensionSize))
          // All other dimensions are parallel.
          .dim(AllDimsExcept({-1}), utils::IteratorType::parallel)
          // Single input for now, can be arbitrary projected permutations.
          // TODO: Multiple inputs, can be arbitrary projected permutations.
          // TODO: Watch out for multiple inputs though as a reduction turns
          //       into a contraction when mixed with projected
          //       permutations. A reduction is often bandwidth bound but
          //       contraction is a different beast that is compute bound
          //       and has a very different schedule.
          //
          .input(NumEqualsTo(1))
          .input(AllOperands(), IsProjectedPermutation())
          // Single output supported atm.
          // TODO: Multiple outputs.
          //
          .output(NumEqualsTo(1))
          // A reduction output must be a projected permutation, match it but we
          // could also drop this technically.
          .output(AllOperands(), IsProjectedPermutation())
          // Only single combiner over 32 bits for now due to reduction warp
          // distribution.
          // TODO: relax this once reduction distribution is more powerful.
          //
          .output(0, ElementTypeBitWidth(32))
          .output(0, SingleCombinerReduction());

  // Mandatory FillOp must create the unique output of the reduction.
  // TODO: Relax this, as any map, broadcast, transpose should also work.
  //
  fill = m_StructuredOp<linalg::FillOp>();
  reduction = reduction.output(NumEqualsTo(1)).output(0, fill);

  // Optional leading or trailign op can be any map, transpose, broadcast but
  // not reduce or windowing operation for now.
  // It must create the unique input for the reduction.
  // TODO: match more optional leading ops, one per input of the reduction.
  // TODO: careful about multi-output and turning into a contraction.
  //
  transform_ext::StructuredOpMatcher commonLeadingOrTrailing =
      m_StructuredOp<linalg::GenericOp>()
          // All parallel dimensions.
          .dim(AllDims(), utils::IteratorType::parallel)
          // All inputs are any projected permutation.
          .input(AllOperands(), IsProjectedPermutation())
          .output(AllOperands(), IsPermutation())
          // leading and trailing may have 0, 1 or more input as long as they do
          // not come from unmatched ops. This extra constraint is taken care of
          // separately. This is also a noop but we document it.
          // TODO: Base and derived classes, atm this does not compile.
          // .input(NumGreaterEqualTo(0))
          // Single output supported atm.
          // TODO: extend this.
          //
          .output(NumEqualsTo(1));
  // TODO: match more optional leading ops, one per input of the reduction.
  // TODO: careful about multi-output and turning into a contraction.
  //
  leading = commonLeadingOrTrailing.rank(
      CaptureStaticValue<int64_t>(captures.maybeLeadingRank));
  reduction = reduction.input(0, leading, OptionalMatch());

  // Optional trailing can be any map, transpose, broadcast but not reduce or
  // windowing operation for now.
  // It must be fed by the unique input for the reduction.
  // TODO: match more optional leading ops, one per input of the reduction.
  // TODO: careful about multi-output and turning into a contraction.
  //
  trailing = commonLeadingOrTrailing.rank(
      CaptureStaticValue<int64_t>(captures.maybeTrailingRank));
  reduction = reduction.result(0, HasAnyUse(), trailing, OptionalMatch())
                  .allTilableOpsCaptured<func::FuncOp>();
}

void transform_ext::makeSplitReductionMatcher(
    transform_ext::StructuredOpMatcher &parallel_reduction,
    transform_ext::StructuredOpMatcher &combiner_reduction,
    transform_ext::StructuredOpMatcher &parallel_fill,
    transform_ext::StructuredOpMatcher &original_fill,
    transform_ext::StructuredOpMatcher &leading,
    transform_ext::StructuredOpMatcher &trailing) {
  original_fill = m_StructuredOp<linalg::FillOp>();
  parallel_fill = m_StructuredOp<linalg::FillOp>();
  trailing = m_StructuredOp<linalg::GenericOp>()
                 .input(AllOperands(), IsPermutation())
                 .output(AllOperands(), IsPermutation())
                 .input(NumEqualsTo(1))
                 .output(NumEqualsTo(1));
  leading = m_StructuredOp<linalg::GenericOp>()
                .input(AllOperands(), IsPermutation())
                .output(AllOperands(), IsPermutation())
                .input(NumEqualsTo(1))
                .output(NumEqualsTo(1));
  parallel_reduction = m_StructuredOp()
                           .dim(AllDims(), ShapeKind::Static)
                           .dim(-1, utils::IteratorType::reduction)
                           .input(AllOperands(), IsPermutation())
                           // TODO: we want to accept any input position here.
                           .input(0, leading, OptionalMatch())
                           .output(NumEqualsTo(1))
                           .output(0, parallel_fill);
  combiner_reduction =
      m_StructuredOp()
          .dim(AllDims(), ShapeKind::Static)
          .dim(-1, utils::IteratorType::reduction)
          // Can be extended to projected permutation with broadcast.
          .input(AllOperands(), IsPermutation())
          .input(0, SubsetOf(parallel_reduction))
          .output(NumEqualsTo(1))
          .output(0, SubsetOf(original_fill))
          .output(0, ElementTypeBitWidth(32))
          .output(0, SingleCombinerReduction())
          .result(0, HasAnyUse(), SubsetOf(trailing), OptionalMatch())
          .allTilableOpsCaptured<func::FuncOp>();
}