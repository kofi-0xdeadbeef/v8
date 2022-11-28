// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-ir.h"

#include "src/execution/isolate-inl.h"
#include "src/heap/local-heap.h"
#include "src/heap/parked-scope.h"
#include "src/maglev/maglev-graph-labeller.h"

namespace v8 {
namespace internal {
namespace maglev {

const char* OpcodeToString(Opcode opcode) {
#define DEF_NAME(Name) #Name,
  static constexpr const char* const names[] = {NODE_BASE_LIST(DEF_NAME)};
#undef DEF_NAME
  return names[static_cast<int>(opcode)];
}

namespace {

// ---
// Print
// ---

void PrintInputs(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {
  if (!node->has_inputs()) return;

  os << " [";
  for (int i = 0; i < node->input_count(); i++) {
    if (i != 0) os << ", ";
    graph_labeller->PrintInput(os, node->input(i));
  }
  os << "]";
}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const ValueNode* node) {
  os << " → " << node->result().operand();
  if (node->has_valid_live_range()) {
    os << ", live range: [" << node->live_range().start << "-"
       << node->live_range().end << "]";
  }
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const NodeBase* node) {}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const UnconditionalControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->target());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const BranchControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->if_true()) << " b"
     << graph_labeller->BlockId(node->if_false());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const Switch* node) {
  for (int i = 0; i < node->size(); i++) {
    const BasicBlockRef& target = node->Cast<Switch>()->targets()[i];
    os << " b" << graph_labeller->BlockId(target.block_ptr());
  }
  if (node->Cast<Switch>()->has_fallthrough()) {
    BasicBlock* fallthrough_target = node->Cast<Switch>()->fallthrough();
    os << " b" << graph_labeller->BlockId(fallthrough_target);
  }
}

class MaybeUnparkForPrint {
 public:
  MaybeUnparkForPrint() {
    LocalHeap* local_heap = LocalHeap::Current();
    if (!local_heap) {
      local_heap = Isolate::Current()->main_thread_local_heap();
    }
    DCHECK_NOT_NULL(local_heap);
    if (local_heap->IsParked()) {
      scope_.emplace(local_heap);
    }
  }

 private:
  base::Optional<UnparkedScope> scope_;
};

template <typename NodeT>
void PrintImpl(std::ostream& os, MaglevGraphLabeller* graph_labeller,
               const NodeT* node, bool skip_targets) {
  MaybeUnparkForPrint unpark;
  os << node->opcode();
  node->PrintParams(os, graph_labeller);
  PrintInputs(os, graph_labeller, node);
  PrintResult(os, graph_labeller, node);
  if (!skip_targets) {
    PrintTargets(os, graph_labeller, node);
  }
}

size_t GetInputLocationsArraySize(const DeoptFrame& top_frame) {
  size_t size = 0;
  const DeoptFrame* frame = &top_frame;
  do {
    switch (frame->type()) {
      case DeoptFrame::FrameType::kInterpretedFrame:
        size += frame->as_interpreted().frame_state()->size(
            frame->as_interpreted().unit());
        break;
      case DeoptFrame::FrameType::kBuiltinContinuationFrame:
        size += frame->as_builtin_continuation().parameters().size() + 1;
        break;
    }
    frame = frame->parent();
  } while (frame != nullptr);
  return size;
}

}  // namespace

bool RootConstant::ToBoolean(LocalIsolate* local_isolate) const {
  switch (index_) {
    case RootIndex::kFalseValue:
    case RootIndex::kNullValue:
    case RootIndex::kUndefinedValue:
    case RootIndex::kempty_string:
      return false;
    default:
      return true;
  }
}

DeoptInfo::DeoptInfo(Zone* zone, DeoptFrame top_frame,
                     compiler::FeedbackSource feedback_to_update)
    : top_frame_(top_frame),
      feedback_to_update_(feedback_to_update),
      input_locations_(zone->NewArray<InputLocation>(
          GetInputLocationsArraySize(top_frame))) {
  // Initialise InputLocations so that they correctly don't have a next use id.
  for (size_t i = 0; i < GetInputLocationsArraySize(top_frame); ++i) {
    new (&input_locations_[i]) InputLocation();
  }
}

bool LazyDeoptInfo::IsResultRegister(interpreter::Register reg) const {
  if (V8_LIKELY(result_size_ == 1)) {
    return reg == result_location_;
  }
  DCHECK_EQ(result_size_, 2);
  return reg == result_location_ ||
         reg == interpreter::Register(result_location_.index() + 1);
}

void NodeBase::Print(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                     bool skip_targets) const {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return PrintImpl(os, graph_labeller, this->Cast<Name>(), skip_targets);
    NODE_BASE_LIST(V)
#undef V
  }
  UNREACHABLE();
}

void NodeBase::Print() const {
  MaglevGraphLabeller labeller;
  Print(std::cout, &labeller);
  std::cout << std::endl;
}

void ValueNode::SetNoSpillOrHint() {
  DCHECK_EQ(state_, kLastUse);
  DCHECK(!IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpillOrHint;
#endif  // DEBUG
  spill_or_hint_ = compiler::InstructionOperand();
}

void ValueNode::SetConstantLocation() {
  DCHECK(IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpillOrHint;
#endif  // DEBUG
  spill_or_hint_ = compiler::ConstantOperand(
      compiler::UnallocatedOperand::cast(result().operand())
          .virtual_register());
}

ValueRepresentation ToValueRepresentation(MachineType type) {
  switch (type.representation()) {
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedSigned:
    case MachineRepresentation::kTaggedPointer:
      return ValueRepresentation::kTagged;
    case MachineRepresentation::kFloat64:
      return ValueRepresentation::kFloat64;
    default:
      return ValueRepresentation::kInt32;
  }
}

void CheckValueInputIs(const NodeBase* node, int i,
                       ValueRepresentation expected,
                       MaglevGraphLabeller* graph_labeller) {
  ValueNode* input = node->input(i).node();
  ValueRepresentation got = input->properties().value_representation();
  if (got != expected) {
    std::ostringstream str;
    str << "Type representation error: node ";
    if (graph_labeller) {
      str << "#" << graph_labeller->NodeId(node) << " : ";
    }
    str << node->opcode() << " (input @" << i << " = " << input->opcode()
        << ") type " << got << " is not " << expected;
    FATAL("%s", str.str().c_str());
  }
}

void CheckValueInputIsWord32(const NodeBase* node, int i,
                             MaglevGraphLabeller* graph_labeller) {
  ValueNode* input = node->input(i).node();
  ValueRepresentation got = input->properties().value_representation();
  if (got != ValueRepresentation::kInt32 &&
      got != ValueRepresentation::kUint32) {
    std::ostringstream str;
    str << "Type representation error: node ";
    if (graph_labeller) {
      str << "#" << graph_labeller->NodeId(node) << " : ";
    }
    str << node->opcode() << " (input @" << i << " = " << input->opcode()
        << ") type " << got << " is not Word32 (Int32 or Uint32)";
    FATAL("%s", str.str().c_str());
  }
}

void GeneratorStore::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void UnsafeSmiTag::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  DCHECK_EQ(input_count(), 1);
  CheckValueInputIsWord32(this, 0, graph_labeller);
}

void Phi::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void Call::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallWithArrayLike::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallWithSpread::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallKnownJSFunction::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void Construct::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void ConstructWithSpread::VerifyInputs(
    MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

void CallBuiltin::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int count = input_count();
  // Verify context.
  if (descriptor.HasContextParameter()) {
    CheckValueInputIs(this, count - 1, ValueRepresentation::kTagged,
                      graph_labeller);
    count--;
  }

// {all_input_count} includes the feedback slot and vector.
#ifdef DEBUG
  int all_input_count = count + (has_feedback() ? 2 : 0);
  if (descriptor.AllowVarArgs()) {
    DCHECK_GE(all_input_count, descriptor.GetParameterCount());
  } else {
    DCHECK_EQ(all_input_count, descriptor.GetParameterCount());
  }
#endif
  int i = 0;
  // Check the rest of inputs.
  for (; i < count; ++i) {
    MachineType type = i < descriptor.GetParameterCount()
                           ? descriptor.GetParameterType(i)
                           : MachineType::AnyTagged();
    CheckValueInputIs(this, i, ToValueRepresentation(type), graph_labeller);
  }
}

void CallRuntime::VerifyInputs(MaglevGraphLabeller* graph_labeller) const {
  for (int i = 0; i < input_count(); i++) {
    CheckValueInputIs(this, i, ValueRepresentation::kTagged, graph_labeller);
  }
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
