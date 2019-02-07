/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UsedVarsAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "DeadCodeEliminationPass.h"
#include "IRAssembler.h"
#include "RedexTest.h"

namespace ptrs = local_pointers;
namespace uv = used_vars;

using namespace testing;

namespace ptrs = local_pointers;

class UsedVarsTest : public RedexTest {};

std::unique_ptr<uv::FixpointIterator> analyze(
    IRCode& code,
    const ptrs::InvokeToSummaryMap& invoke_to_esc_summary_map,
    const side_effects::InvokeToSummaryMap& invoke_to_eff_summary_map) {
  code.build_cfg(/* editable */ false);
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();

  ptrs::FixpointIterator pointers_fp_iter(cfg, invoke_to_esc_summary_map);
  pointers_fp_iter.run(ptrs::Environment());
  auto used_vars_fp_iter = std::make_unique<uv::FixpointIterator>(
      pointers_fp_iter, invoke_to_eff_summary_map, cfg);
  used_vars_fp_iter->run(uv::UsedVarsSet());

  return used_vars_fp_iter;
}

void optimize(const uv::FixpointIterator& fp_iter, IRCode* code) {
  for (auto it : uv::get_dead_instructions(*code, fp_iter)) {
    code->remove_opcode(it);
  }
}

// We need to construct the classes in our tests because the used vars analysis
// will call resolve_method() during its analysis. resolve_method() needs the
// method to reside in a class hierarchy in order to work correctly.
DexClass* create_simple_class(const std::string& name) {
  ClassCreator cc(DexType::make_type(name.c_str()));
  cc.set_super(get_object_type());
  auto* ctor =
      static_cast<DexMethod*>(DexMethod::make_method(name + ".<init>:()V"));
  ctor->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  cc.add_method(ctor);
  return cc.create();
}

TEST_F(UsedVarsTest, simple) {
  create_simple_class("LFoo;");

  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
      (const v1 0)
      (iput v1 v0 "LFoo;.bar:I")
      (return-void)
    )
  )");

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) &&
        insn->get_method() == DexMethod::get_method("LFoo;.<init>:()V")) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(UsedVarsTest, join) {
  create_simple_class("LFoo;");
  create_simple_class("LBar;");

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (goto :join)

      (:true)
      (new-instance "LBar;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LBar;.<init>:()V")
      (sput v0 "LUnknownClass;.unknownField:I")

      (:join)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && is_init(insn->get_method())) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      (goto :join)
      (:true)
      (sput v0 "LUnknownClass;.unknownField:I")
      (:join)
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(UsedVarsTest, noDeleteInit) {
  // Only one branch has a non-escaping object.
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :true)
      ; This object is unused and non-escaping; however, since we cannot delete
      ; the `iput` instruction in the join-block below, we cannot delete the
      ; call to Foo.<init>() in this block: writing to an uninitialized object
      ; would be a verification error.
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (goto :join)

      (:true)
      (sget-object "LBar;.bar:LBar;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LBar;.<init>:()V")

      (:join)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");
  auto expected = assembler::to_s_expr(code.get());

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && is_init(insn->get_method())) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(UsedVarsTest, noDeleteAliasedInit) {
  create_simple_class("LFoo;");

  // The used register differs from the initialized register, but they both
  // point to the same object.
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (move-object v1 v0)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (sput-object v0 "LBar;.foo:LFoo;")
      (return-void)
    )
  )");
  auto expected = assembler::to_s_expr(code.get());

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode()) && is_init(insn->get_method())) {
      invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
      invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(UsedVarsTest, noDeleteInitForUnreadObject) {
  auto foo_cls = create_simple_class("LFoo;");
  // This method will only modify the `this` argument.
  auto no_side_effects_method = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.nosideeffects:()V"));
  no_side_effects_method->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  foo_cls->get_dmethods().push_back(no_side_effects_method);

  // The object is never read or allowed to escape, but there's a non-removable
  // if-* opcode that branches on it. Check that we keep its initializer.
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
      ; Unfortunately, with our current implementation, we aren't able to remove
      ; this no-op call even though it would be safe to do so.
      (invoke-direct (v0) "LFoo;.nosideeffects:()V")
      (if-eqz v0 :exit)
      (invoke-static () "LBar;.something:()V")
      (:exit)
      (return-void)
    )
  )");
  auto expected = assembler::to_s_expr(code.get());

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto method = insn->get_method();
      if (is_init(method)) {
        invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
        invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
      } else if (method->get_name()->str() == "nosideeffects") {
        invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
        invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
      }
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(UsedVarsTest, noReturn) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
      (invoke-static () "LFoo;.noReturn:()V")
      ; This instruction is never executed since noReturn() never returns.
      ; Practically speaking, the instance of Foo in v0 is not used at runtime.
      ; However, if we are to delete the new-instance opcode above, we must also
      ; delete this iget opcode, otherwise the verifier will throw an error.
      ; This is a bit tedious to implement properly -- e.g. we would need to
      ; ensure that the `return` opcode below is replaced with an infinite loop
      ; so that we don't have any unterminated blocks that trip the dex verifier
      ; -- so for now we just assume that all methods return.
      (iget v0 "LFoo;.bar:I")
      (move-result-pseudo v1)
      (return v1)
    )
  )");
  // We expect nothing to change.
  auto expected = assembler::to_s_expr(code.get());

  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto callee = insn->get_method();
      if (callee == DexMethod::get_method("LFoo;.<init>:()V")) {
        invoke_to_eff_summary_map.emplace(insn, side_effects::Summary({0}));
        invoke_to_esc_summary_map.emplace(insn, ptrs::EscapeSummary{});
      } else if (callee == DexMethod::get_method("LFoo;.noReturn:()V")) {
        invoke_to_eff_summary_map.emplace(
            insn, side_effects::Summary(side_effects::EFF_THROWS, {}));
        invoke_to_esc_summary_map.emplace(
            insn, ptrs::EscapeSummary(ptrs::ParamSet::bottom(), {}));
      }
    }
  }
  auto fp_iter =
      analyze(*code, invoke_to_esc_summary_map, invoke_to_eff_summary_map);
  optimize(*fp_iter, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}
