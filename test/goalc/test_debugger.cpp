#include "gtest/gtest.h"
#include "goalc/compiler/Compiler.h"
#include "test/goalc/framework/test_runner.h"

#ifdef __linux
TEST(Debugger, DebuggerBasicConnect) {
  Compiler compiler;
  // evidently you can't ptrace threads in your own process, so we need to run the runtime in a
  // separate process.
  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string("(dbg)");
    EXPECT_TRUE(compiler.get_debugger().is_valid());
    EXPECT_TRUE(compiler.get_debugger().is_halted());
    compiler.shutdown_target();  // will detach/unhalt, then send the usual shutdown message

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

TEST(Debugger, DebuggerBreakAndContinue) {
  Compiler compiler;
  // evidently you can't ptrace threads in your own process, so we need to run the runtime in a
  // separate process.
  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string("(dbg)");
    EXPECT_TRUE(compiler.get_debugger().is_valid());
    EXPECT_TRUE(compiler.get_debugger().is_halted());
    for (int i = 0; i < 20; i++) {
      EXPECT_TRUE(compiler.get_debugger().do_continue());
      EXPECT_TRUE(compiler.get_debugger().do_break());
    }
    compiler.shutdown_target();

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

TEST(Debugger, DebuggerReadMemory) {
  Compiler compiler;
  // evidently you can't ptrace threads in your own process, so we need to run the runtime in a
  // separate process.
  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string("(dbg)");
    EXPECT_TRUE(compiler.get_debugger().do_continue());
    auto result = compiler.run_test_from_string("\"test_string!\"");
    EXPECT_TRUE(compiler.get_debugger().do_break());
    auto addr = std::stoi(result.at(0));
    u8 str_buff[256];
    compiler.get_debugger().read_memory(str_buff, 256, addr + 4);

    EXPECT_EQ(0, strcmp((char*)str_buff, "test_string!"));
    compiler.shutdown_target();

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

TEST(Debugger, DebuggerWriteMemory) {
  Compiler compiler;
  // evidently you can't ptrace threads in your own process, so we need to run the runtime in a
  // separate process.
  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string("(dbg)");
    EXPECT_TRUE(compiler.get_debugger().do_continue());
    auto result = compiler.run_test_from_string("(define x (the int 123)) 'x");
    EXPECT_TRUE(compiler.get_debugger().do_break());
    auto addr = std::stoi(result.at(0));
    u32 value;
    EXPECT_TRUE(compiler.get_debugger().read_value(&value, addr));
    EXPECT_EQ(value, 123);
    EXPECT_TRUE(compiler.get_debugger().write_value<u32>(456, addr));
    EXPECT_TRUE(compiler.get_debugger().read_value(&value, addr));
    EXPECT_EQ(value, 456);

    EXPECT_TRUE(compiler.get_debugger().do_continue());
    result = compiler.run_test_from_string("x");
    EXPECT_EQ(456, std::stoi(result.at(0)));

    compiler.shutdown_target();

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

TEST(Debugger, Symbol) {
  Compiler compiler;
  // evidently you can't ptrace threads in your own process, so we need to run the runtime in a
  // separate process.
  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string("(dbg)");
    EXPECT_TRUE(compiler.get_debugger().do_continue());
    auto result = compiler.run_test_from_string("(define test-symbol (the int 123))");
    EXPECT_TRUE(compiler.get_debugger().do_break());
    auto addr = compiler.get_debugger().get_symbol_address("test-symbol");
    u32 value;
    EXPECT_TRUE(compiler.get_debugger().read_value(&value, addr));
    EXPECT_EQ(value, 123);
    EXPECT_TRUE(compiler.get_debugger().write_value<u32>(456, addr));
    EXPECT_TRUE(compiler.get_debugger().read_value(&value, addr));
    EXPECT_EQ(value, 456);

    EXPECT_TRUE(compiler.get_debugger().do_continue());
    result = compiler.run_test_from_string("test-symbol");
    EXPECT_EQ(456, std::stoi(result.at(0)));

    compiler.shutdown_target();

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

TEST(Debugger, SimpleBreakpoint) {
  Compiler compiler;

  if (!fork()) {
    GoalTest::runtime_no_kernel();
    exit(0);
  } else {
    compiler.connect_to_target();
    compiler.poke_target();
    compiler.run_test_from_string(
        "(defun fake-function () 0) (defun test-function () (+ 1 2 3 4 5 6)) (defun "
        "fake-function-2 () 0)");
    compiler.run_test_from_string("(dbg)", "a");
    u32 func_addr;
    EXPECT_TRUE(compiler.get_debugger().get_symbol_value("test-function", &func_addr));
    EXPECT_TRUE(compiler.get_debugger().is_valid());
    EXPECT_TRUE(compiler.get_debugger().is_halted());

    compiler.get_debugger().add_addr_breakpoint(func_addr);  // todo from code.
    compiler.run_test_from_string("(:cont)", "a");
    compiler.run_test_from_string("(test-function)", "a");
    // wait for breakpoint to be hit.
    while (!compiler.get_debugger().is_halted()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    compiler.get_debugger().update_break_info();
    auto expected_instr_before_rip = compiler.get_debugger().get_x86_base_addr() + func_addr;
    auto rip = compiler.get_debugger().get_regs().rip;
    // instructions can be at most 15 bytes long.
    EXPECT_TRUE(rip > expected_instr_before_rip && rip < expected_instr_before_rip + 15);

    // check rsp in goal code to make sure the GOAL stack is in the right space.
    auto rsp = compiler.get_debugger().get_regs().gprs[emitter::RSP];
    EXPECT_TRUE(rsp < compiler.get_debugger().get_x86_base_addr() + EE_MAIN_MEM_SIZE);
    EXPECT_TRUE(rsp > compiler.get_debugger().get_x86_base_addr() + EE_MAIN_MEM_SIZE - (16 * 1024));

    EXPECT_TRUE(compiler.get_debugger().is_halted());
    auto bi = compiler.get_debugger().get_cached_break_info();
    EXPECT_TRUE(bi.knows_function);
    EXPECT_TRUE(bi.knows_object);
    EXPECT_TRUE(bi.object_name == "*listener*");
    EXPECT_TRUE(bi.function_name == "test-function");
    auto disasm = compiler.get_debugger().disassemble_at_rip(bi);
    EXPECT_FALSE(disasm.failed);
    // if we change this to be before the break instruction this might need to be 0 in the future.
    EXPECT_EQ(bi.function_offset, 1);

    compiler.get_debugger().remove_addr_breakpoint(func_addr);
    compiler.get_debugger().do_continue();

    auto result = compiler.run_test_from_string("(test-function)");
    EXPECT_EQ(std::stoi(result.at(0)), 21);
    compiler.shutdown_target();

    // and now the child process should be done!
    EXPECT_TRUE(wait(nullptr) >= 0);
  }
}

#endif
