; A module the verifier rejects (a block with no terminator, built through the
; C API) gives the verifier's text as the error, exit 1.
; RUN: not llvm-air-write --self-test-broken-module 2>&1 | FileCheck %s

; CHECK: error: Basic Block in function 'f' does not have terminator!
