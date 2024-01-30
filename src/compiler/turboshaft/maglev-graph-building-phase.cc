// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/turboshaft/maglev-graph-building-phase.h"

#include "src/compiler/js-heap-broker.h"
#include "src/compiler/turboshaft/assembler.h"
#include "src/compiler/turboshaft/machine-optimization-reducer.h"
#include "src/compiler/turboshaft/operations.h"
#include "src/compiler/turboshaft/representations.h"
#include "src/compiler/turboshaft/required-optimization-reducer.h"
#include "src/compiler/turboshaft/value-numbering-reducer.h"
#include "src/compiler/turboshaft/variable-reducer.h"
#include "src/handles/global-handles-inl.h"
#include "src/handles/handles.h"
#include "src/maglev/maglev-compilation-info.h"
#include "src/maglev/maglev-graph-builder.h"
#include "src/maglev/maglev-graph-processor.h"
#include "src/maglev/maglev-ir.h"

namespace v8::internal::compiler::turboshaft {

#include "src/compiler/turboshaft/define-assembler-macros.inc"

class GraphBuilder {
 public:
  using AssemblerT =
      TSAssembler<MachineOptimizationReducer, VariableReducer,
                  RequiredOptimizationReducer, ValueNumberingReducer>;

  GraphBuilder(Graph& graph, Zone* temp_zone)
      : temp_zone_(temp_zone),
        assembler_(graph, graph, temp_zone),
        node_mapping_(temp_zone),
        block_mapping_(temp_zone) {}

  void PreProcessGraph(maglev::Graph* graph) {
    for (maglev::BasicBlock* block : *graph) {
      block_mapping_[block] =
          block->is_loop() ? __ NewLoopHeader() : __ NewBlock();
    }
    // Constants are not in a block in Maglev but are in Turboshaft. We bind a
    // block now, so that Constants can then be emitted.
    __ Bind(__ NewBlock());
  }

  void PostProcessGraph(maglev::Graph* graph) {}

  void PreProcessBasicBlock(maglev::BasicBlock* block) {
    if (__ current_block() != nullptr) {
      // The first block for Constants doesn't end with a Jump, so we add one
      // now.
      __ Goto(Map(block));
    }
    __ Bind(Map(block));
  }

  maglev::ProcessResult Process(maglev::Constant* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ HeapConstant(node->object().object()));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::RootConstant* node,
                                const maglev::ProcessingState& state) {
    SetMap(
        node,
        __ HeapConstant(
            MakeRef(broker_, node->DoReify(isolate_)).AsHeapObject().object()));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::Int32Constant* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ Word32Constant(node->value()));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::Float64Constant* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ Float64Constant(
                     base::bit_cast<double>(node->value().get_bits())));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::SmiConstant* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ SmiConstant(node->value()));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::InitialValue* node,
                                const maglev::ProcessingState& state) {
#ifdef DEBUG
    char* debug_name = strdup(node->source().ToString().c_str());
#else
    char* debug_name = nullptr;
#endif
    SetMap(node, __ Parameter(node->source().ToParameterIndex(),
                              RegisterRepresentation::Tagged(), debug_name));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::FunctionEntryStackCheck* node,
                                const maglev::ProcessingState& state) {
    __ StackCheck(StackCheckOp::CheckOrigin::kFromJS,
                  StackCheckOp::CheckKind::kFunctionHeaderCheck);
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::Jump* node,
                                const maglev::ProcessingState& state) {
    __ Goto(Map(node->target()));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::BranchIfToBooleanTrue* node,
                                const maglev::ProcessingState& state) {
    TruncateJSPrimitiveToUntaggedOp::InputAssumptions assumption =
        node->check_type() == maglev::CheckType::kCheckHeapObject
            ? TruncateJSPrimitiveToUntaggedOp::InputAssumptions::kObject
            : TruncateJSPrimitiveToUntaggedOp::InputAssumptions::kHeapObject;
    V<Word32> condition = __ TruncateJSPrimitiveToUntagged(
        Map(node->condition_input()),
        TruncateJSPrimitiveToUntaggedOp::UntaggedKind::kBit, assumption);
    __ Branch(condition, Map(node->if_true()), Map(node->if_false()));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::Int32Compare* node,
                                const maglev::ProcessingState& state) {
    Label<Tagged> done(this);
    IF (ConvertInt32Compare(node->left_input(), node->right_input(),
                            node->operation())) {
      GOTO(done, __ HeapConstant(factory_->true_value()));
    }
    ELSE {
      GOTO(done, __ HeapConstant(factory_->false_value()));
    }
    END_IF
    BIND(done, result);
    SetMap(node, result);
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::BranchIfInt32Compare* node,
                                const maglev::ProcessingState& state) {
    V<Word32> condition = ConvertInt32Compare(
        node->left_input(), node->right_input(), node->operation());
    __ Branch(condition, Map(node->if_true()), Map(node->if_false()));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::CheckedSmiUntag* node,
                                const maglev::ProcessingState& state) {
    SetMap(node,
           __ CheckedSmiUntag(Map(node->input()),
                              BuildFrameState(node->eager_deopt_info()),
                              node->eager_deopt_info()->feedback_to_update()));
    return maglev::ProcessResult::kContinue;
  }

#define PROCESS_BINOP_WITH_OVERFLOW(MaglevName, TurboshaftName,                \
                                    minus_zero_mode)                           \
  maglev::ProcessResult Process(maglev::Int32##MaglevName##WithOverflow* node, \
                                const maglev::ProcessingState& state) {        \
    OpIndex frame_state = BuildFrameState(node->eager_deopt_info());           \
    SetMap(node,                                                               \
           __ Word32##TurboshaftName##DeoptOnOverflow(                         \
               Map(node->left_input()), Map(node->right_input()), frame_state, \
               node->eager_deopt_info()->feedback_to_update(),                 \
               CheckForMinusZeroMode::k##minus_zero_mode));                    \
    return maglev::ProcessResult::kContinue;                                   \
  }
  PROCESS_BINOP_WITH_OVERFLOW(Add, SignedAdd, DontCheckForMinusZero)
  PROCESS_BINOP_WITH_OVERFLOW(Subtract, SignedSub, DontCheckForMinusZero)
  PROCESS_BINOP_WITH_OVERFLOW(Multiply, SignedMul, CheckForMinusZero)
  PROCESS_BINOP_WITH_OVERFLOW(Divide, SignedDiv, CheckForMinusZero)
  PROCESS_BINOP_WITH_OVERFLOW(Modulus, SignedMod, CheckForMinusZero)
#undef PROCESS_BINOP_WITH_OVERFLOW

#define PROCESS_FLOAT64_BINOP(MaglevName, TurboshaftName)               \
  maglev::ProcessResult Process(maglev::Float64##MaglevName* node,      \
                                const maglev::ProcessingState& state) { \
    SetMap(node, __ Float64##TurboshaftName(Map(node->left_input()),    \
                                            Map(node->right_input()))); \
    return maglev::ProcessResult::kContinue;                            \
  }
  PROCESS_FLOAT64_BINOP(Add, Add)
  PROCESS_FLOAT64_BINOP(Subtract, Sub)
  PROCESS_FLOAT64_BINOP(Multiply, Mul)
  PROCESS_FLOAT64_BINOP(Divide, Div)
  PROCESS_FLOAT64_BINOP(Modulus, Mod)
  PROCESS_FLOAT64_BINOP(Exponentiate, Power)
#undef PROCESS_FLOAT64_BINOP

#define PROCESS_INT32_BITWISE_BINOP(Name)                               \
  maglev::ProcessResult Process(maglev::Int32Bitwise##Name* node,       \
                                const maglev::ProcessingState& state) { \
    SetMap(node, __ Word32Bitwise##Name(Map(node->left_input()),        \
                                        Map(node->right_input())));     \
    return maglev::ProcessResult::kContinue;                            \
  }
  PROCESS_INT32_BITWISE_BINOP(And)
  PROCESS_INT32_BITWISE_BINOP(Or)
  PROCESS_INT32_BITWISE_BINOP(Xor)
#undef PROCESS_INT32_BITWISE_BINOP

#define PROCESS_INT32_SHIFT(MaglevName, TurboshaftName)                 \
  maglev::ProcessResult Process(maglev::Int32##MaglevName* node,        \
                                const maglev::ProcessingState& state) { \
    SetMap(node, __ Word32##TurboshaftName(Map(node->left_input()),     \
                                           Map(node->right_input())));  \
    return maglev::ProcessResult::kContinue;                            \
  }
  PROCESS_INT32_SHIFT(ShiftLeft, ShiftLeft)
  PROCESS_INT32_SHIFT(ShiftRight, ShiftRightArithmetic)
  PROCESS_INT32_SHIFT(ShiftRightLogical, ShiftRightLogical)
#undef PROCESS_INT32_SHIFT

  maglev::ProcessResult Process(maglev::Int32BitwiseNot* node,
                                const maglev::ProcessingState& state) {
    // Turboshaft doesn't have a bitwise Not operator; we instead use "^ -1".
    SetMap(node, __ Word32BitwiseXor(Map(node->value_input()),
                                     __ Word32Constant(-1)));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::Float64Negate* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ Float64Negate(Map(node->input())));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::Float64Round* node,
                                const maglev::ProcessingState& state) {
    if (node->kind() == maglev::Float64Round::Kind::kFloor) {
      SetMap(node, __ Float64RoundDown(Map(node->input())));
    } else if (node->kind() == maglev::Float64Round::Kind::kCeil) {
      SetMap(node, __ Float64RoundUp(Map(node->input())));
    } else {
      DCHECK_EQ(node->kind(), maglev::Float64Round::Kind::kNearest);
      // Nearest rounds to +infinity on ties. We emulate this by rounding up and
      // adjusting if the difference exceeds 0.5 (like SimplifiedLowering does
      // for lower Float64Round).
      OpIndex input = Map(node->input());
      ScopedVariable<Float64, AssemblerT> result(Asm(),
                                                 __ Float64RoundUp(input));
      IF_NOT (__ Float64LessThanOrEqual(__ Float64Sub(*result, 0.5), input)) {
        result = __ Float64Sub(*result, 1.0);
      }
      END_IF
      SetMap(node, *result);
    }
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::Int32ToNumber* node,
                                const maglev::ProcessingState& state) {
    SetMap(node, __ ConvertInt32ToNumber(Map(node->input())));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::Float64ToTagged* node,
                                const maglev::ProcessingState& state) {
    // Float64ToTagged's conversion mode is used to control whether integer
    // floats should be converted to Smis or to HeapNumbers: kCanonicalizeSmi
    // means that they can be converted to Smis, and otherwise they should
    // remain HeapNumbers.
    ConvertUntaggedToJSPrimitiveOp::JSPrimitiveKind kind =
        node->conversion_mode() ==
                maglev::Float64ToTagged::ConversionMode::kCanonicalizeSmi
            ? ConvertUntaggedToJSPrimitiveOp::JSPrimitiveKind::kNumber
            : ConvertUntaggedToJSPrimitiveOp::JSPrimitiveKind::kHeapNumber;
    SetMap(node,
           __ ConvertUntaggedToJSPrimitive(
               Map(node->input()), kind, RegisterRepresentation::Float64(),
               ConvertUntaggedToJSPrimitiveOp::InputInterpretation::kSigned,
               CheckForMinusZeroMode::kCheckForMinusZero));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::CheckedNumberOrOddballToFloat64* node,
                                const maglev::ProcessingState& state) {
    SetMap(node,
           __ ConvertJSPrimitiveToUntaggedOrDeopt(
               Map(node->input()), BuildFrameState(node->eager_deopt_info()),
               ConvertJSPrimitiveToUntaggedOrDeoptOp::JSPrimitiveKind::
                   kNumberOrOddball,
               ConvertJSPrimitiveToUntaggedOrDeoptOp::UntaggedKind::kFloat64,
               CheckForMinusZeroMode::kCheckForMinusZero,
               node->eager_deopt_info()->feedback_to_update()));
    return maglev::ProcessResult::kContinue;
  }
  maglev::ProcessResult Process(maglev::TruncateUint32ToInt32* node,
                                const maglev::ProcessingState& state) {
    // This doesn't matter in Turboshaft: both Uint32 and Int32 are Word32.
    SetMap(node, Map(node->input()));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::Return* node,
                                const maglev::ProcessingState& state) {
    __ Return(Map(node->value_input()));
    return maglev::ProcessResult::kContinue;
  }

  maglev::ProcessResult Process(maglev::ReduceInterruptBudgetForReturn*,
                                const maglev::ProcessingState&) {
    // No need to update the interrupt budget once we reach Turboshaft.
    return maglev::ProcessResult::kContinue;
  }

  template <typename NodeT>
  maglev::ProcessResult Process(NodeT* node,
                                const maglev::ProcessingState& state) {
    UNIMPLEMENTED();
  }

  AssemblerT& Asm() { return assembler_; }
  Zone* temp_zone() { return temp_zone_; }
  Zone* graph_zone() { return Asm().output_graph().graph_zone(); }

 private:
  OpIndex BuildFrameState(maglev::EagerDeoptInfo* eager_deopt_info) {
    DCHECK_EQ(eager_deopt_info->top_frame().type(),
              maglev::DeoptFrame::FrameType::kInterpretedFrame);
    maglev::InterpretedDeoptFrame& frame =
        eager_deopt_info->top_frame().as_interpreted();
    FrameStateData::Builder builder;
    if (eager_deopt_info->top_frame().parent() != nullptr) {
      // TODO(dmercadier): do something about inlining.
      UNIMPLEMENTED();
    }

    // Closure
    builder.AddInput(MachineType::AnyTagged(), Map(frame.closure()));

    // Parameters
    frame.frame_state()->ForEachParameter(
        frame.unit(), [&](maglev::ValueNode* value, interpreter::Register reg) {
          builder.AddInput(MachineType::AnyTagged(), Map(value));
        });

    // Context
    builder.AddInput(MachineType::AnyTagged(),
                     Map(frame.frame_state()->context(frame.unit())));

    // The accumulator should be included both in the locals and the "stack"
    // input.
    if (frame.frame_state()->liveness()->AccumulatorIsLive()) {
      builder.AddInput(MachineType::AnyTagged(),
                       Map(frame.frame_state()->accumulator(frame.unit())));
    } else {
      // TODO(dmercadier): should we add an unused register or nothing here?
      builder.AddUnusedRegister();
    }

    // Locals
    // note that ForEachLocal skips the accumulator.
    frame.frame_state()->ForEachLocal(
        frame.unit(), [&](maglev::ValueNode* value, interpreter::Register reg) {
          builder.AddInput(MachineType::AnyTagged(), Map(value));
        });

    // Accumulator
    if (frame.frame_state()->liveness()->AccumulatorIsLive()) {
      builder.AddInput(MachineType::AnyTagged(),
                       Map(frame.frame_state()->accumulator(frame.unit())));
    } else {
      builder.AddUnusedRegister();
    }

    const FrameStateInfo* frame_state_info = MakeFrameStateInfo(frame);
    return __ FrameState(
        builder.Inputs(), builder.inlined(),
        builder.AllocateFrameStateData(*frame_state_info, graph_zone()));
  }

  const FrameStateInfo* MakeFrameStateInfo(
      maglev::InterpretedDeoptFrame& maglev_frame) {
    FrameStateType type = FrameStateType::kUnoptimizedFunction;
    int parameter_count = maglev_frame.unit().parameter_count();
    int local_count =
        maglev_frame.frame_state()->liveness()->live_value_count();
    Handle<SharedFunctionInfo> shared_info =
        PipelineData::Get().info()->shared_info();
    FrameStateFunctionInfo* info = graph_zone()->New<FrameStateFunctionInfo>(
        type, parameter_count, local_count, shared_info);

    return graph_zone()->New<FrameStateInfo>(maglev_frame.bytecode_position(),
                                             OutputFrameStateCombine::Ignore(),
                                             info);
  }

  V<Word32> ConvertInt32Compare(maglev::Input left_input,
                                maglev::Input right_input,
                                ::Operation operation) {
    ComparisonOp::Kind kind;
    bool swap = false;
    switch (operation) {
      case ::Operation::kEqual:
        kind = ComparisonOp::Kind::kEqual;
        break;
      case ::Operation::kLessThan:
        kind = ComparisonOp::Kind::kSignedLessThan;
        break;
      case ::Operation::kLessThanOrEqual:
        kind = ComparisonOp::Kind::kSignedLessThanOrEqual;
        break;
      case ::Operation::kGreaterThan:
        kind = ComparisonOp::Kind::kSignedLessThan;
        swap = true;
        break;
      case ::Operation::kGreaterThanOrEqual:
        kind = ComparisonOp::Kind::kSignedLessThanOrEqual;
        swap = true;
        break;
      default:
        UNREACHABLE();
    }
    V<Word32> left = Map(left_input);
    V<Word32> right = Map(right_input);
    if (swap) std::swap(left, right);
    return __ Comparison(left, right, kind, WordRepresentation::Word32());
  }

  OpIndex Map(const maglev::Input input) { return Map(input.node()); }
  OpIndex Map(const maglev::NodeBase* node) { return node_mapping_[node]; }
  Block* Map(const maglev::BasicBlock* block) { return block_mapping_[block]; }

  OpIndex SetMap(maglev::NodeBase* node, OpIndex idx) {
    node_mapping_[node] = idx;
    return idx;
  }

  Zone* temp_zone_;
  LocalIsolate* isolate_ = PipelineData::Get().isolate()->AsLocalIsolate();
  JSHeapBroker* broker_ = PipelineData::Get().broker();
  LocalFactory* factory_ = isolate_->factory();
  AssemblerT assembler_;
  ZoneUnorderedMap<const maglev::NodeBase*, OpIndex> node_mapping_;
  ZoneUnorderedMap<const maglev::BasicBlock*, Block*> block_mapping_;
};

void MaglevGraphBuildingPhase::Run(Zone* temp_zone) {
  PipelineData& data = PipelineData::Get();
  JSHeapBroker* broker = data.broker();
  UnparkedScopeIfNeeded unparked_scope(broker);

  auto compilation_info = maglev::MaglevCompilationInfo::New(
      data.isolate(), broker, data.info()->closure(),
      data.info()->osr_offset());

  LocalIsolate* local_isolate = broker->local_isolate()
                                    ? broker->local_isolate()
                                    : broker->isolate()->AsLocalIsolate();
  maglev::Graph* maglev_graph =
      maglev::Graph::New(temp_zone, data.info()->is_osr());
  maglev::MaglevGraphBuilder maglev_graph_builder(
      local_isolate, compilation_info->toplevel_compilation_unit(),
      maglev_graph);
  maglev_graph_builder.Build();

  maglev::GraphProcessor<GraphBuilder, true> builder(data.graph(), temp_zone);
  builder.ProcessGraph(maglev_graph);
}

#include "src/compiler/turboshaft/undef-assembler-macros.inc"

}  // namespace v8::internal::compiler::turboshaft
