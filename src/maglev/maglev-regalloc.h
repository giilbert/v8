// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_MAGLEV_MAGLEV_REGALLOC_H_
#define V8_MAGLEV_MAGLEV_REGALLOC_H_

#include "src/codegen/reglist.h"
#include "src/compiler/backend/instruction.h"
#include "src/maglev/maglev-compilation-info.h"
#include "src/maglev/maglev-graph.h"
#include "src/maglev/maglev-ir.h"
#include "src/maglev/maglev-regalloc-data.h"

namespace v8 {
namespace internal {
namespace maglev {

class MaglevCompilationInfo;
class MaglevPrintingVisitor;
class MergePointRegisterState;

class StraightForwardRegisterAllocator {
 public:
  StraightForwardRegisterAllocator(MaglevCompilationInfo* compilation_info,
                                   Graph* graph);
  ~StraightForwardRegisterAllocator();

 private:
  std::vector<int> future_register_uses_[Register::kNumRegisters];
  ValueNode* register_values_[Register::kNumRegisters];

  RegList free_registers_ = kAllocatableGeneralRegisters;

  struct SpillSlotInfo {
    SpillSlotInfo(uint32_t slot_index, NodeIdT freed_at_position)
        : slot_index(slot_index), freed_at_position(freed_at_position) {}
    uint32_t slot_index;
    NodeIdT freed_at_position;
  };
  struct SpillSlots {
    int top = 0;
    // Sorted from earliest freed_at_position to latest freed_at_position.
    std::vector<SpillSlotInfo> free_slots;
  };

  SpillSlots untagged_;
  SpillSlots tagged_;

  RegList used_registers() const {
    // Only allocatable registers should be free.
    DCHECK_EQ(free_registers_, free_registers_ & kAllocatableGeneralRegisters);
    return kAllocatableGeneralRegisters ^ free_registers_;
  }

  void ComputePostDominatingHoles(Graph* graph);
  void AllocateRegisters(Graph* graph);

  void PrintLiveRegs() const;

  void UpdateUse(Input* input) { return UpdateUse(input->node(), input); }
  void UpdateUse(ValueNode* node, InputLocation* input_location);
  void UpdateUse(const EagerDeoptInfo& deopt_info);
  void UpdateUse(const LazyDeoptInfo& deopt_info);
  void UpdateUse(const MaglevCompilationUnit& unit,
                 const CheckpointedInterpreterState* state,
                 InputLocation* input_locations, int& index);

  void AllocateControlNode(ControlNode* node, BasicBlock* block);
  void AllocateNode(Node* node);
  void AllocateNodeResult(ValueNode* node);
  void AssignInput(Input& input);
  void AssignTemporaries(NodeBase* node);
  void TryAllocateToInput(Phi* phi);

  void FreeRegisters(ValueNode* node) {
    RegList list = node->ClearRegisters<Register>();
    DCHECK_EQ(free_registers_ & list, kEmptyRegList);
    free_registers_ |= list;
  }
  void FreeRegister(Register reg) { free_registers_.set(reg); }

  ValueNode* GetRegisterValue(Register reg) const {
    DCHECK(!free_registers_.has(reg));
    ValueNode* node = register_values_[reg.code()];
    DCHECK_NOT_NULL(node);
    return node;
  }

  void FreeSomeRegister();
  void AddMoveBeforeCurrentNode(compiler::AllocatedOperand source,
                                compiler::AllocatedOperand target);

  void AllocateSpillSlot(ValueNode* node);
  void Spill(ValueNode* node);
  void SpillAndClearRegisters();
  void SpillRegisters();

  compiler::AllocatedOperand AllocateRegister(ValueNode* node);
  compiler::AllocatedOperand ForceAllocate(Register reg, ValueNode* node);
  void SetRegister(Register reg, ValueNode* node);
  void DropRegisterValue(Register reg);
  compiler::InstructionOperand TryAllocateRegister(ValueNode* node);

  void InitializeRegisterValues(MergePointRegisterState& target_state);
  void EnsureInRegister(MergePointRegisterState& target_state,
                        ValueNode* incoming);

  void InitializeBranchTargetRegisterValues(ControlNode* source,
                                            BasicBlock* target);
  void InitializeConditionalBranchRegisters(ConditionalControlNode* source,
                                            BasicBlock* target);
  void MergeRegisterValues(ControlNode* control, BasicBlock* target,
                           int predecessor_id);

  MaglevGraphLabeller* graph_labeller() const {
    return compilation_info_->graph_labeller();
  }

  MaglevCompilationInfo* compilation_info_;
  std::unique_ptr<MaglevPrintingVisitor> printing_visitor_;
  BlockConstIterator block_it_;
  NodeIterator node_it_;
};

}  // namespace maglev
}  // namespace internal
}  // namespace v8

#endif  // V8_MAGLEV_MAGLEV_REGALLOC_H_
