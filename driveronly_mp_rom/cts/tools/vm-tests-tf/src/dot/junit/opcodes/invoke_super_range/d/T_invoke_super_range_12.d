; Copyright (C) 2008 The Android Open Source Project
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;      http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.

.source T_invoke_super_range_12.java
.class public dot.junit.opcodes.invoke_super_range.d.T_invoke_super_range_12
.super dot/junit/opcodes/invoke_super_range/d/TSuper


.method public <init>()V
.limit regs 2

       invoke-direct {v1}, dot/junit/opcodes/invoke_super_range/d/TSuper/<init>()V
       return-void
.end method


.method public run()V
.limit regs 8
       move-object v4, v5
       invoke-super/range {v4..v5}, dot.junit.opcodes.invoke_super_range.d.T_invoke_super_range_12/testString(Ljava/lang/String;)V

       return-void
.end method

