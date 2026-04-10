# PatchMelonDSArm.cmake — add retrodebug execution hooks to ARM.h and ARM.cpp
# Called from FetchDependencies.cmake after melonDS source is fetched.
# MELONDS_SRC must be set to the melonDS source directory.

# --- Patch ARM.h: add hook fields to ARM class ---
file(READ "${MELONDS_SRC}/src/ARM.h" ARM_H)

# Add fields after "u32 DataCycles;"
string(REPLACE
    "u32 DataCycles;"
    "u32 DataCycles;

    // Retrodebug execution hook: called before each instruction.
    // Returns true to request halt (breakpoint hit).
    bool (*RetroDebugHook)(void *userdata, u32 addr, bool thumb) = nullptr;
    void *RetroDebugUserData = nullptr;
    bool RetroDebugHalt = false;"
    ARM_H "${ARM_H}")

file(WRITE "${MELONDS_SRC}/src/ARM.h" "${ARM_H}")
message(STATUS "  Patched ARM.h")

# --- Patch ARM.cpp: add hook calls in interpreter execute loops ---
file(READ "${MELONDS_SRC}/src/ARM.cpp" ARM_CPP)

# ARMv5 Thumb path: insert hook before GdbCheckC in Thumb block
# Pattern: "if (CPSR & 0x20) // THUMB\n            {\n"
# followed by GdbCheckC or prefetch
string(REPLACE
    "if (CPSR & 0x20) // THUMB
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 2;
                CurInstr = NextInstr[0];"
    "if (CPSR & 0x20) // THUMB
            {
                if (RetroDebugHook)
                {
                    u32 _rdAddr = R[15] - 2;
                    if (RetroDebugHook(RetroDebugUserData, _rdAddr, true))
                    { RetroDebugHalt = true; break; }
                }

                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 2;
                CurInstr = NextInstr[0];"
    ARM_CPP "${ARM_CPP}")

# ARMv5 + ARMv4 ARM path: insert hook before GdbCheckC in ARM block
# Pattern: "else\n            {\n                if constexpr (mode == CPUExecuteMode::InterpreterGDB)\n                    GdbCheckC();\n\n                // prefetch\n                R[15] += 4;"
string(REPLACE
    "else
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 4;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                NextInstr[1] = CodeRead32(R[15], false);"
    "else
            {
                if (RetroDebugHook)
                {
                    u32 _rdAddr = R[15] - 4;
                    if (RetroDebugHook(RetroDebugUserData, _rdAddr, false))
                    { RetroDebugHalt = true; break; }
                }

                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 4;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                NextInstr[1] = CodeRead32(R[15], false);"
    ARM_CPP "${ARM_CPP}")

file(WRITE "${MELONDS_SRC}/src/ARM.cpp" "${ARM_CPP}")
message(STATUS "  Patched ARM.cpp")
