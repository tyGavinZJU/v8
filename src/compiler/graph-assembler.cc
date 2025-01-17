// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/graph-assembler.h"

#include "src/codegen/code-factory.h"
#include "src/compiler/linkage.h"
#include "src/compiler/schedule.h"
// For TNode types.
#include "src/objects/heap-number.h"
#include "src/objects/oddball.h"
#include "src/objects/smi.h"
#include "src/objects/string.h"

namespace v8 {
namespace internal {
namespace compiler {

class GraphAssembler::BasicBlockUpdater {
 public:
  BasicBlockUpdater(Schedule* schedule, Graph* graph, Zone* temp_zone);

  Node* AddNode(Node* node);
  Node* AddNode(Node* node, BasicBlock* to);
  Node* AddClonedNode(Node* node);

  BasicBlock* NewBasicBlock(bool deferred);
  BasicBlock* SplitBasicBlock();
  void AddBind(BasicBlock* block);
  void AddBranch(Node* branch, BasicBlock* tblock, BasicBlock* fblock);
  void AddGoto(BasicBlock* to);
  void AddGoto(BasicBlock* from, BasicBlock* to);
  void AddThrow(Node* node);

  void StartBlock(BasicBlock* block);
  BasicBlock* Finalize(BasicBlock* original);

  BasicBlock* original_block() { return original_block_; }
  BasicBlock::Control original_control() { return original_control_; }
  Node* original_control_input() { return original_control_input_; }

 private:
  enum State { kUnchanged, kChanged };

  Zone* temp_zone() { return temp_zone_; }

  bool IsOriginalNode(Node* node);
  void UpdateSuccessors(BasicBlock* block);
  void SetBlockDeferredFromPredecessors();
  void CopyForChange();

  Zone* temp_zone_;

  // Current basic block we are scheduling.
  BasicBlock* current_block_;

  // The original block that we are lowering.
  BasicBlock* original_block_;

  // Position in the current block, only applicable in the 'unchanged' state.
  BasicBlock::iterator node_it_;
  BasicBlock::iterator end_it_;

  Schedule* schedule_;
  Graph* graph_;

  // The nodes in the original block if we are in 'changed' state. Retained to
  // avoid invalidating iterators that are iterating over the original nodes of
  // the block.
  NodeVector saved_nodes_;

  // The original control, control input and successors, to enable recovery of
  // them when we finalize the block.
  struct SuccessorInfo {
    BasicBlock* block;
    size_t index;
  };
  ZoneVector<SuccessorInfo> saved_successors_;
  BasicBlock::Control original_control_;
  Node* original_control_input_;
  bool original_deferred_;
  size_t original_node_count_;

  State state_;
};

GraphAssembler::BasicBlockUpdater::BasicBlockUpdater(Schedule* schedule,
                                                     Graph* graph,
                                                     Zone* temp_zone)
    : temp_zone_(temp_zone),
      current_block_(nullptr),
      original_block_(nullptr),
      schedule_(schedule),
      graph_(graph),
      saved_nodes_(schedule->zone()),
      saved_successors_(schedule->zone()),
      original_control_(BasicBlock::kNone),
      original_control_input_(nullptr),
      original_deferred_(false),
      original_node_count_(graph->NodeCount()),
      state_(kUnchanged) {}

Node* GraphAssembler::BasicBlockUpdater::AddNode(Node* node) {
  return AddNode(node, current_block_);
}

Node* GraphAssembler::BasicBlockUpdater::AddNode(Node* node, BasicBlock* to) {
  if (state_ == kUnchanged) {
    DCHECK_EQ(to, original_block());

    if (node_it_ != end_it_ && *node_it_ == node) {
      node_it_++;
      return node;
    }

    CopyForChange();
  }

  // Add the node to the basic block.
  DCHECK(!schedule_->IsScheduled(node));
  schedule_->AddNode(to, node);
  return node;
}

Node* GraphAssembler::BasicBlockUpdater::AddClonedNode(Node* node) {
  DCHECK(node->op()->HasProperty(Operator::kPure));
  if (state_ == kUnchanged) {
    CopyForChange();
  }

  if (schedule_->IsScheduled(node) &&
      schedule_->block(node) == current_block_) {
    // Node is already scheduled for the current block, don't add it again.
    return node;
  } else if (!schedule_->IsScheduled(node) && !IsOriginalNode(node)) {
    // Node is not scheduled yet, so we can add it directly.
    return AddNode(node);
  } else {
    // TODO(9684): Potentially add some per-block caching so we can avoid
    // cloning if we've already cloned for this block.
    return AddNode(graph_->CloneNode(node));
  }
}

bool GraphAssembler::BasicBlockUpdater::IsOriginalNode(Node* node) {
  // Return true if node was part of the original schedule and might currently
  // be re-added to the schedule after a CopyForChange.
  return node->id() < original_node_count_;
}

void GraphAssembler::BasicBlockUpdater::CopyForChange() {
  DCHECK_EQ(kUnchanged, state_);

  // Save successor.
  DCHECK(saved_successors_.empty());
  for (BasicBlock* successor : original_block()->successors()) {
    for (size_t i = 0; i < successor->PredecessorCount(); i++) {
      if (successor->PredecessorAt(i) == original_block()) {
        saved_successors_.push_back({successor, i});
        break;
      }
    }
  }
  DCHECK_EQ(saved_successors_.size(), original_block()->SuccessorCount());

  // Save control.
  original_control_ = original_block()->control();
  original_control_input_ = original_block()->control_input();

  // Save original nodes (to allow them to continue to be iterated by the user
  // of graph assembler).
  original_block()->nodes()->swap(saved_nodes_);
  DCHECK(original_block()->nodes()->empty());

  // Re-insert the nodes from the front of the block.
  original_block()->InsertNodes(original_block()->begin(), saved_nodes_.begin(),
                                node_it_);

  // Remove the tail from the schedule.
  for (; node_it_ != end_it_; node_it_++) {
    schedule_->SetBlockForNode(nullptr, *node_it_);
  }

  // Reset the control.
  if (original_block()->control() != BasicBlock::kGoto) {
    schedule_->SetBlockForNode(nullptr, original_block()->control_input());
  }
  original_block()->set_control_input(nullptr);
  original_block()->set_control(BasicBlock::kNone);
  original_block()->ClearSuccessors();

  state_ = kChanged;
  end_it_ = {};
  node_it_ = {};
}

BasicBlock* GraphAssembler::BasicBlockUpdater::NewBasicBlock(bool deferred) {
  BasicBlock* block = schedule_->NewBasicBlock();
  block->set_deferred(deferred || original_deferred_);
  return block;
}

BasicBlock* GraphAssembler::BasicBlockUpdater::SplitBasicBlock() {
  return NewBasicBlock(current_block_->deferred());
}

void GraphAssembler::BasicBlockUpdater::AddBind(BasicBlock* to) {
  DCHECK_NOT_NULL(to);
  current_block_ = to;
  // Basic block should only have the control node, if any.
  DCHECK_LE(current_block_->NodeCount(), 1);
  SetBlockDeferredFromPredecessors();
}

void GraphAssembler::BasicBlockUpdater::SetBlockDeferredFromPredecessors() {
  if (!current_block_->deferred()) {
    bool deferred = true;
    for (BasicBlock* pred : current_block_->predecessors()) {
      if (!pred->deferred()) {
        deferred = false;
        break;
      }
    }
    current_block_->set_deferred(deferred);
  }
}

void GraphAssembler::BasicBlockUpdater::AddBranch(Node* node,
                                                  BasicBlock* tblock,
                                                  BasicBlock* fblock) {
  if (state_ == kUnchanged) {
    DCHECK_EQ(current_block_, original_block());
    CopyForChange();
  }

  DCHECK_EQ(state_, kChanged);
  schedule_->AddBranch(current_block_, node, tblock, fblock);
  current_block_ = nullptr;
}

void GraphAssembler::BasicBlockUpdater::AddGoto(BasicBlock* to) {
  DCHECK_NOT_NULL(current_block_);
  AddGoto(current_block_, to);
}

void GraphAssembler::BasicBlockUpdater::AddGoto(BasicBlock* from,
                                                BasicBlock* to) {
  if (state_ == kUnchanged) {
    CopyForChange();
  }

  if (to->deferred() && !from->deferred()) {
    // Add a new block with the correct deferred hint to avoid merges into the
    // target block with different deferred hints.
    // TODO(9684): Only split the current basic block if the label's target
    // block has multiple merges.
    BasicBlock* new_block = NewBasicBlock(to->deferred());
    schedule_->AddGoto(from, new_block);
    from = new_block;
  }

  schedule_->AddGoto(from, to);
  current_block_ = nullptr;
}

void GraphAssembler::BasicBlockUpdater::AddThrow(Node* node) {
  if (state_ == kUnchanged) {
    CopyForChange();
  }
  schedule_->AddThrow(current_block_, node);

  // Clear original successors and replace the block's original control and
  // control input to the throw, since this block is now connected directly to
  // the end.
  if (original_control_input_ != nullptr) {
    NodeProperties::ReplaceUses(original_control_input_, node, nullptr, node);
    original_control_input_->Kill();
  }
  original_control_input_ = node;
  original_control_ = BasicBlock::kThrow;

  for (SuccessorInfo succ : saved_successors_) {
    succ.block->RemovePredecessor(succ.index);
  }
  saved_successors_.clear();
}

void GraphAssembler::BasicBlockUpdater::UpdateSuccessors(BasicBlock* block) {
  for (SuccessorInfo succ : saved_successors_) {
    (succ.block->predecessors())[succ.index] = block;
    block->AddSuccessor(succ.block);
  }
  saved_successors_.clear();
  block->set_control(original_control_);
  block->set_control_input(original_control_input_);
  if (original_control_input_ != nullptr) {
    schedule_->SetBlockForNode(block, original_control_input_);
  } else {
    DCHECK_EQ(BasicBlock::kGoto, original_control_);
  }
}

void GraphAssembler::BasicBlockUpdater::StartBlock(BasicBlock* block) {
  DCHECK_NULL(current_block_);
  DCHECK_NULL(original_block_);
  DCHECK(saved_nodes_.empty());
  block->ResetRPOInfo();
  current_block_ = block;
  original_block_ = block;
  original_deferred_ = block->deferred();
  node_it_ = block->begin();
  end_it_ = block->end();
  state_ = kUnchanged;
}

BasicBlock* GraphAssembler::BasicBlockUpdater::Finalize(BasicBlock* original) {
  DCHECK_EQ(original, original_block());
  BasicBlock* block = current_block_;
  if (state_ == kChanged) {
    UpdateSuccessors(block);
  } else {
    DCHECK_EQ(block, original_block());
    if (node_it_ != end_it_) {
      // We have not got to the end of the node list, we need to trim.
      block->TrimNodes(node_it_);
    }
  }
  original_control_ = BasicBlock::kNone;
  saved_nodes_.clear();
  original_deferred_ = false;
  original_control_input_ = nullptr;
  original_block_ = nullptr;
  current_block_ = nullptr;
  return block;
}

GraphAssembler::GraphAssembler(JSGraph* jsgraph, Zone* zone, Schedule* schedule)
    : temp_zone_(zone),
      jsgraph_(jsgraph),
      effect_(nullptr),
      control_(nullptr),
      block_updater_(schedule != nullptr ? new BasicBlockUpdater(
                                               schedule, jsgraph->graph(), zone)
                                         : nullptr) {}

GraphAssembler::~GraphAssembler() = default;

Node* GraphAssembler::IntPtrConstant(intptr_t value) {
  return AddClonedNode(jsgraph()->IntPtrConstant(value));
}

Node* GraphAssembler::Int32Constant(int32_t value) {
  return AddClonedNode(jsgraph()->Int32Constant(value));
}

Node* GraphAssembler::Int64Constant(int64_t value) {
  return AddClonedNode(jsgraph()->Int64Constant(value));
}

Node* GraphAssembler::UniqueIntPtrConstant(intptr_t value) {
  return AddNode(graph()->NewNode(
      machine()->Is64()
          ? common()->Int64Constant(value)
          : common()->Int32Constant(static_cast<int32_t>(value))));
}

Node* GraphAssembler::SmiConstant(int32_t value) {
  return AddClonedNode(jsgraph()->SmiConstant(value));
}

Node* GraphAssembler::Uint32Constant(uint32_t value) {
  return AddClonedNode(jsgraph()->Uint32Constant(value));
}

Node* GraphAssembler::Float64Constant(double value) {
  return AddClonedNode(jsgraph()->Float64Constant(value));
}

TNode<HeapObject> GraphAssembler::HeapConstant(Handle<HeapObject> object) {
  return TNode<HeapObject>::UncheckedCast(
      AddClonedNode(jsgraph()->HeapConstant(object)));
}

TNode<Object> GraphAssembler::Constant(const ObjectRef& ref) {
  return TNode<Object>::UncheckedCast(AddClonedNode(jsgraph()->Constant(ref)));
}

TNode<Number> GraphAssembler::NumberConstant(double value) {
  return TNode<Number>::UncheckedCast(
      AddClonedNode(jsgraph()->Constant(value)));
}

Node* GraphAssembler::ExternalConstant(ExternalReference ref) {
  return AddClonedNode(jsgraph()->ExternalConstant(ref));
}

Node* GraphAssembler::CEntryStubConstant(int result_size) {
  return AddClonedNode(jsgraph()->CEntryStubConstant(result_size));
}

Node* GraphAssembler::LoadFramePointer() {
  return AddNode(graph()->NewNode(machine()->LoadFramePointer()));
}

#define SINGLETON_CONST_DEF(Name, Type)              \
  TNode<Type> GraphAssembler::Name##Constant() {     \
    return TNode<Type>::UncheckedCast(               \
        AddClonedNode(jsgraph()->Name##Constant())); \
  }
JSGRAPH_SINGLETON_CONSTANT_LIST(SINGLETON_CONST_DEF)
#undef SINGLETON_CONST_DEF

#define SINGLETON_CONST_TEST_DEF(Name, ...)                      \
  TNode<Boolean> GraphAssembler::Is##Name(TNode<Object> value) { \
    return TNode<Boolean>::UncheckedCast(                        \
        ReferenceEqual(value, Name##Constant()));                \
  }
JSGRAPH_SINGLETON_CONSTANT_LIST(SINGLETON_CONST_TEST_DEF)
#undef SINGLETON_CONST_TEST_DEF

#define PURE_UNOP_DEF(Name)                                     \
  Node* GraphAssembler::Name(Node* input) {                     \
    return AddNode(graph()->NewNode(machine()->Name(), input)); \
  }
PURE_ASSEMBLER_MACH_UNOP_LIST(PURE_UNOP_DEF)
#undef PURE_UNOP_DEF

#define PURE_BINOP_DEF(Name)                                          \
  Node* GraphAssembler::Name(Node* left, Node* right) {               \
    return AddNode(graph()->NewNode(machine()->Name(), left, right)); \
  }
PURE_ASSEMBLER_MACH_BINOP_LIST(PURE_BINOP_DEF)
#undef PURE_BINOP_DEF

#define CHECKED_BINOP_DEF(Name)                                       \
  Node* GraphAssembler::Name(Node* left, Node* right) {               \
    return AddNode(                                                   \
        graph()->NewNode(machine()->Name(), left, right, control())); \
  }
CHECKED_ASSEMBLER_MACH_BINOP_LIST(CHECKED_BINOP_DEF)
#undef CHECKED_BINOP_DEF

Node* GraphAssembler::IntPtrEqual(Node* left, Node* right) {
  return WordEqual(left, right);
}

Node* GraphAssembler::TaggedEqual(Node* left, Node* right) {
  if (COMPRESS_POINTERS_BOOL) {
    return Word32Equal(left, right);
  } else {
    return WordEqual(left, right);
  }
}

Node* GraphAssembler::SmiSub(Node* left, Node* right) {
  if (COMPRESS_POINTERS_BOOL) {
    return Int32Sub(left, right);
  } else {
    return IntSub(left, right);
  }
}

Node* GraphAssembler::SmiLessThan(Node* left, Node* right) {
  if (COMPRESS_POINTERS_BOOL) {
    return Int32LessThan(left, right);
  } else {
    return IntLessThan(left, right);
  }
}

Node* GraphAssembler::Float64RoundDown(Node* value) {
  CHECK(machine()->Float64RoundDown().IsSupported());
  return AddNode(graph()->NewNode(machine()->Float64RoundDown().op(), value));
}

Node* GraphAssembler::Float64RoundTruncate(Node* value) {
  CHECK(machine()->Float64RoundTruncate().IsSupported());
  return AddNode(
      graph()->NewNode(machine()->Float64RoundTruncate().op(), value));
}

Node* GraphAssembler::Projection(int index, Node* value) {
  return AddNode(
      graph()->NewNode(common()->Projection(index), value, control()));
}

Node* GraphAssembler::Allocate(AllocationType allocation, Node* size) {
  return AddNode(
      graph()->NewNode(simplified()->AllocateRaw(Type::Any(), allocation), size,
                       effect(), control()));
}

Node* GraphAssembler::LoadField(FieldAccess const& access, Node* object) {
  Node* value = AddNode(graph()->NewNode(simplified()->LoadField(access),
                                         object, effect(), control()));
  return value;
}

Node* GraphAssembler::LoadElement(ElementAccess const& access, Node* object,
                                  Node* index) {
  Node* value = AddNode(graph()->NewNode(simplified()->LoadElement(access),
                                         object, index, effect(), control()));
  return value;
}

Node* GraphAssembler::StoreField(FieldAccess const& access, Node* object,
                                 Node* value) {
  return AddNode(graph()->NewNode(simplified()->StoreField(access), object,
                                  value, effect(), control()));
}

Node* GraphAssembler::StoreElement(ElementAccess const& access, Node* object,
                                   Node* index, Node* value) {
  return AddNode(graph()->NewNode(simplified()->StoreElement(access), object,
                                  index, value, effect(), control()));
}

void GraphAssembler::TransitionAndStoreElement(MapRef double_map,
                                               MapRef fast_map,
                                               TNode<HeapObject> object,
                                               TNode<Number> index,
                                               TNode<Object> value) {
  AddNode(graph()->NewNode(simplified()->TransitionAndStoreElement(
                               double_map.object(), fast_map.object()),
                           object, index, value, effect(), control()));
}

TNode<Number> GraphAssembler::StringLength(TNode<String> string) {
  return AddNode<Number>(
      graph()->NewNode(simplified()->StringLength(), string));
}

TNode<Boolean> GraphAssembler::ReferenceEqual(TNode<Object> lhs,
                                              TNode<Object> rhs) {
  return AddNode<Boolean>(
      graph()->NewNode(simplified()->ReferenceEqual(), lhs, rhs));
}

TNode<Number> GraphAssembler::NumberMin(TNode<Number> lhs, TNode<Number> rhs) {
  return AddNode<Number>(graph()->NewNode(simplified()->NumberMin(), lhs, rhs));
}

TNode<Number> GraphAssembler::NumberMax(TNode<Number> lhs, TNode<Number> rhs) {
  return AddNode<Number>(graph()->NewNode(simplified()->NumberMax(), lhs, rhs));
}

TNode<Number> GraphAssembler::NumberAdd(TNode<Number> lhs, TNode<Number> rhs) {
  return AddNode<Number>(graph()->NewNode(simplified()->NumberAdd(), lhs, rhs));
}

TNode<Number> GraphAssembler::NumberSubtract(TNode<Number> lhs,
                                             TNode<Number> rhs) {
  return AddNode<Number>(
      graph()->NewNode(simplified()->NumberSubtract(), lhs, rhs));
}

TNode<Boolean> GraphAssembler::NumberLessThan(TNode<Number> lhs,
                                              TNode<Number> rhs) {
  return AddNode<Boolean>(
      graph()->NewNode(simplified()->NumberLessThan(), lhs, rhs));
}

TNode<Boolean> GraphAssembler::NumberLessThanOrEqual(TNode<Number> lhs,
                                                     TNode<Number> rhs) {
  return AddNode<Boolean>(
      graph()->NewNode(simplified()->NumberLessThanOrEqual(), lhs, rhs));
}

TNode<String> GraphAssembler::StringSubstring(TNode<String> string,
                                              TNode<Number> from,
                                              TNode<Number> to) {
  return AddNode<String>(graph()->NewNode(
      simplified()->StringSubstring(), string, from, to, effect(), control()));
}

TNode<Boolean> GraphAssembler::ObjectIsCallable(TNode<Object> value) {
  return AddNode<Boolean>(
      graph()->NewNode(simplified()->ObjectIsCallable(), value));
}

Node* GraphAssembler::CheckIf(Node* cond, DeoptimizeReason reason) {
  return AddNode(graph()->NewNode(simplified()->CheckIf(reason), cond, effect(),
                                  control()));
}

TNode<Boolean> GraphAssembler::NumberIsFloat64Hole(TNode<Number> value) {
  return AddNode<Boolean>(
      graph()->NewNode(simplified()->NumberIsFloat64Hole(), value));
}

TNode<Boolean> GraphAssembler::ToBoolean(TNode<Object> value) {
  return AddNode<Boolean>(graph()->NewNode(simplified()->ToBoolean(), value));
}

TNode<FixedArrayBase> GraphAssembler::MaybeGrowFastElements(
    ElementsKind kind, const FeedbackSource& feedback, TNode<JSArray> array,
    TNode<FixedArrayBase> elements, TNode<Number> new_length,
    TNode<Number> old_length) {
  GrowFastElementsMode mode = IsDoubleElementsKind(kind)
                                  ? GrowFastElementsMode::kDoubleElements
                                  : GrowFastElementsMode::kSmiOrObjectElements;
  return AddNode<FixedArrayBase>(graph()->NewNode(
      simplified()->MaybeGrowFastElements(mode, feedback), array, elements,
      new_length, old_length, effect(), control()));
}

Node* GraphAssembler::TypeGuard(Type type, Node* value) {
  return AddNode(
      graph()->NewNode(common()->TypeGuard(type), value, effect(), control()));
}

Node* GraphAssembler::Checkpoint(FrameState frame_state) {
  return AddNode(graph()->NewNode(common()->Checkpoint(), frame_state, effect(),
                                  control()));
}

Node* GraphAssembler::LoopExit(Control loop_header) {
  return AddNode(
      graph()->NewNode(common()->LoopExit(), control(), loop_header));
}

Node* GraphAssembler::LoopExitEffect() {
  return AddNode(
      graph()->NewNode(common()->LoopExitEffect(), effect(), control()));
}

Node* GraphAssembler::DebugBreak() {
  return AddNode(
      graph()->NewNode(machine()->DebugBreak(), effect(), control()));
}

Node* GraphAssembler::Unreachable() {
  return AddNode(
      graph()->NewNode(common()->Unreachable(), effect(), control()));
}

Node* GraphAssembler::Store(StoreRepresentation rep, Node* object, Node* offset,
                            Node* value) {
  return AddNode(graph()->NewNode(machine()->Store(rep), object, offset, value,
                                  effect(), control()));
}

Node* GraphAssembler::Load(MachineType type, Node* object, Node* offset) {
  Node* value = AddNode(graph()->NewNode(machine()->Load(type), object, offset,
                                         effect(), control()));
  return value;
}

Node* GraphAssembler::StoreUnaligned(MachineRepresentation rep, Node* object,
                                     Node* offset, Node* value) {
  Operator const* const op =
      (rep == MachineRepresentation::kWord8 ||
       machine()->UnalignedStoreSupported(rep))
          ? machine()->Store(StoreRepresentation(rep, kNoWriteBarrier))
          : machine()->UnalignedStore(rep);
  return AddNode(
      graph()->NewNode(op, object, offset, value, effect(), control()));
}

Node* GraphAssembler::LoadUnaligned(MachineType type, Node* object,
                                    Node* offset) {
  Operator const* const op =
      (type.representation() == MachineRepresentation::kWord8 ||
       machine()->UnalignedLoadSupported(type.representation()))
          ? machine()->Load(type)
          : machine()->UnalignedLoad(type);
  return AddNode(graph()->NewNode(op, object, offset, effect(), control()));
}

Node* GraphAssembler::Retain(Node* buffer) {
  return AddNode(graph()->NewNode(common()->Retain(), buffer, effect()));
}

Node* GraphAssembler::UnsafePointerAdd(Node* base, Node* external) {
  return AddNode(graph()->NewNode(machine()->UnsafePointerAdd(), base, external,
                                  effect(), control()));
}

TNode<Number> GraphAssembler::ToNumber(TNode<Object> value) {
  return AddNode<Number>(graph()->NewNode(ToNumberOperator(),
                                          ToNumberBuiltinConstant(), value,
                                          NoContextConstant(), effect()));
}

Node* GraphAssembler::BitcastWordToTagged(Node* value) {
  return AddNode(graph()->NewNode(machine()->BitcastWordToTagged(), value,
                                  effect(), control()));
}

Node* GraphAssembler::BitcastTaggedToWord(Node* value) {
  return AddNode(graph()->NewNode(machine()->BitcastTaggedToWord(), value,
                                  effect(), control()));
}

Node* GraphAssembler::BitcastTaggedToWordForTagAndSmiBits(Node* value) {
  return AddNode(
      graph()->NewNode(machine()->BitcastTaggedToWordForTagAndSmiBits(), value,
                       effect(), control()));
}

Node* GraphAssembler::Word32PoisonOnSpeculation(Node* value) {
  return AddNode(graph()->NewNode(machine()->Word32PoisonOnSpeculation(), value,
                                  effect(), control()));
}

Node* GraphAssembler::DeoptimizeIf(DeoptimizeReason reason,
                                   FeedbackSource const& feedback,
                                   Node* condition, Node* frame_state,
                                   IsSafetyCheck is_safety_check) {
  return AddNode(
      graph()->NewNode(common()->DeoptimizeIf(DeoptimizeKind::kEager, reason,
                                              feedback, is_safety_check),
                       condition, frame_state, effect(), control()));
}

Node* GraphAssembler::DeoptimizeIfNot(DeoptimizeReason reason,
                                      FeedbackSource const& feedback,
                                      Node* condition, Node* frame_state,
                                      IsSafetyCheck is_safety_check) {
  return AddNode(graph()->NewNode(
      common()->DeoptimizeUnless(DeoptimizeKind::kEager, reason, feedback,
                                 is_safety_check),
      condition, frame_state, effect(), control()));
}

void GraphAssembler::BranchWithCriticalSafetyCheck(
    Node* condition, GraphAssemblerLabel<0u>* if_true,
    GraphAssemblerLabel<0u>* if_false) {
  BranchHint hint = BranchHint::kNone;
  if (if_true->IsDeferred() != if_false->IsDeferred()) {
    hint = if_false->IsDeferred() ? BranchHint::kTrue : BranchHint::kFalse;
  }

  BranchImpl(condition, if_true, if_false, hint,
             IsSafetyCheck::kCriticalSafetyCheck);
}

void GraphAssembler::RecordBranchInBlockUpdater(Node* branch,
                                                Node* if_true_control,
                                                Node* if_false_control,
                                                BasicBlock* if_true_block,
                                                BasicBlock* if_false_block) {
  DCHECK_NOT_NULL(block_updater_);
  // TODO(9684): Only split the current basic block if the label's target
  // block has multiple merges.
  BasicBlock* if_true_target = block_updater_->SplitBasicBlock();
  BasicBlock* if_false_target = block_updater_->SplitBasicBlock();

  block_updater_->AddBranch(branch, if_true_target, if_false_target);

  block_updater_->AddNode(if_true_control, if_true_target);
  block_updater_->AddGoto(if_true_target, if_true_block);

  block_updater_->AddNode(if_false_control, if_false_target);
  block_updater_->AddGoto(if_false_target, if_false_block);
}

void GraphAssembler::BindBasicBlock(BasicBlock* block) {
  if (block_updater_) {
    block_updater_->AddBind(block);
  }
}

BasicBlock* GraphAssembler::NewBasicBlock(bool deferred) {
  if (!block_updater_) return nullptr;
  return block_updater_->NewBasicBlock(deferred);
}

void GraphAssembler::GotoBasicBlock(BasicBlock* block) {
  if (block_updater_) {
    block_updater_->AddGoto(block);
  }
}

void GraphAssembler::GotoIfBasicBlock(BasicBlock* block, Node* branch,
                                      IrOpcode::Value goto_if) {
  if (block_updater_) {
    // TODO(9684): Only split the current basic block for the goto_target
    // if block has multiple merges.
    BasicBlock* goto_target = block_updater_->SplitBasicBlock();
    BasicBlock* fallthrough_target = block_updater_->SplitBasicBlock();

    if (goto_if == IrOpcode::kIfTrue) {
      block_updater_->AddBranch(branch, goto_target, fallthrough_target);
    } else {
      DCHECK_EQ(goto_if, IrOpcode::kIfFalse);
      block_updater_->AddBranch(branch, fallthrough_target, goto_target);
    }

    block_updater_->AddNode(control(), goto_target);
    block_updater_->AddGoto(goto_target, block);

    block_updater_->AddBind(fallthrough_target);
  }
}

BasicBlock* GraphAssembler::FinalizeCurrentBlock(BasicBlock* block) {
  if (block_updater_) {
    block = block_updater_->Finalize(block);
    if (control() == jsgraph()->Dead()) {
      // If the block's end is unreachable, then reset current effect and
      // control to that of the block's throw control node.
      DCHECK(block->control() == BasicBlock::kThrow);
      Node* throw_node = block->control_input();
      control_ = NodeProperties::GetControlInput(throw_node);
      effect_ = NodeProperties::GetEffectInput(throw_node);
    }
  }
  return block;
}

void GraphAssembler::ConnectUnreachableToEnd() {
  DCHECK_EQ(effect()->opcode(), IrOpcode::kUnreachable);
  Node* throw_node = graph()->NewNode(common()->Throw(), effect(), control());
  NodeProperties::MergeControlToEnd(graph(), common(), throw_node);
  effect_ = control_ = jsgraph()->Dead();
  if (block_updater_) {
    block_updater_->AddThrow(throw_node);
  }
}

Node* GraphAssembler::AddClonedNode(Node* node) {
  DCHECK(node->op()->HasProperty(Operator::kPure));
  if (block_updater_) {
    node = block_updater_->AddClonedNode(node);
  }

  UpdateEffectControlWith(node);
  return node;
}

Node* GraphAssembler::AddNode(Node* node) {
  if (block_updater_) {
    block_updater_->AddNode(node);
  }

  if (node->opcode() == IrOpcode::kTerminate) {
    return node;
  }

  UpdateEffectControlWith(node);
  return node;
}

void GraphAssembler::Reset(BasicBlock* block) {
  effect_ = nullptr;
  control_ = nullptr;
  if (block_updater_) {
    block_updater_->StartBlock(block);
  }
}

void GraphAssembler::InitializeEffectControl(Node* effect, Node* control) {
  effect_ = effect;
  control_ = control;
}

Operator const* GraphAssembler::ToNumberOperator() {
  if (!to_number_operator_.is_set()) {
    Callable callable =
        Builtins::CallableFor(jsgraph()->isolate(), Builtins::kToNumber);
    CallDescriptor::Flags flags = CallDescriptor::kNoFlags;
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        graph()->zone(), callable.descriptor(),
        callable.descriptor().GetStackParameterCount(), flags,
        Operator::kEliminatable);
    to_number_operator_.set(common()->Call(call_descriptor));
  }
  return to_number_operator_.get();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
