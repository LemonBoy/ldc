// ThinLTO: Test inline assembly functions with thinlto

// REQUIRES: atleast_llvm309
// REQUIRES: LTO

// RUN: %ldc -flto=thin %S/inputs/asm_x86.d -c -of=%t_input%obj
// RUN: %ldc -flto=thin -I%S %s %t_input%obj

// The above test triggered a bug on Linux, but not on OS X. We check here that the
// module with (LLVM module-scope) inline assembly is indeed not emitted as bitcode
// so that this test would fail on OS X too without a fix.
// RUN: %ldc -flto=thin %S/inputs/asm_x86.d -c -of=%t_input%obj -vv | FileCheck %s
// CHECK-NOT: Writing LLVM bitcode

import inputs.asm_x86;

int main() {
    return simplefunction();
}
