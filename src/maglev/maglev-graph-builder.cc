// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-graph-builder.h"

#include "src/common/globals.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/feedback-source.h"
#include "src/compiler/heap-refs.h"
#include "src/compiler/processed-feedback.h"
#include "src/handles/maybe-handles-inl.h"
#include "src/ic/handler-configuration-inl.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-interpreter-frame-state.h"
#include "src/maglev/maglev-ir.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/name-inl.h"
#include "src/objects/property-cell.h"
#include "src/objects/slots-inl.h"

namespace v8 {
namespace internal {

namespace maglev {

namespace {

int LoadSimpleFieldHandler(FieldIndex field_index) {
  int config = LoadHandler::KindBits::encode(LoadHandler::Kind::kField) |
               LoadHandler::IsInobjectBits::encode(field_index.is_inobject()) |
               LoadHandler::IsDoubleBits::encode(field_index.is_double()) |
               LoadHandler::FieldIndexBits::encode(field_index.index());
  return config;
}

}  // namespace

MaglevGraphBuilder::MaglevGraphBuilder(LocalIsolate* local_isolate,
                                       MaglevCompilationUnit* compilation_unit,
                                       Graph* graph, MaglevGraphBuilder* parent)
    : local_isolate_(local_isolate),
      compilation_unit_(compilation_unit),
      parent_(parent),
      graph_(graph),
      iterator_(bytecode().object()),
      // Add an extra jump_target slot for the inline exit if needed.
      jump_targets_(zone()->NewArray<BasicBlockRef>(bytecode().length() +
                                                    (is_inline() ? 1 : 0))),
      // Overallocate merge_states_ by one to allow always looking up the
      // next offset. This overallocated slot can also be used for the inline
      // exit when needed.
      merge_states_(zone()->NewArray<MergePointInterpreterFrameState*>(
          bytecode().length() + 1)),
      current_interpreter_frame_(*compilation_unit_) {
  memset(merge_states_, 0,
         bytecode().length() * sizeof(InterpreterFrameState*));
  // Default construct basic block refs.
  // TODO(leszeks): This could be a memset of nullptr to ..._jump_targets_.
  for (int i = 0; i < bytecode().length(); ++i) {
    new (&jump_targets_[i]) BasicBlockRef();
  }

  if (is_inline()) {
    DCHECK_NOT_NULL(parent_);
    DCHECK_GT(compilation_unit->inlining_depth(), 0);
    // The allocation/initialisation logic here relies on inline_exit_offset
    // being the offset one past the end of the bytecode.
    DCHECK_EQ(inline_exit_offset(), bytecode().length());
    merge_states_[inline_exit_offset()] = nullptr;
    new (&jump_targets_[inline_exit_offset()]) BasicBlockRef();
  }

  CalculatePredecessorCounts();

  for (auto& offset_and_info : bytecode_analysis().GetLoopInfos()) {
    int offset = offset_and_info.first;
    const compiler::LoopInfo& loop_info = offset_and_info.second;

    const compiler::BytecodeLivenessState* liveness =
        bytecode_analysis().GetInLivenessFor(offset);

    merge_states_[offset] = zone()->New<MergePointInterpreterFrameState>(
        *compilation_unit_, offset, NumPredecessors(offset), liveness,
        &loop_info);
  }
}

void MaglevGraphBuilder::StartPrologue() {
  current_block_ = zone()->New<BasicBlock>(nullptr);
  block_offset_ = -1;
}

BasicBlock* MaglevGraphBuilder::EndPrologue() {
  BasicBlock* first_block = CreateBlock<Jump>({}, &jump_targets_[0]);
  MergeIntoFrameState(first_block, 0);
  return first_block;
}

void MaglevGraphBuilder::SetArgument(int i, ValueNode* value) {
  interpreter::Register reg = interpreter::Register::FromParameterIndex(i);
  current_interpreter_frame_.set(reg, value);
}

void MaglevGraphBuilder::BuildRegisterFrameInitialization() {
  // TODO(leszeks): Extract out a separate "incoming context/closure" nodes,
  // to be able to read in the machine register but also use the frame-spilled
  // slot.
  interpreter::Register regs[] = {interpreter::Register::current_context(),
                                  interpreter::Register::function_closure()};
  for (interpreter::Register& reg : regs) {
    current_interpreter_frame_.set(reg, AddNewNode<InitialValue>({}, reg));
  }

  interpreter::Register new_target_or_generator_register =
      bytecode().incoming_new_target_or_generator_register();

  int register_index = 0;
  // TODO(leszeks): Don't emit if not needed.
  ValueNode* undefined_value =
      AddNewNode<RootConstant>({}, RootIndex::kUndefinedValue);
  if (new_target_or_generator_register.is_valid()) {
    int new_target_index = new_target_or_generator_register.index();
    for (; register_index < new_target_index; register_index++) {
      StoreRegister(interpreter::Register(register_index), undefined_value);
    }
    StoreRegister(
        new_target_or_generator_register,
        // TODO(leszeks): Expose in Graph.
        AddNewNode<RegisterInput>({}, kJavaScriptCallNewTargetRegister));
    register_index++;
  }
  for (; register_index < register_count(); register_index++) {
    StoreRegister(interpreter::Register(register_index), undefined_value);
  }
}

// TODO(v8:7700): Clean up after all bytecodes are supported.
#define MAGLEV_UNIMPLEMENTED(BytecodeName)                              \
  do {                                                                  \
    std::cerr << "Maglev: Can't compile "                               \
              << Brief(*compilation_unit_->function().object())         \
              << ", bytecode " #BytecodeName " is not supported\n";     \
    found_unsupported_bytecode_ = true;                                 \
    this_field_will_be_unused_once_all_bytecodes_are_supported_ = true; \
  } while (false)

#define MAGLEV_UNIMPLEMENTED_BYTECODE(Name) \
  void MaglevGraphBuilder::Visit##Name() { MAGLEV_UNIMPLEMENTED(Name); }

namespace {
template <Operation kOperation>
struct NodeForOperationHelper;

#define NODE_FOR_OPERATION_HELPER(Name)               \
  template <>                                         \
  struct NodeForOperationHelper<Operation::k##Name> { \
    using generic_type = Generic##Name;               \
  };
OPERATION_LIST(NODE_FOR_OPERATION_HELPER)
#undef NODE_FOR_OPERATION_HELPER

template <Operation kOperation>
using GenericNodeForOperation =
    typename NodeForOperationHelper<kOperation>::generic_type;
}  // namespace

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericUnaryOperationNode() {
  FeedbackSlot slot_index = GetSlotOperand(0);
  ValueNode* value = GetAccumulatorTagged();
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {value}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericBinaryOperationNode() {
  ValueNode* left = LoadRegisterTagged(0);
  ValueNode* right = GetAccumulatorTagged();
  FeedbackSlot slot_index = GetSlotOperand(1);
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {left, right}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericBinarySmiOperationNode() {
  ValueNode* left = GetAccumulatorTagged();
  Smi constant = Smi::FromInt(iterator_.GetImmediateOperand(0));
  ValueNode* right = AddNewNode<SmiConstant>({}, constant);
  FeedbackSlot slot_index = GetSlotOperand(1);
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {left, right}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitUnaryOperation() {
  // TODO(victorgomes): Use feedback info and create optimized versions.
  BuildGenericUnaryOperationNode<kOperation>();
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitBinaryOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(1);

  if (nexus.ic_state() == InlineCacheState::MONOMORPHIC) {
    if (nexus.kind() == FeedbackSlotKind::kBinaryOp) {
      BinaryOperationHint hint = nexus.GetBinaryOperationFeedback();

      if (hint == BinaryOperationHint::kSignedSmall) {
        ValueNode *left, *right;
        if (IsRegisterEqualToAccumulator(0)) {
          left = right = LoadRegisterInt32(0);
        } else {
          left = LoadRegisterInt32(0);
          right = GetAccumulatorInt32();
        }

        if (kOperation == Operation::kAdd) {
          SetAccumulator(AddNewNode<Int32AddWithOverflow>({left, right}));
          return;
        }
      }
    }
  }

  // TODO(victorgomes): Use feedback info and create optimized versions.
  BuildGenericBinaryOperationNode<kOperation>();
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitBinarySmiOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(1);

  if (nexus.ic_state() == InlineCacheState::MONOMORPHIC) {
    if (nexus.kind() == FeedbackSlotKind::kBinaryOp) {
      BinaryOperationHint hint = nexus.GetBinaryOperationFeedback();

      if (hint == BinaryOperationHint::kSignedSmall) {
        ValueNode* left = GetAccumulatorInt32();
        int32_t constant = iterator_.GetImmediateOperand(0);

        if (kOperation == Operation::kAdd) {
          if (constant == 0) {
            // For addition of zero, when the accumulator passed the Smi check,
            // it already has the right value, so we can just return.
            return;
          }
          // TODO(victorgomes): We could create an Int32Add node that receives
          // a constant and avoid a register move.
          ValueNode* right = AddNewNode<Int32Constant>({}, constant);
          SetAccumulator(AddNewNode<Int32AddWithOverflow>({left, right}));
          return;
        }
      }
    }
  }

  // TODO(victorgomes): Use feedback info and create optimized versions.
  BuildGenericBinarySmiOperationNode<kOperation>();
}

void MaglevGraphBuilder::VisitLdar() {
  MoveNodeBetweenRegisters(iterator_.GetRegisterOperand(0),
                           interpreter::Register::virtual_accumulator());
}

void MaglevGraphBuilder::VisitLdaZero() {
  SetAccumulator(AddNewNode<SmiConstant>({}, Smi::zero()));
}
void MaglevGraphBuilder::VisitLdaSmi() {
  Smi constant = Smi::FromInt(iterator_.GetImmediateOperand(0));
  SetAccumulator(AddNewNode<SmiConstant>({}, constant));
}
void MaglevGraphBuilder::VisitLdaUndefined() {
  SetAccumulator(AddNewNode<RootConstant>({}, RootIndex::kUndefinedValue));
}
void MaglevGraphBuilder::VisitLdaNull() {
  SetAccumulator(AddNewNode<RootConstant>({}, RootIndex::kNullValue));
}
void MaglevGraphBuilder::VisitLdaTheHole() {
  SetAccumulator(AddNewNode<RootConstant>({}, RootIndex::kTheHoleValue));
}
void MaglevGraphBuilder::VisitLdaTrue() {
  SetAccumulator(AddNewNode<RootConstant>({}, RootIndex::kTrueValue));
}
void MaglevGraphBuilder::VisitLdaFalse() {
  SetAccumulator(AddNewNode<RootConstant>({}, RootIndex::kFalseValue));
}
void MaglevGraphBuilder::VisitLdaConstant() {
  SetAccumulator(GetConstant(GetRefOperand<HeapObject>(0)));
}

void MaglevGraphBuilder::VisitLdaContextSlot() {
  ValueNode* context = LoadRegisterTagged(0);
  int slot_index = iterator_.GetIndexOperand(1);
  int depth = iterator_.GetUnsignedImmediateOperand(2);

  for (int i = 0; i < depth; ++i) {
    context = AddNewNode<LoadField>(
        {context}, LoadSimpleFieldHandler(FieldIndex::ForInObjectOffset(
                       Context::OffsetOfElementAt(Context::PREVIOUS_INDEX),
                       FieldIndex::kTagged)));
  }

  // TODO(leszeks): Passing a LoadHandler to LoadField here is a bit of
  // a hack, maybe we should have a LoadRawOffset or similar.
  SetAccumulator(AddNewNode<LoadField>(
      {context},
      LoadSimpleFieldHandler(FieldIndex::ForInObjectOffset(
          Context::OffsetOfElementAt(slot_index), FieldIndex::kTagged))));
}
void MaglevGraphBuilder::VisitLdaImmutableContextSlot() {
  // TODO(leszeks): Consider context specialising.
  VisitLdaCurrentContextSlot();
}
void MaglevGraphBuilder::VisitLdaCurrentContextSlot() {
  ValueNode* context = GetContext();
  int slot_index = iterator_.GetIndexOperand(0);

  // TODO(leszeks): Passing a LoadHandler to LoadField here is a bit of
  // a hack, maybe we should have a LoadRawOffset or similar.
  SetAccumulator(AddNewNode<LoadField>(
      {context},
      LoadSimpleFieldHandler(FieldIndex::ForInObjectOffset(
          Context::OffsetOfElementAt(slot_index), FieldIndex::kTagged))));
}
void MaglevGraphBuilder::VisitLdaImmutableCurrentContextSlot() {
  // TODO(leszeks): Consider context specialising.
  VisitLdaCurrentContextSlot();
}

void MaglevGraphBuilder::VisitStar() {
  MoveNodeBetweenRegisters(interpreter::Register::virtual_accumulator(),
                           iterator_.GetRegisterOperand(0));
}
#define SHORT_STAR_VISITOR(Name, ...)                                          \
  void MaglevGraphBuilder::Visit##Name() {                                     \
    MoveNodeBetweenRegisters(                                                  \
        interpreter::Register::virtual_accumulator(),                          \
        interpreter::Register::FromShortStar(interpreter::Bytecode::k##Name)); \
  }
SHORT_STAR_BYTECODE_LIST(SHORT_STAR_VISITOR)
#undef SHORT_STAR_VISITOR

void MaglevGraphBuilder::VisitMov() {
  MoveNodeBetweenRegisters(iterator_.GetRegisterOperand(0),
                           iterator_.GetRegisterOperand(1));
}
MAGLEV_UNIMPLEMENTED_BYTECODE(PushContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(PopContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestReferenceEqual)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestUndetectable)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestNull)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestUndefined)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestTypeOf)

void MaglevGraphBuilder::BuildPropertyCellAccess(
    const compiler::PropertyCellRef& property_cell) {
  // TODO(leszeks): A bunch of this is copied from
  // js-native-context-specialization.cc -- I wonder if we can unify it
  // somehow.
  bool was_cached = property_cell.Cache();
  CHECK(was_cached);

  compiler::ObjectRef property_cell_value = property_cell.value();
  if (property_cell_value.IsTheHole()) {
    // The property cell is no longer valid.
    EmitUnconditionalDeopt();
    return;
  }

  PropertyDetails property_details = property_cell.property_details();
  PropertyCellType property_cell_type = property_details.cell_type();
  DCHECK_EQ(PropertyKind::kData, property_details.kind());

  if (!property_details.IsConfigurable() && property_details.IsReadOnly()) {
    SetAccumulator(GetConstant(property_cell_value));
    return;
  }

  // Record a code dependency on the cell if we can benefit from the
  // additional feedback, or the global property is configurable (i.e.
  // can be deleted or reconfigured to an accessor property).
  if (property_cell_type != PropertyCellType::kMutable ||
      property_details.IsConfigurable()) {
    broker()->dependencies()->DependOnGlobalProperty(property_cell);
  }

  // Load from constant/undefined global property can be constant-folded.
  if (property_cell_type == PropertyCellType::kConstant ||
      property_cell_type == PropertyCellType::kUndefined) {
    SetAccumulator(GetConstant(property_cell_value));
    return;
  }

  ValueNode* property_cell_node =
      AddNewNode<Constant>({}, property_cell.AsHeapObject());
  // TODO(leszeks): Padding a LoadHandler to LoadField here is a bit of
  // a hack, maybe we should have a LoadRawOffset or similar.
  SetAccumulator(AddNewNode<LoadField>(
      {property_cell_node},
      LoadSimpleFieldHandler(FieldIndex::ForInObjectOffset(
          PropertyCell::kValueOffset, FieldIndex::kTagged))));
}

void MaglevGraphBuilder::VisitLdaGlobal() {
  // LdaGlobal <name_index> <slot>

  static const int kNameOperandIndex = 0;
  static const int kSlotOperandIndex = 1;

  compiler::NameRef name = GetRefOperand<Name>(kNameOperandIndex);
  const compiler::ProcessedFeedback& access_feedback =
      broker()->GetFeedbackForGlobalAccess(compiler::FeedbackSource(
          feedback(), GetSlotOperand(kSlotOperandIndex)));

  if (access_feedback.IsInsufficient()) {
    EmitUnconditionalDeopt();
    return;
  }

  const compiler::GlobalAccessFeedback& global_access_feedback =
      access_feedback.AsGlobalAccess();

  if (global_access_feedback.IsPropertyCell()) {
    BuildPropertyCellAccess(global_access_feedback.property_cell());
  } else {
    // TODO(leszeks): Handle the IsScriptContextSlot case.

    ValueNode* context = GetContext();
    SetAccumulator(AddNewNode<LoadGlobal>({context}, name));
  }
}
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaGlobalInsideTypeof)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaGlobal)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaContextSlot)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaCurrentContextSlot)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupSlot)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupContextSlot)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupGlobalSlot)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupSlotInsideTypeof)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupContextSlotInsideTypeof)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaLookupGlobalSlotInsideTypeof)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaLookupSlot)
void MaglevGraphBuilder::VisitGetNamedProperty() {
  // GetNamedProperty <object> <name_index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(feedback_source,
                                             compiler::AccessMode::kLoad, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      EmitUnconditionalDeopt();
      return;

    case compiler::ProcessedFeedback::kNamedAccess: {
      const compiler::NamedAccessFeedback& named_feedback =
          processed_feedback.AsNamedAccess();
      if (named_feedback.maps().size() == 1) {
        // Monomorphic load, check the handler.
        // TODO(leszeks): Make GetFeedbackForPropertyAccess read the handler.
        MaybeObjectHandle handler =
            FeedbackNexusForSlot(slot).FindHandlerForMap(
                named_feedback.maps()[0].object());
        if (!handler.is_null() && handler->IsSmi()) {
          // Smi handler, emit a map check and LoadField.
          int smi_handler = handler->ToSmi().value();
          LoadHandler::Kind kind = LoadHandler::KindBits::decode(smi_handler);
          if (kind == LoadHandler::Kind::kField &&
              !LoadHandler::IsWasmStructBits::decode(smi_handler)) {
            AddNewNode<CheckMaps>({object}, named_feedback.maps()[0]);
            SetAccumulator(AddNewNode<LoadField>({object}, smi_handler));
            return;
          }
        }
      }
    } break;

    default:
      break;
  }

  // Create a generic load in the fallthrough.
  ValueNode* context = GetContext();
  SetAccumulator(
      AddNewNode<LoadNamedGeneric>({context, object}, name, feedback_source));
}

MAGLEV_UNIMPLEMENTED_BYTECODE(GetNamedPropertyFromSuper)
MAGLEV_UNIMPLEMENTED_BYTECODE(GetKeyedProperty)
MAGLEV_UNIMPLEMENTED_BYTECODE(LdaModuleVariable)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaModuleVariable)

void MaglevGraphBuilder::VisitSetNamedProperty() {
  // SetNamedProperty <object> <name_index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kStore, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      EmitUnconditionalDeopt();
      return;

    case compiler::ProcessedFeedback::kNamedAccess: {
      const compiler::NamedAccessFeedback& named_feedback =
          processed_feedback.AsNamedAccess();
      if (named_feedback.maps().size() == 1) {
        // Monomorphic store, check the handler.
        // TODO(leszeks): Make GetFeedbackForPropertyAccess read the handler.
        MaybeObjectHandle handler =
            FeedbackNexusForSlot(slot).FindHandlerForMap(
                named_feedback.maps()[0].object());
        if (!handler.is_null() && handler->IsSmi()) {
          int smi_handler = handler->ToSmi().value();
          StoreHandler::Kind kind = StoreHandler::KindBits::decode(smi_handler);
          if (kind == StoreHandler::Kind::kField) {
            AddNewNode<CheckMaps>({object}, named_feedback.maps()[0]);
            ValueNode* value = GetAccumulatorTagged();
            AddNewNode<StoreField>({object, value}, smi_handler);
            return;
          }
        }
      }
    } break;

    default:
      break;
  }

  // TODO(victorgomes): Generic store.
  MAGLEV_UNIMPLEMENTED(VisitSetNamedProperty);
}

MAGLEV_UNIMPLEMENTED_BYTECODE(DefineNamedOwnProperty)
MAGLEV_UNIMPLEMENTED_BYTECODE(SetKeyedProperty)
MAGLEV_UNIMPLEMENTED_BYTECODE(DefineKeyedOwnProperty)
MAGLEV_UNIMPLEMENTED_BYTECODE(StaInArrayLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(DefineKeyedOwnPropertyInLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CollectTypeProfile)

void MaglevGraphBuilder::VisitAdd() { VisitBinaryOperation<Operation::kAdd>(); }
void MaglevGraphBuilder::VisitSub() {
  VisitBinaryOperation<Operation::kSubtract>();
}
void MaglevGraphBuilder::VisitMul() {
  VisitBinaryOperation<Operation::kMultiply>();
}
void MaglevGraphBuilder::VisitDiv() {
  VisitBinaryOperation<Operation::kDivide>();
}
void MaglevGraphBuilder::VisitMod() {
  VisitBinaryOperation<Operation::kModulus>();
}
void MaglevGraphBuilder::VisitExp() {
  VisitBinaryOperation<Operation::kExponentiate>();
}
void MaglevGraphBuilder::VisitBitwiseOr() {
  VisitBinaryOperation<Operation::kBitwiseOr>();
}
void MaglevGraphBuilder::VisitBitwiseXor() {
  VisitBinaryOperation<Operation::kBitwiseXor>();
}
void MaglevGraphBuilder::VisitBitwiseAnd() {
  VisitBinaryOperation<Operation::kBitwiseAnd>();
}
void MaglevGraphBuilder::VisitShiftLeft() {
  VisitBinaryOperation<Operation::kShiftLeft>();
}
void MaglevGraphBuilder::VisitShiftRight() {
  VisitBinaryOperation<Operation::kShiftRight>();
}
void MaglevGraphBuilder::VisitShiftRightLogical() {
  VisitBinaryOperation<Operation::kShiftRightLogical>();
}

void MaglevGraphBuilder::VisitAddSmi() {
  VisitBinarySmiOperation<Operation::kAdd>();
}
void MaglevGraphBuilder::VisitSubSmi() {
  VisitBinarySmiOperation<Operation::kSubtract>();
}
void MaglevGraphBuilder::VisitMulSmi() {
  VisitBinarySmiOperation<Operation::kMultiply>();
}
void MaglevGraphBuilder::VisitDivSmi() {
  VisitBinarySmiOperation<Operation::kDivide>();
}
void MaglevGraphBuilder::VisitModSmi() {
  VisitBinarySmiOperation<Operation::kModulus>();
}
void MaglevGraphBuilder::VisitExpSmi() {
  VisitBinarySmiOperation<Operation::kExponentiate>();
}
void MaglevGraphBuilder::VisitBitwiseOrSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseOr>();
}
void MaglevGraphBuilder::VisitBitwiseXorSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseXor>();
}
void MaglevGraphBuilder::VisitBitwiseAndSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseAnd>();
}
void MaglevGraphBuilder::VisitShiftLeftSmi() {
  VisitBinarySmiOperation<Operation::kShiftLeft>();
}
void MaglevGraphBuilder::VisitShiftRightSmi() {
  VisitBinarySmiOperation<Operation::kShiftRight>();
}
void MaglevGraphBuilder::VisitShiftRightLogicalSmi() {
  VisitBinarySmiOperation<Operation::kShiftRightLogical>();
}

void MaglevGraphBuilder::VisitInc() {
  VisitUnaryOperation<Operation::kIncrement>();
}
void MaglevGraphBuilder::VisitDec() {
  VisitUnaryOperation<Operation::kDecrement>();
}
void MaglevGraphBuilder::VisitNegate() {
  VisitUnaryOperation<Operation::kNegate>();
}
void MaglevGraphBuilder::VisitBitwiseNot() {
  VisitUnaryOperation<Operation::kBitwiseNot>();
}

MAGLEV_UNIMPLEMENTED_BYTECODE(ToBooleanLogicalNot)
MAGLEV_UNIMPLEMENTED_BYTECODE(LogicalNot)
MAGLEV_UNIMPLEMENTED_BYTECODE(TypeOf)
MAGLEV_UNIMPLEMENTED_BYTECODE(DeletePropertyStrict)
MAGLEV_UNIMPLEMENTED_BYTECODE(DeletePropertySloppy)
MAGLEV_UNIMPLEMENTED_BYTECODE(GetSuperConstructor)

void MaglevGraphBuilder::InlineCallFromRegisters(
    int argc_count, ConvertReceiverMode receiver_mode,
    compiler::JSFunctionRef function) {
  // The undefined constant node has to be created before the inner graph is
  // created.
  RootConstant* undefined_constant;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    undefined_constant =
        AddNewNode<RootConstant>({}, RootIndex::kUndefinedValue);
  }

  // Create a new compilation unit and graph builder for the inlined
  // function.
  MaglevCompilationUnit* inner_unit =
      MaglevCompilationUnit::NewInner(zone(), compilation_unit_, function);
  MaglevGraphBuilder inner_graph_builder(local_isolate_, inner_unit, graph_,
                                         this);

  // Finish the current block with a jump to the inlined function.
  BasicBlockRef start_ref, end_ref;
  BasicBlock* block = CreateBlock<JumpToInlined>({}, &start_ref, inner_unit);
  ResolveJumpsToBlockAtOffset(block, block_offset_);

  // Manually create the prologue of the inner function graph, so that we
  // can manually set up the arguments.
  inner_graph_builder.StartPrologue();

  int arg_index = 0;
  int reg_count;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    reg_count = argc_count;
    if (function.shared().language_mode() == LanguageMode::kSloppy) {
      // TODO(leszeks): Store the global proxy somehow.
      inner_graph_builder.SetArgument(arg_index++, undefined_constant);
    } else {
      inner_graph_builder.SetArgument(arg_index++, undefined_constant);
    }
  } else {
    reg_count = argc_count + 1;
  }
  for (int i = 0; i < reg_count && i < inner_unit->parameter_count(); i++) {
    inner_graph_builder.SetArgument(arg_index++, LoadRegisterTagged(i + 1));
  }
  for (; arg_index < inner_unit->parameter_count(); arg_index++) {
    inner_graph_builder.SetArgument(arg_index, undefined_constant);
  }
  // TODO(leszeks): Also correctly set up the closure and context slots, instead
  // of using InitialValue.
  inner_graph_builder.BuildRegisterFrameInitialization();
  BasicBlock* inlined_prologue = inner_graph_builder.EndPrologue();

  // Set the entry JumpToInlined to jump to the prologue block.
  // TODO(leszeks): Passing start_ref to JumpToInlined creates a two-element
  // linked list of refs. Consider adding a helper to explicitly set the target
  // instead.
  start_ref.SetToBlockAndReturnNext(inlined_prologue)
      ->SetToBlockAndReturnNext(inlined_prologue);

  // Build the inlined function body.
  inner_graph_builder.BuildBody();

  // All returns in the inlined body jump to a merge point one past the
  // bytecode length (i.e. at offset bytecode.length()). Create a block at
  // this fake offset and have it jump out of the inlined function, into a new
  // block that we create which resumes execution of the outer function.
  // TODO(leszeks): Wrap this up in a helper.
  DCHECK_NULL(inner_graph_builder.current_block_);
  inner_graph_builder.ProcessMergePoint(
      inner_graph_builder.inline_exit_offset());
  inner_graph_builder.StartNewBlock(inner_graph_builder.inline_exit_offset());
  BasicBlock* end_block =
      inner_graph_builder.CreateBlock<JumpFromInlined>({}, &end_ref);
  inner_graph_builder.ResolveJumpsToBlockAtOffset(
      end_block, inner_graph_builder.inline_exit_offset());

  // Pull the returned accumulator value out of the inlined function's final
  // merged return state.
  current_interpreter_frame_.set_accumulator(
      inner_graph_builder.current_interpreter_frame_.accumulator());

  // Create a new block at our current offset, and resume execution. Do this
  // manually to avoid trying to resolve any merges to this offset, which will
  // have already been processed on entry to this visitor.
  current_block_ =
      zone()->New<BasicBlock>(zone()->New<MergePointInterpreterFrameState>(
          *compilation_unit_, current_interpreter_frame_,
          iterator_.current_offset(), 1, block, GetInLiveness()));
  block_offset_ = iterator_.current_offset();
  // Set the exit JumpFromInlined to jump to this resume block.
  // TODO(leszeks): Passing start_ref to JumpFromInlined creates a two-element
  // linked list of refs. Consider adding a helper to explicitly set the target
  // instead.
  end_ref.SetToBlockAndReturnNext(current_block_)
      ->SetToBlockAndReturnNext(current_block_);
}

// TODO(v8:7700): Read feedback and implement inlining
void MaglevGraphBuilder::BuildCallFromRegisterList(
    ConvertReceiverMode receiver_mode) {
  ValueNode* function = LoadRegisterTagged(0);

  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  ValueNode* context = GetContext();

  size_t input_count = args.register_count() + Call::kFixedInputCount;

  RootConstant* undefined_constant;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    // The undefined constant node has to be created before the call node.
    undefined_constant =
        AddNewNode<RootConstant>({}, RootIndex::kUndefinedValue);
    input_count++;
  }

  Call* call = AddNewNode<Call>(input_count, receiver_mode, function, context);
  int arg_index = 0;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    call->set_arg(arg_index++, undefined_constant);
  }
  for (int i = 0; i < args.register_count(); ++i) {
    call->set_arg(arg_index++, current_interpreter_frame_.get(args[i]));
  }

  SetAccumulator(call);
}

void MaglevGraphBuilder::BuildCallFromRegisters(
    int argc_count, ConvertReceiverMode receiver_mode) {
  // Indices and counts of operands on the bytecode.
  const int kFirstArgumentOperandIndex = 1;
  const int kReceiverOperandCount =
      (receiver_mode == ConvertReceiverMode::kNullOrUndefined) ? 0 : 1;
  const int kReceiverAndArgOperandCount = kReceiverOperandCount + argc_count;
  const int kSlotOperandIndex =
      kFirstArgumentOperandIndex + kReceiverAndArgOperandCount;

  DCHECK_LE(argc_count, 2);
  ValueNode* function = LoadRegisterTagged(0);
  ValueNode* context = GetContext();
  FeedbackSlot slot = GetSlotOperand(kSlotOperandIndex);

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForCall(compiler::FeedbackSource(feedback(), slot));
  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      EmitUnconditionalDeopt();
      return;

    case compiler::ProcessedFeedback::kCall: {
      if (!FLAG_maglev_inlining) break;

      const compiler::CallFeedback& call_feedback = processed_feedback.AsCall();
      CallFeedbackContent content = call_feedback.call_feedback_content();
      if (content != CallFeedbackContent::kTarget) break;

      base::Optional<compiler::HeapObjectRef> maybe_target =
          call_feedback.target();
      if (!maybe_target.has_value()) break;

      compiler::HeapObjectRef target = maybe_target.value();
      if (!target.IsJSFunction()) break;

      compiler::JSFunctionRef function = target.AsJSFunction();
      base::Optional<compiler::FeedbackVectorRef> maybe_feedback_vector =
          function.feedback_vector(broker()->dependencies());
      if (!maybe_feedback_vector.has_value()) break;

      return InlineCallFromRegisters(argc_count, receiver_mode, function);
    }

    default:
      break;
  }
  // On fallthrough, create a generic call.

  int argc_count_with_recv = argc_count + 1;
  size_t input_count = argc_count_with_recv + Call::kFixedInputCount;

  // The undefined constant node has to be created before the call node.
  RootConstant* undefined_constant;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    undefined_constant =
        AddNewNode<RootConstant>({}, RootIndex::kUndefinedValue);
  }

  Call* call = AddNewNode<Call>(input_count, receiver_mode, function, context);
  int arg_index = 0;
  int reg_count = argc_count_with_recv;
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    reg_count = argc_count;
    call->set_arg(arg_index++, undefined_constant);
  }
  for (int i = 0; i < reg_count; i++) {
    call->set_arg(arg_index++, LoadRegisterTagged(i + 1));
  }

  SetAccumulator(call);
}

void MaglevGraphBuilder::VisitCallAnyReceiver() {
  BuildCallFromRegisterList(ConvertReceiverMode::kAny);
}
void MaglevGraphBuilder::VisitCallProperty() {
  BuildCallFromRegisterList(ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty0() {
  BuildCallFromRegisters(0, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty1() {
  BuildCallFromRegisters(1, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty2() {
  BuildCallFromRegisters(2, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver() {
  BuildCallFromRegisterList(ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver0() {
  BuildCallFromRegisters(0, ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver1() {
  BuildCallFromRegisters(1, ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver2() {
  BuildCallFromRegisters(2, ConvertReceiverMode::kNullOrUndefined);
}

MAGLEV_UNIMPLEMENTED_BYTECODE(CallWithSpread)
MAGLEV_UNIMPLEMENTED_BYTECODE(CallRuntime)
MAGLEV_UNIMPLEMENTED_BYTECODE(CallRuntimeForPair)
MAGLEV_UNIMPLEMENTED_BYTECODE(CallJSRuntime)
MAGLEV_UNIMPLEMENTED_BYTECODE(InvokeIntrinsic)
MAGLEV_UNIMPLEMENTED_BYTECODE(Construct)
MAGLEV_UNIMPLEMENTED_BYTECODE(ConstructWithSpread)

void MaglevGraphBuilder::VisitTestEqual() {
  VisitBinaryOperation<Operation::kEqual>();
}
void MaglevGraphBuilder::VisitTestEqualStrict() {
  VisitBinaryOperation<Operation::kStrictEqual>();
}
void MaglevGraphBuilder::VisitTestLessThan() {
  VisitBinaryOperation<Operation::kLessThan>();
}
void MaglevGraphBuilder::VisitTestLessThanOrEqual() {
  VisitBinaryOperation<Operation::kLessThanOrEqual>();
}
void MaglevGraphBuilder::VisitTestGreaterThan() {
  VisitBinaryOperation<Operation::kGreaterThan>();
}
void MaglevGraphBuilder::VisitTestGreaterThanOrEqual() {
  VisitBinaryOperation<Operation::kGreaterThanOrEqual>();
}

MAGLEV_UNIMPLEMENTED_BYTECODE(TestInstanceOf)
MAGLEV_UNIMPLEMENTED_BYTECODE(TestIn)
MAGLEV_UNIMPLEMENTED_BYTECODE(ToName)
MAGLEV_UNIMPLEMENTED_BYTECODE(ToNumber)
MAGLEV_UNIMPLEMENTED_BYTECODE(ToNumeric)
MAGLEV_UNIMPLEMENTED_BYTECODE(ToObject)
MAGLEV_UNIMPLEMENTED_BYTECODE(ToString)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateRegExpLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateArrayLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateArrayFromIterable)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateEmptyArrayLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateObjectLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateEmptyObjectLiteral)
MAGLEV_UNIMPLEMENTED_BYTECODE(CloneObject)
MAGLEV_UNIMPLEMENTED_BYTECODE(GetTemplateObject)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateClosure)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateBlockContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateCatchContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateFunctionContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateEvalContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateWithContext)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateMappedArguments)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateUnmappedArguments)
MAGLEV_UNIMPLEMENTED_BYTECODE(CreateRestParameter)

void MaglevGraphBuilder::VisitJumpLoop() {
  int target = iterator_.GetJumpTargetOffset();
  BasicBlock* block =
      target == iterator_.current_offset()
          ? FinishBlock<JumpLoop>(next_offset(), {}, &jump_targets_[target])
          : FinishBlock<JumpLoop>(next_offset(), {},
                                  jump_targets_[target].block_ptr());

  merge_states_[target]->MergeLoop(*compilation_unit_,
                                   current_interpreter_frame_, block, target);
  block->set_predecessor_id(0);
}
void MaglevGraphBuilder::VisitJump() {
  BasicBlock* block = FinishBlock<Jump>(
      next_offset(), {}, &jump_targets_[iterator_.GetJumpTargetOffset()]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
  DCHECK_LT(next_offset(), bytecode().length());
}
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpConstant)
void MaglevGraphBuilder::VisitJumpIfNullConstant() { VisitJumpIfNull(); }
void MaglevGraphBuilder::VisitJumpIfNotNullConstant() { VisitJumpIfNotNull(); }
void MaglevGraphBuilder::VisitJumpIfUndefinedConstant() {
  VisitJumpIfUndefined();
}
void MaglevGraphBuilder::VisitJumpIfNotUndefinedConstant() {
  VisitJumpIfNotUndefined();
}
void MaglevGraphBuilder::VisitJumpIfUndefinedOrNullConstant() {
  VisitJumpIfUndefinedOrNull();
}
void MaglevGraphBuilder::VisitJumpIfTrueConstant() { VisitJumpIfTrue(); }
void MaglevGraphBuilder::VisitJumpIfFalseConstant() { VisitJumpIfFalse(); }
void MaglevGraphBuilder::VisitJumpIfJSReceiverConstant() {
  VisitJumpIfJSReceiver();
}
void MaglevGraphBuilder::VisitJumpIfToBooleanTrueConstant() {
  VisitJumpIfToBooleanTrue();
}
void MaglevGraphBuilder::VisitJumpIfToBooleanFalseConstant() {
  VisitJumpIfToBooleanFalse();
}

void MaglevGraphBuilder::MergeIntoFrameState(BasicBlock* predecessor,
                                             int target) {
  if (merge_states_[target] == nullptr) {
    DCHECK(!bytecode_analysis().IsLoopHeader(target));
    const compiler::BytecodeLivenessState* liveness =
        bytecode_analysis().GetInLivenessFor(target);
    // If there's no target frame state, allocate a new one.
    merge_states_[target] = zone()->New<MergePointInterpreterFrameState>(
        *compilation_unit_, current_interpreter_frame_, target,
        NumPredecessors(target), predecessor, liveness);
  } else {
    // If there already is a frame state, merge.
    merge_states_[target]->Merge(*compilation_unit_, current_interpreter_frame_,
                                 predecessor, target);
  }
}

void MaglevGraphBuilder::MergeDeadIntoFrameState(int target) {
  // If there is no merge state yet, don't create one, but just reduce the
  // number of possible predecessors to zero.
  predecessors_[target]--;
  if (merge_states_[target]) {
    // If there already is a frame state, merge.
    merge_states_[target]->MergeDead(*compilation_unit_, target);
  }
}

void MaglevGraphBuilder::MergeIntoInlinedReturnFrameState(
    BasicBlock* predecessor) {
  int target = inline_exit_offset();
  if (merge_states_[target] == nullptr) {
    // All returns should have the same liveness, which is that only the
    // accumulator is live.
    const compiler::BytecodeLivenessState* liveness =
        bytecode_analysis().GetInLivenessFor(iterator_.current_offset());
    DCHECK(liveness->AccumulatorIsLive());
    DCHECK_EQ(liveness->live_value_count(), 1);

    // If there's no target frame state, allocate a new one.
    merge_states_[target] = zone()->New<MergePointInterpreterFrameState>(
        *compilation_unit_, current_interpreter_frame_, target,
        NumPredecessors(target), predecessor, liveness);
  } else {
    // Again, all returns should have the same liveness, so double check this.
    DCHECK(bytecode_analysis()
               .GetInLivenessFor(iterator_.current_offset())
               ->Equals(*merge_states_[target]->frame_state().liveness()));
    merge_states_[target]->Merge(*compilation_unit_, current_interpreter_frame_,
                                 predecessor, target);
  }
}

void MaglevGraphBuilder::BuildBranchIfTrue(ValueNode* node, int true_target,
                                           int false_target) {
  BasicBlock* block = FinishBlock<BranchIfTrue>(next_offset(), {node},
                                                &jump_targets_[true_target],
                                                &jump_targets_[false_target]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
}
void MaglevGraphBuilder::BuildBranchIfToBooleanTrue(ValueNode* node,
                                                    int true_target,
                                                    int false_target) {
  BasicBlock* block = FinishBlock<BranchIfToBooleanTrue>(
      next_offset(), {node}, &jump_targets_[true_target],
      &jump_targets_[false_target]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
}
void MaglevGraphBuilder::VisitJumpIfToBooleanTrue() {
  BuildBranchIfToBooleanTrue(GetAccumulatorTagged(),
                             iterator_.GetJumpTargetOffset(), next_offset());
}
void MaglevGraphBuilder::VisitJumpIfToBooleanFalse() {
  BuildBranchIfToBooleanTrue(GetAccumulatorTagged(), next_offset(),
                             iterator_.GetJumpTargetOffset());
}
void MaglevGraphBuilder::VisitJumpIfTrue() {
  BuildBranchIfTrue(GetAccumulatorTagged(), iterator_.GetJumpTargetOffset(),
                    next_offset());
}
void MaglevGraphBuilder::VisitJumpIfFalse() {
  BuildBranchIfTrue(GetAccumulatorTagged(), next_offset(),
                    iterator_.GetJumpTargetOffset());
}
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfNull)
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfNotNull)
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfUndefined)
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfNotUndefined)
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfUndefinedOrNull)
MAGLEV_UNIMPLEMENTED_BYTECODE(JumpIfJSReceiver)
MAGLEV_UNIMPLEMENTED_BYTECODE(SwitchOnSmiNoFeedback)
MAGLEV_UNIMPLEMENTED_BYTECODE(ForInEnumerate)
MAGLEV_UNIMPLEMENTED_BYTECODE(ForInPrepare)
MAGLEV_UNIMPLEMENTED_BYTECODE(ForInContinue)
MAGLEV_UNIMPLEMENTED_BYTECODE(ForInNext)
MAGLEV_UNIMPLEMENTED_BYTECODE(ForInStep)
MAGLEV_UNIMPLEMENTED_BYTECODE(SetPendingMessage)
MAGLEV_UNIMPLEMENTED_BYTECODE(Throw)
MAGLEV_UNIMPLEMENTED_BYTECODE(ReThrow)
void MaglevGraphBuilder::VisitReturn() {
  if (!is_inline()) {
    FinishBlock<Return>(next_offset(), {GetAccumulatorTagged()});
    return;
  }

  // All inlined function returns instead jump to one past the end of the
  // bytecode, where we'll later create a final basic block which resumes
  // execution of the caller.
  // TODO(leszeks): Consider shortcutting this Jump for cases where there is
  // only one return and no need to merge return states.
  BasicBlock* block = FinishBlock<Jump>(next_offset(), {},
                                        &jump_targets_[inline_exit_offset()]);
  MergeIntoInlinedReturnFrameState(block);
}
MAGLEV_UNIMPLEMENTED_BYTECODE(ThrowReferenceErrorIfHole)
MAGLEV_UNIMPLEMENTED_BYTECODE(ThrowSuperNotCalledIfHole)
MAGLEV_UNIMPLEMENTED_BYTECODE(ThrowSuperAlreadyCalledIfNotHole)
MAGLEV_UNIMPLEMENTED_BYTECODE(ThrowIfNotSuperConstructor)
MAGLEV_UNIMPLEMENTED_BYTECODE(SwitchOnGeneratorState)
MAGLEV_UNIMPLEMENTED_BYTECODE(SuspendGenerator)
MAGLEV_UNIMPLEMENTED_BYTECODE(ResumeGenerator)
MAGLEV_UNIMPLEMENTED_BYTECODE(GetIterator)
MAGLEV_UNIMPLEMENTED_BYTECODE(Debugger)
MAGLEV_UNIMPLEMENTED_BYTECODE(IncBlockCounter)
MAGLEV_UNIMPLEMENTED_BYTECODE(Abort)

void MaglevGraphBuilder::VisitWide() { UNREACHABLE(); }
void MaglevGraphBuilder::VisitExtraWide() { UNREACHABLE(); }
#define DEBUG_BREAK(Name, ...) \
  void MaglevGraphBuilder::Visit##Name() { UNREACHABLE(); }
DEBUG_BREAK_BYTECODE_LIST(DEBUG_BREAK)
#undef DEBUG_BREAK
void MaglevGraphBuilder::VisitIllegal() { UNREACHABLE(); }

}  // namespace maglev
}  // namespace internal
}  // namespace v8
