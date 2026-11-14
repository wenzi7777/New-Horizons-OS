#include "VmCore.h"

#include <math.h>
#include <string.h>

#include "FlowMath.h"
#if defined(ESP_PLATFORM)
#include "esp_cpu.h"
#include "esp_attr.h"
#define VM_HOT IRAM_ATTR
#else
#define VM_HOT
#endif

// Bit-for-bit agreement with the App Library's JavaScript VM: no fused
// multiply-add (GCC fuses by default for GNU C++, the JS side never does).
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace nhos {

#if defined(ESP_PLATFORM)
VM_HOT uint32_t vmCycles() { return esp_cpu_get_cycle_count(); }
#else
uint32_t vmCycles() { return 0; }
#endif

namespace {

// Must equal MatrixScanner.h's kPressureActiveThreshold / kPressureFullScale
// and opset.mjs; the conformance test reads both. Repeated here so this file
// stays free of the Arduino headers MatrixScanner.h pulls in.
constexpr float kVmActiveThreshold = 50.0f;
constexpr float kVmFullScale = 2000.0f;
constexpr uint8_t kMaxWindow = 128;
constexpr uint8_t kOledRowsVm = 4;
constexpr uint8_t kMaxOledDigitsVm = 3;
constexpr uint8_t kMaxExtLedsVm = 9;

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline int16_t rdi16(const uint8_t* p) { return static_cast<int16_t>(rd16(p)); }
inline float rdf(const uint8_t* p) {
  float v;
  memcpy(&v, p, sizeof(v));
  return v;
}

const char* const kErrTruncated = "image_truncated";

bool isInt(float v, float lo, float hi) {
  return v >= lo && v <= hi && floorf(v) == v;
}

// Truncation to uint32 as the bit operations define it: NaN and negatives
// give 0, the top saturates.
uint32_t toU32(float x) {
  if (!(x > 0.0f)) return 0;
  if (x >= 4294967295.0f) return 0xFFFFFFFFu;
  return static_cast<uint32_t>(x);
}

bool isSweep(uint8_t op) { return op < kFlowOpCount && kFlowOps[op].sweep; }

uint32_t nativeFuel(uint8_t op, uint16_t cells) {
  if (op == static_cast<uint8_t>(FlowOp::Features)) return 2U * cells;
  return isSweep(op) ? cells : 4U;
}

int8_t b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

uint32_t VmLayout::poolBytes() const {
  return stackAt() + static_cast<uint32_t>(stack) * 4U;
}

size_t vmBase64DecodedLength(const char* text, size_t textLen) {
  if (textLen % 4 != 0) return 0;
  size_t out = textLen / 4 * 3;
  if (textLen >= 1 && text[textLen - 1] == '=') --out;
  if (textLen >= 2 && text[textLen - 2] == '=') --out;
  return out;
}

int32_t vmBase64Decode(const char* text, size_t textLen, uint8_t* out, size_t capacity) {
  if (textLen % 4 != 0) return -1;
  const size_t length = vmBase64DecodedLength(text, textLen);
  if (length > capacity) return -1;
  size_t at = 0;
  for (size_t i = 0; i < textLen; i += 4) {
    int8_t v[4];
    for (uint8_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=') {
        // Padding only in the last group, last one or two characters.
        if (i + 4 != textLen || k < 2 || (k == 2 && text[i + 3] != '=')) return -1;
        v[k] = 0;
      } else {
        v[k] = b64Value(c);
        if (v[k] < 0) return -1;
      }
    }
    const uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                            (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
    const uint8_t bytes[3] = {static_cast<uint8_t>(triple >> 16), static_cast<uint8_t>(triple >> 8),
                              static_cast<uint8_t>(triple)};
    for (uint8_t k = 0; k < 3 && at < length; ++k) out[at++] = bytes[k];
  }
  return static_cast<int32_t>(length);
}

bool vmParseLayout(const uint8_t* b, size_t length, VmLayout& out, const char*& error) {
  out = VmLayout();
  if (length < kVmHeaderBytes || length > 0xFFFF) { error = kErrTruncated; return false; }
  if (b[0] != 0x4E || b[1] != 0x56) { error = "image_bad_magic"; return false; }
  if (b[2] != kVmImageVersion) { error = "image_unsupported_version"; return false; }
  out.length = static_cast<uint16_t>(length);
  out.codeLen = rd16(b + 4);
  out.constCount = rd16(b + 6);
  out.arena = rd16(b + 8);
  out.stack = rd16(b + 10);
  out.fnCount = b[12];
  out.entryCount = b[13];
  out.initCount = b[14];
  out.stringCount = b[15];
  out.nodeCount = b[16];
  out.stringBytes = rd16(b + 18);
  if (out.codeLen == 0 || out.codeLen > kVmMaxCode) { error = "image_code_size"; return false; }
  if (out.constCount > kVmMaxConsts) { error = "image_too_many_consts"; return false; }
  if (out.arena > kVmMaxArena) { error = "image_arena_size"; return false; }
  if (out.stack == 0 || out.stack > kVmMaxStack) { error = "image_stack_size"; return false; }
  if (out.fnCount == 0 || out.fnCount > kVmMaxFunctions) { error = "image_functions"; return false; }
  if (out.stringCount > kVmMaxStrings) { error = "image_too_many_strings"; return false; }
  if (out.nodeCount > kVmMaxNodes) { error = "image_too_many_nodes"; return false; }
  uint32_t at = kVmHeaderBytes;
  out.fnAt = static_cast<uint16_t>(at);
  at += out.fnCount * 8U;
  out.entryAt = static_cast<uint16_t>(at);
  at += out.entryCount * 2U;
  out.initAt = static_cast<uint16_t>(at);
  at += out.initCount * 4U;
  out.nodesAt = static_cast<uint16_t>(at);
  at += out.nodeCount * 4U;
  out.stringsAt = static_cast<uint16_t>(at);
  at += out.stringBytes;
  const uint32_t constsAt = (at + 3U) & ~3U;
  const uint32_t codeAt = constsAt + out.constCount * 4U;
  if (codeAt + out.codeLen != length) { error = "image_size_mismatch"; return false; }
  for (uint32_t i = at; i < constsAt; ++i) {
    if (b[i] != 0) { error = "image_bad_padding"; return false; }
  }
  out.constsAt = static_cast<uint16_t>(constsAt);
  out.codeAt = static_cast<uint16_t>(codeAt);

  for (uint8_t i = 0; i < out.fnCount; ++i) {
    if (b[out.fnAt + i * 8U + 7] != 0) { error = "image_bad_function"; return false; }
  }
  for (uint8_t i = 0; i < out.entryCount; ++i) {
    const uint8_t event = b[out.entryAt + i * 2U];
    const uint8_t fn = b[out.entryAt + i * 2U + 1];
    if (event >= kVmEventCount || fn >= out.fnCount) { error = "image_bad_entry"; return false; }
    for (uint8_t j = 0; j < i; ++j) {
      if (b[out.entryAt + j * 2U] == event) { error = "image_bad_entry"; return false; }
    }
  }
  for (uint8_t i = 0; i < out.initCount; ++i) {
    const uint16_t slot = rd16(b + out.initAt + i * 4U);
    const uint16_t k = rd16(b + out.initAt + i * 4U + 2);
    if (slot >= out.arena || k >= out.constCount) { error = "image_bad_init"; return false; }
  }
  for (uint8_t i = 0; i < out.nodeCount; ++i) {
    const uint8_t* n = b + out.nodesAt + i * 4U;
    if (n[0] < 1 || n[0] >= kFlowOpCount || (n[1] & ~3) != 0 || rd16(n + 2) >= out.arena) {
      error = "image_bad_node";
      return false;
    }
  }
  uint32_t s = out.stringsAt;
  const uint32_t stringsEnd = out.stringsAt + out.stringBytes;
  for (uint8_t i = 0; i < out.stringCount; ++i) {
    if (s >= stringsEnd) { error = "image_bad_strings"; return false; }
    const uint8_t len = b[s];
    if (len > kVmMaxString || s + len + 2U > stringsEnd) { error = "image_bad_strings"; return false; }
    for (uint8_t c = 0; c < len; ++c) {
      const uint8_t ch = b[s + 1 + c];
      if (ch < 0x20 || ch > 0x7E) { error = "image_bad_strings"; return false; }
    }
    if (b[s + 1 + len] != 0) { error = "image_bad_strings"; return false; }
    s += len + 2U;
  }
  if (s != stringsEnd) { error = "image_bad_strings"; return false; }
  return true;
}

namespace {

struct Insn {
  uint8_t opcode;
  uint8_t size;
  int32_t a, b, c;
};

bool decode(const uint8_t* code, uint16_t codeLen, uint16_t pc, Insn& out) {
  if (pc >= codeLen) return false;
  out.opcode = code[pc];
  if (out.opcode >= kVmOpCount) return false;
  const VmOpInfo& info = kVmOps[out.opcode];
  out.size = info.size;
  if (pc + out.size > codeLen) return false;
  out.a = out.b = out.c = 0;
  switch (info.operands) {
    case 1: out.a = code[pc + 1]; break;
    case 2: out.a = rd16(code + pc + 1); break;
    case 3: out.a = rdi16(code + pc + 1); break;
    case 4: out.a = rd16(code + pc + 1); out.b = rd16(code + pc + 3); break;
    case 5: out.a = code[pc + 1]; out.b = rd16(code + pc + 2); out.c = rd16(code + pc + 4); break;
    default: break;
  }
  return true;
}

struct Checker {
  const uint8_t* img;
  const VmLayout& L;
  uint16_t declared;
  float k(uint16_t i) const { return rdf(img + L.constsAt + i * 4U); }
  uint8_t stringLen(uint16_t index) const {
    uint32_t s = L.stringsAt;
    for (uint16_t i = 0; i < index; ++i) s += img[s] + 2U;
    return img[s];
  }

  // A NAT's operands, docs/vm.md §9 rule 4-5.
  bool native(int32_t op, int32_t state, int32_t param, uint16_t& usedCaps, const char*& error) const {
    if (op <= 0 || op >= kFlowOpCount || kVmNatives[op].params == 0xFF) { error = "unknown_native"; return false; }
    const FlowOp fop = static_cast<FlowOp>(op);
    const uint16_t cap = kFlowOps[op].capability;
    if (cap != 0 && (declared & cap) == 0) { error = "capability_not_declared"; return false; }
    usedCaps |= cap;
    const uint8_t params = kVmNatives[op].params;
    if (param + params > L.constCount) { error = "const_out_of_range"; return false; }
    uint32_t slots = kVmNatives[op].state;
    const auto P = [&](uint8_t i) { return k(static_cast<uint16_t>(param + i)); };
    const auto stringOk = [&](float v, bool nonEmpty) {
      return isInt(v, 0, static_cast<float>(L.stringCount) - 1) &&
             (!nonEmpty || stringLen(static_cast<uint16_t>(v)) > 0);
    };
    switch (fop) {
      case FlowOp::RegionSum:
      case FlowOp::RegionPeak:
      case FlowOp::RegionActive:
      case FlowOp::RegionRowCentroid:
      case FlowOp::RegionColCentroid: {
        for (uint8_t i = 0; i < 4; ++i) {
          if (!isInt(P(i), 0, 255)) { error = "invalid_region"; return false; }
        }
        const float rel = P(4);
        if (!isInt(rel, 0, kRegionRelRows | kRegionRelCols)) { error = "invalid_region_rel"; return false; }
        const uint8_t bits = static_cast<uint8_t>(rel);
        if (((bits & kRegionRelRows) != 0 && (P(2) > kMaxRegionPercent || P(0) > P(2))) ||
            ((bits & kRegionRelCols) != 0 && (P(3) > kMaxRegionPercent || P(1) > P(3)))) {
          error = "invalid_region_percent";
          return false;
        }
        if (!isfinite(P(5))) { error = "invalid_param"; return false; }
        break;
      }
      case FlowOp::ActiveCells:
        if (!isfinite(P(0))) { error = "invalid_param"; return false; }
        break;
      case FlowOp::Threshold:
        if (!isfinite(P(0)) || !isfinite(P(1))) { error = "invalid_param"; return false; }
        break;
      case FlowOp::Debounce:
        if (!isInt(P(0), 0, 65535)) { error = "invalid_ms"; return false; }
        break;
      case FlowOp::Emit:
        if (!stringOk(P(0), true)) { error = "invalid_string"; return false; }
        break;
      case FlowOp::EmitValue:
        if (!stringOk(P(0), true)) { error = "invalid_string"; return false; }
        if (P(1) != 0 && P(1) != 1) { error = "invalid_flag"; return false; }
        break;
      case FlowOp::Mean:
      case FlowOp::MaxHold:
      case FlowOp::Integrate:
        if (!isInt(P(0), 1, kMaxWindow)) { error = "invalid_window"; return false; }
        slots += static_cast<uint32_t>(P(0));
        break;
      case FlowOp::Counter:
      case FlowOp::CounterReset:
        if (P(0) != 0 && P(0) != 1) { error = "invalid_flag"; return false; }
        break;
      case FlowOp::Led:
        for (uint8_t i = 0; i < 3; ++i) {
          if (!isInt(P(i), 0, 255)) { error = "invalid_colour"; return false; }
        }
        break;
      case FlowOp::OledText:
        if (!isInt(P(0), 0, kOledRowsVm - 1)) { error = "invalid_oled_row"; return false; }
        if (!stringOk(P(1), false)) { error = "invalid_string"; return false; }
        if (!isInt(P(2), 0, kMaxOledDigitsVm)) { error = "invalid_oled_digits"; return false; }
        break;
      case FlowOp::OledBar:
        if (!isInt(P(0), 0, kOledRowsVm - 1)) { error = "invalid_oled_row"; return false; }
        if (!stringOk(P(1), false)) { error = "invalid_string"; return false; }
        if (!(P(3) > P(2))) { error = "invalid_range"; return false; }
        break;
      case FlowOp::ExtPixel:
        if (!isInt(P(0), 0, kMaxExtLedsVm - 1)) { error = "invalid_ext_led_index"; return false; }
        for (uint8_t i = 1; i < 4; ++i) {
          if (!isInt(P(i), 0, 255)) { error = "invalid_colour"; return false; }
        }
        break;
      case FlowOp::ExtMeter:
        if (!(P(1) > P(0))) { error = "invalid_range"; return false; }
        break;
      case FlowOp::Imu:
        if (!isInt(P(0), 0, static_cast<float>(ImuField::Count) - 1)) { error = "invalid_field"; return false; }
        break;
      case FlowOp::Mag:
        if (!isInt(P(0), 0, static_cast<float>(MagField::Count) - 1)) { error = "invalid_field"; return false; }
        break;
      default:
        break;
    }
    if (static_cast<uint32_t>(state) + slots > L.arena) { error = "arena_out_of_range"; return false; }
    return true;
  }
};

}  // namespace

bool vmVerify(const uint8_t* img, size_t length, uint16_t declaredCaps, uint16_t cellCount,
              VmVerified& out, const char*& error) {
  out = VmVerified();
  VmLayout& L = out.layout;
  if (!vmParseLayout(img, length, L, error)) return false;
  const uint8_t* code = img + L.codeAt;
  VmFunctionInfo fns[kVmMaxFunctions];
  for (uint8_t f = 0; f < L.fnCount; ++f) {
    const uint8_t* p = img + L.fnAt + f * 8U;
    fns[f] = {rd16(p), rd16(p + 2), p[4], p[5], p[6]};
    const VmFunctionInfo& fn = fns[f];
    if ((f == 0 && fn.pc != 0) || (f > 0 && fn.pc <= fns[f - 1].pc) || fn.pc >= L.codeLen ||
        fn.args > fn.locals || fn.rets > 1 || static_cast<uint32_t>(fn.base) + fn.locals > L.arena) {
      error = "image_bad_function";
      return false;
    }
  }
  for (uint8_t i = 0; i < L.entryCount; ++i) {
    const VmFunctionInfo& fn = fns[img[L.entryAt + i * 2U + 1]];
    if (fn.args != 0 || fn.rets != 0) { error = "image_bad_entry"; return false; }
  }

  // Scratch: one byte of stack depth per code byte (0xFF = not an
  // instruction start, 0xFE = not reached yet), and a work list. Transient,
  // install/bind time only.
  uint8_t* depth = new uint8_t[L.codeLen];
  uint16_t* work = new uint16_t[L.codeLen];
  const Checker check{img, L, declaredCaps};
  bool ok = true;
  bool loopFree = true;
  uint64_t estimatedNs = 0;
  uint16_t used = 0;
  uint8_t ownMax[kVmMaxFunctions] = {0};
  // Call sites: callee and the depth at the call, per function, flattened.
  struct Site { uint8_t from; uint8_t callee; uint8_t depth; };
  Site* sites = new Site[L.codeLen / 3 + 1];
  uint16_t siteCount = 0;

  for (uint8_t f = 0; f < L.fnCount && ok; ++f) {
    const uint16_t start = fns[f].pc;
    const uint16_t end = f + 1 < L.fnCount ? fns[f + 1].pc : L.codeLen;
    const VmFunctionInfo& fn = fns[f];
    for (uint16_t i = start; i < end; ++i) depth[i] = 0xFF;
    Insn insn{};
    uint8_t lastOpcode = 0xFF;
    for (uint16_t pc = start; pc < end; pc = static_cast<uint16_t>(pc + insn.size)) {
      if (!decode(code, L.codeLen, pc, insn)) { error = "bad_opcode"; ok = false; break; }
      if (pc + insn.size > end) { error = "insn_crosses_function"; ok = false; break; }
      depth[pc] = 0xFE;
    }
    if (!ok) break;
    // Operands.
    for (uint16_t pc = start; pc < end && ok; pc = static_cast<uint16_t>(pc + insn.size)) {
      decode(code, L.codeLen, pc, insn);
      lastOpcode = insn.opcode;
      switch (static_cast<VmOp>(insn.opcode)) {
        case VmOp::Pushk: if (insn.a >= L.constCount) { error = "const_out_of_range"; ok = false; } break;
        case VmOp::Loadg:
        case VmOp::Storeg: if (insn.a >= L.arena) { error = "arena_out_of_range"; ok = false; } break;
        case VmOp::Loadl:
        case VmOp::Storel: if (insn.a >= fn.locals) { error = "local_out_of_range"; ok = false; } break;
        case VmOp::Aload:
        case VmOp::Astore:
          if (insn.b < 1 || insn.a + insn.b > L.arena) { error = "arena_out_of_range"; ok = false; }
          break;
        case VmOp::Jmp:
        case VmOp::Jz:
        case VmOp::Jnz: {
          const int32_t target = pc + insn.size + insn.a;
          if (target < start || target >= end || depth[target] == 0xFF) { error = "bad_jump"; ok = false; }
          if (target <= pc) loopFree = false;
          break;
        }
        case VmOp::Call: if (insn.a >= L.fnCount) { error = "bad_call"; ok = false; } break;
        case VmOp::Ret: if (insn.a != fn.rets) { error = "bad_return"; ok = false; } break;
        case VmOp::Sys: {
          if (insn.a >= kVmSyscallCount) { error = "unknown_syscall"; ok = false; break; }
          const uint16_t cap = kVmSyscalls[insn.a].capability;
          if (cap != 0 && (declaredCaps & cap) == 0) { error = "capability_not_declared"; ok = false; }
          used |= cap;
          break;
        }
        case VmOp::Nat:
          if (!check.native(insn.a, insn.b, insn.c, used, error)) ok = false;
          break;
        default:
          break;
      }
      if (!ok) break;
      if (static_cast<VmOp>(insn.opcode) == VmOp::Nat) {
        const FlowOpInfo& info = kFlowOps[insn.a];
        estimatedNs += info.sweep ? static_cast<uint64_t>(cellCount) * info.nsPerCell : info.nsFlat;
      } else {
        estimatedNs += kVmInsnNs;
      }
    }
    if (!ok) break;
    if (lastOpcode != static_cast<uint8_t>(VmOp::End) && lastOpcode != static_cast<uint8_t>(VmOp::Ret) &&
        lastOpcode != static_cast<uint8_t>(VmOp::Jmp)) {
      error = "falls_off_function";
      ok = false;
      break;
    }

    // Stack depth by abstract interpretation: one depth per instruction.
    uint16_t top = 0;
    depth[start] = 0;
    work[top++] = start;
    uint8_t max = 0;
    while (top > 0 && ok) {
      const uint16_t pc = work[--top];
      decode(code, L.codeLen, pc, insn);
      const uint8_t d = depth[pc];
      const VmOp op = static_cast<VmOp>(insn.opcode);
      int32_t pops = kVmOps[insn.opcode].pops;
      int32_t pushes = kVmOps[insn.opcode].pushes;
      if (op == VmOp::Call) {
        pops = fns[insn.a].args;
        pushes = fns[insn.a].rets;
        sites[siteCount++] = {f, static_cast<uint8_t>(insn.a), d};
      } else if (op == VmOp::Ret) {
        pops = insn.a;
        pushes = 0;
        if (d != pops) { error = "stack_unbalanced_at_return"; ok = false; break; }
      } else if (op == VmOp::Nat) {
        pops = kFlowOps[insn.a].inputs;
      } else if (op == VmOp::Sys) {
        pops = kVmSyscalls[insn.a].args;
        pushes = kVmSyscalls[insn.a].results;
      } else if (op == VmOp::End) {
        if (d != 0) { error = "stack_unbalanced_at_end"; ok = false; break; }
      }
      if (d < pops) { error = "stack_underflow"; ok = false; break; }
      const int32_t after = d - pops + pushes;
      if (after > kVmMaxStack) { error = "stack_too_small"; ok = false; break; }
      if (after > max) max = static_cast<uint8_t>(after);
      if (d > max) max = d;
      int32_t next[2];
      uint8_t nextCount = 0;
      if (op == VmOp::Jmp) {
        next[nextCount++] = pc + insn.size + insn.a;
      } else if (op == VmOp::Jz || op == VmOp::Jnz) {
        next[nextCount++] = pc + insn.size + insn.a;
        next[nextCount++] = pc + insn.size;
      } else if (op != VmOp::End && op != VmOp::Ret) {
        next[nextCount++] = pc + insn.size;
      }
      for (uint8_t n = 0; n < nextCount; ++n) {
        const int32_t target = next[n];
        if (target >= end || depth[target] == 0xFF) { error = "falls_off_function"; ok = false; break; }
        if (depth[target] == 0xFE) {
          depth[target] = static_cast<uint8_t>(after);
          work[top++] = static_cast<uint16_t>(target);
        } else if (depth[target] != after) {
          error = "stack_inconsistent";
          ok = false;
          break;
        }
      }
    }
    ownMax[f] = max;
  }

  // Call graph: acyclic and at most kVmMaxCallDepth deep, callee depth folded
  // in. Functions are few (<= 32): iterate to a fixed point, which also finds
  // a cycle as a level that keeps growing.
  uint8_t total[kVmMaxFunctions];
  uint8_t level[kVmMaxFunctions];
  for (uint8_t f = 0; f < L.fnCount; ++f) {
    total[f] = ownMax[f];
    level[f] = 0;
  }
  for (uint8_t round = 0; ok && round <= L.fnCount + 1; ++round) {
    bool changed = false;
    for (uint16_t i = 0; i < siteCount; ++i) {
      const Site& site = sites[i];
      const int32_t deep = site.depth - fns[site.callee].args + total[site.callee];
      if (deep > total[site.from]) {
        if (deep > kVmMaxStack) { error = "stack_too_small"; ok = false; break; }
        total[site.from] = static_cast<uint8_t>(deep);
        changed = true;
      }
      if (level[site.callee] + 1 > level[site.from]) {
        level[site.from] = static_cast<uint8_t>(level[site.callee] + 1);
        changed = true;
        if (level[site.from] > L.fnCount) { error = "recursion"; ok = false; break; }
      }
    }
    if (!changed) break;
    if (round == L.fnCount + 1) { error = "recursion"; ok = false; }
  }
  uint8_t maxStack = 0;
  for (uint8_t f = 0; ok && f < L.fnCount; ++f) {
    if (level[f] > kVmMaxCallDepth) { error = "call_too_deep"; ok = false; }
    if (total[f] > maxStack) maxStack = total[f];
  }
  if (ok && maxStack > L.stack) { error = "stack_too_small"; ok = false; }

  delete[] depth;
  delete[] work;
  delete[] sites;
  if (!ok) return false;
  out.loopFree = loopFree;
  out.estimatedNs = loopFree ? static_cast<uint32_t>(estimatedNs > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : estimatedNs) : 0;
  out.maxStack = maxStack;
  out.usedCapabilities = used;
  return true;
}

// --- machine -------------------------------------------------------------------

void VmMachine::bind(uint8_t* memory, const VmLayout& layout) {
  mem_ = memory;
  layout_ = layout;
  reset();
}

void VmMachine::unbind() {
  mem_ = nullptr;
  layout_ = VmLayout();
  suspended_ = false;
  fault_ = 0;
  ledHeld_ = false;
}

VM_HOT VmFunctionInfo VmMachine::function(uint8_t index) const {
  const uint8_t* p = mem_ + layout_.fnAt + index * 8U;
  return {rd16(p), rd16(p + 2), p[4], p[5], p[6]};
}

float VmMachine::slot(uint16_t index) const {
  return index < layout_.arena ? arena()[index] : 0.0f;
}

void VmMachine::setSlot(uint16_t index, float value) {
  if (index < layout_.arena) arena()[index] = value;
}

VM_HOT float VmMachine::constant(uint16_t index) const {
  return index < layout_.constCount ? rdf(mem_ + layout_.constsAt + index * 4U) : 0.0f;
}

const char* VmMachine::string(uint16_t index) const {
  if (mem_ == nullptr || index >= layout_.stringCount) return "";
  uint32_t s = layout_.stringsAt;
  for (uint16_t i = 0; i < index; ++i) s += mem_[s] + 2U;
  return reinterpret_cast<const char*>(mem_ + s + 1);
}

VmNodeEntry VmMachine::node(uint8_t index) const {
  if (mem_ == nullptr || index >= layout_.nodeCount) return {0, 0, 0};
  const uint8_t* n = mem_ + layout_.nodesAt + index * 4U;
  return {n[0], n[1], rd16(n + 2)};
}

bool VmMachine::natAt(uint16_t pc, uint8_t& op, uint16_t& state, uint16_t& param) const {
  if (mem_ == nullptr || pc == kNoPc || pc + 6U > layout_.codeLen) return false;
  const uint8_t* c = mem_ + layout_.codeAt + pc;
  if (c[0] != static_cast<uint8_t>(VmOp::Nat)) return false;
  op = c[1];
  state = rd16(c + 2);
  param = rd16(c + 4);
  return true;
}

void VmMachine::reset() {
  if (mem_ == nullptr) return;
  memset(arena(), 0, static_cast<size_t>(layout_.arena) * 4U);
  for (uint8_t i = 0; i < layout_.initCount; ++i) {
    const uint8_t* p = mem_ + layout_.initAt + i * 4U;
    arena()[rd16(p)] = constant(rd16(p + 2));
  }
  pc_ = 0;
  sp_ = 0;
  fn_ = 0;
  callDepth_ = 0;
  suspended_ = false;
  fault_ = 0;
  runDegraded_ = false;
  pressed_ = false;
  persistDirty_ = false;
  budgetLoad_ = 0;
  graceLeft_ = 0;
  for (uint8_t i = 0; i < 4; ++i) displayPc_[i] = kNoPc;
  for (uint8_t i = 0; i < kPixels; ++i) pixelPc_[i] = kNoPc;
  meterPc_ = kNoPc;
  ledHeld_ = false;
  ledRgb_[0] = ledRgb_[1] = ledRgb_[2] = 0;
}

bool VmMachine::hasEntry(VmEvent event) const {
  if (mem_ == nullptr) return false;
  for (uint8_t i = 0; i < layout_.entryCount; ++i) {
    if (mem_[layout_.entryAt + i * 2U] == static_cast<uint8_t>(event)) return true;
  }
  return false;
}

VM_HOT void VmMachine::begin(VmEvent event, bool pressed) {
  uint8_t fn = 0;
  for (uint8_t i = 0; i < layout_.entryCount; ++i) {
    if (mem_[layout_.entryAt + i * 2U] == static_cast<uint8_t>(event)) fn = mem_[layout_.entryAt + i * 2U + 1];
  }
  fn_ = fn;
  pc_ = function(fn).pc;
  sp_ = 0;
  callDepth_ = 0;
  runDegraded_ = false;
  pressed_ = pressed;
  for (uint8_t i = 0; i < 4; ++i) displayPc_[i] = kNoPc;
  for (uint8_t i = 0; i < kPixels; ++i) pixelPc_[i] = kNoPc;
  meterPc_ = kNoPc;
  suspended_ = true;
}

VM_HOT VmRunResult VmMachine::run(const VmContext& ctx, VmHostSink& host, uint32_t fuelLimit,
                           uint32_t (*clock)(), uint32_t deadlineUs) {
  const uint8_t* code = mem_ + layout_.codeAt;
  float* f = arena();
  float* st = stack();
  uint16_t pc = pc_;
  uint8_t sp = sp_;
  uint32_t fuel = 0;
  uint32_t nextCheck = kVmFuelCheck;
  const uint8_t* constBase = mem_ + layout_.constsAt;

#define VM_FAULT(code_)        \
  do {                         \
    fault_ = (code_);          \
    suspended_ = false;        \
    pc_ = pc;                  \
    sp_ = sp;                  \
    return VmRunResult::Fault; \
  } while (0)

  for (;;) {
    if (fuel >= fuelLimit) {
      pc_ = pc;
      sp_ = sp;
      return VmRunResult::Suspended;
    }
    if (clock != nullptr && fuel >= nextCheck) {
      nextCheck = fuel + kVmFuelCheck;
      if (static_cast<int32_t>(clock() - deadlineUs) >= 0) {
        pc_ = pc;
        sp_ = sp;
        return VmRunResult::Suspended;
      }
    }
    const uint8_t opcode = code[pc];
    ++fuel;
#if NHOS_VM_PROFILE
    ++prof.insns;
#endif
    switch (static_cast<VmOp>(opcode)) {
      case VmOp::End:
        pc_ = pc;
        sp_ = 0;
        callDepth_ = 0;
        suspended_ = false;
        return VmRunResult::Done;
      case VmOp::Pushk: st[sp++] = rdf(constBase + rd16(code + pc + 1) * 4U); pc += 3; break;
      case VmOp::Push0: st[sp++] = 0.0f; pc += 1; break;
      case VmOp::Push1: st[sp++] = 1.0f; pc += 1; break;
      case VmOp::Pop: --sp; pc += 1; break;
      case VmOp::Dup: st[sp] = st[sp - 1]; ++sp; pc += 1; break;
      case VmOp::Swap: { const float t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; pc += 1; break; }
      case VmOp::Loadg: st[sp++] = f[rd16(code + pc + 1)]; pc += 3; break;
      case VmOp::Storeg: f[rd16(code + pc + 1)] = st[--sp]; pc += 3; break;
      case VmOp::Loadl: st[sp++] = f[function(fn_).base + code[pc + 1]]; pc += 2; break;
      case VmOp::Storel: f[function(fn_).base + code[pc + 1]] = st[--sp]; pc += 2; break;
      case VmOp::Aload: {
        const uint16_t base = rd16(code + pc + 1);
        const uint16_t len = rd16(code + pc + 3);
        const float i = st[sp - 1];
        if (!(i >= 0.0f && i < static_cast<float>(len)) || floorf(i) != i) VM_FAULT(1);
        st[sp - 1] = f[base + static_cast<uint16_t>(i)];
        pc += 5;
        break;
      }
      case VmOp::Astore: {
        const uint16_t base = rd16(code + pc + 1);
        const uint16_t len = rd16(code + pc + 3);
        const float v = st[--sp];
        const float i = st[--sp];
        if (!(i >= 0.0f && i < static_cast<float>(len)) || floorf(i) != i) VM_FAULT(1);
        f[base + static_cast<uint16_t>(i)] = v;
        pc += 5;
        break;
      }
      case VmOp::Add: --sp; st[sp - 1] = st[sp - 1] + st[sp]; pc += 1; break;
      case VmOp::Sub: --sp; st[sp - 1] = st[sp - 1] - st[sp]; pc += 1; break;
      case VmOp::Mul: --sp; st[sp - 1] = st[sp - 1] * st[sp]; pc += 1; break;
      case VmOp::Div: --sp; st[sp - 1] = st[sp] != 0.0f ? st[sp - 1] / st[sp] : 0.0f; pc += 1; break;
      case VmOp::Mod: --sp; st[sp - 1] = st[sp] != 0.0f ? fmodf(st[sp - 1], st[sp]) : 0.0f; pc += 1; break;
      case VmOp::Neg: st[sp - 1] = -st[sp - 1]; pc += 1; break;
      case VmOp::Abs: st[sp - 1] = fabsf(st[sp - 1]); pc += 1; break;
      case VmOp::Min: { --sp; const float a = st[sp - 1], b = st[sp]; st[sp - 1] = a < b ? a : b; pc += 1; break; }
      case VmOp::Max: { --sp; const float a = st[sp - 1], b = st[sp]; st[sp - 1] = a > b ? a : b; pc += 1; break; }
      case VmOp::Floor: st[sp - 1] = floorf(st[sp - 1]); pc += 1; break;
      case VmOp::Sqrt: st[sp - 1] = flowSqrt(st[sp - 1]); pc += 1; break;
      case VmOp::Atan2: --sp; st[sp - 1] = flowAtan2Deg(st[sp - 1], st[sp]); pc += 1; break;
      case VmOp::Clamp: {
        sp -= 2;
        float v = st[sp - 1];
        const float lo = st[sp], hi = st[sp + 1];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        st[sp - 1] = v;
        pc += 1;
        break;
      }
      case VmOp::Lt: --sp; st[sp - 1] = st[sp - 1] < st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Le: --sp; st[sp - 1] = st[sp - 1] <= st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Gt: --sp; st[sp - 1] = st[sp - 1] > st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Ge: --sp; st[sp - 1] = st[sp - 1] >= st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Eq: --sp; st[sp - 1] = st[sp - 1] == st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Ne: --sp; st[sp - 1] = st[sp - 1] != st[sp] ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Not: st[sp - 1] = st[sp - 1] != 0.0f ? 0.0f : 1.0f; pc += 1; break;
      case VmOp::And: --sp; st[sp - 1] = (st[sp - 1] != 0.0f && st[sp] != 0.0f) ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Or: --sp; st[sp - 1] = (st[sp - 1] != 0.0f || st[sp] != 0.0f) ? 1.0f : 0.0f; pc += 1; break;
      case VmOp::Band: --sp; st[sp - 1] = static_cast<float>(toU32(st[sp - 1]) & toU32(st[sp])); pc += 1; break;
      case VmOp::Bor: --sp; st[sp - 1] = static_cast<float>(toU32(st[sp - 1]) | toU32(st[sp])); pc += 1; break;
      case VmOp::Bxor: --sp; st[sp - 1] = static_cast<float>(toU32(st[sp - 1]) ^ toU32(st[sp])); pc += 1; break;
      case VmOp::Shl: --sp; st[sp - 1] = static_cast<float>(toU32(st[sp - 1]) << (toU32(st[sp]) & 31U)); pc += 1; break;
      case VmOp::Shr: --sp; st[sp - 1] = static_cast<float>(toU32(st[sp - 1]) >> (toU32(st[sp]) & 31U)); pc += 1; break;
      case VmOp::Jmp: pc = static_cast<uint16_t>(pc + 3 + rdi16(code + pc + 1)); break;
      case VmOp::Jz: { const float v = st[--sp]; pc = static_cast<uint16_t>(pc + 3 + (v == 0.0f ? rdi16(code + pc + 1) : 0)); break; }
      case VmOp::Jnz: { const float v = st[--sp]; pc = static_cast<uint16_t>(pc + 3 + (v != 0.0f ? rdi16(code + pc + 1) : 0)); break; }
      case VmOp::Call: {
        const uint8_t callee = static_cast<uint8_t>(rd16(code + pc + 1));
        const VmFunctionInfo fn = function(callee);
        if (callDepth_ >= kVmMaxCallDepth) VM_FAULT(2);
        calls_[callDepth_++] = {static_cast<uint16_t>(pc + 3), fn_};
        for (uint8_t k = 0; k < fn.locals; ++k) f[fn.base + k] = 0.0f;
        for (int16_t k = static_cast<int16_t>(fn.args) - 1; k >= 0; --k) f[fn.base + k] = st[--sp];
        fn_ = callee;
        pc = fn.pc;
        break;
      }
      case VmOp::Ret: {
        if (callDepth_ == 0) VM_FAULT(3);
        const Frame back = calls_[--callDepth_];
        fn_ = back.fn;
        pc = back.pc;
        break;
      }
      case VmOp::Nat: {
        const uint8_t op = code[pc + 1];
        const uint16_t state = rd16(code + pc + 2);
        const uint16_t param = rd16(code + pc + 4);
        sp = static_cast<uint8_t>(sp - kFlowOps[op].inputs);
        fuel += nativeFuel(op, ctx.values != nullptr ? ctx.cells : 0) - 1;
#if NHOS_VM_PROFILE
        const uint32_t n0 = vmCycles();
#endif
        native(op, state, param, st + sp, ctx, host, pc);
#if NHOS_VM_PROFILE
        const uint32_t dn = vmCycles() - n0;
        prof.natCyc += dn;
        ++prof.nats;
        prof.opCyc[op] += dn;
        ++prof.opCalls[op];
#endif
        st[sp++] = f[state];
        pc += 6;
        break;
      }
      case VmOp::Sys: {
        const uint16_t id = rd16(code + pc + 1);
        const VmSyscallInfo& info = kVmSyscalls[id];
        sp = static_cast<uint8_t>(sp - info.args);
        float result = 0.0f;
        if (!syscall(id, st + sp, result, ctx, host)) VM_FAULT(4);
        if (info.results > 0) st[sp++] = result;
        fuel += info.fuel - 1U;
        pc += 3;
        break;
      }
      case VmOp::Degraded: runDegraded_ = true; pc += 1; break;
      default:
        VM_FAULT(8);
    }
  }
#undef VM_FAULT
}

VM_HOT bool VmMachine::syscall(uint16_t id, float* args, float& result, const VmContext& ctx,
                        VmHostSink& host) {
  switch (id) {
    case 0: result = static_cast<float>(ctx.nowMs % 16777216U); return true;
    case 1: result = static_cast<float>(ctx.frameSeq % 16777216U); return true;
    case 2: result = ctx.values != nullptr ? static_cast<float>(ctx.rows) : 0.0f; return true;
    case 3: result = ctx.values != nullptr ? static_cast<float>(ctx.cols) : 0.0f; return true;
    case 4: {
      if (ctx.values == nullptr) { result = 0.0f; return true; }
      const float r = args[0], c = args[1];
      if (!(r >= 0.0f && r < ctx.rows && c >= 0.0f && c < ctx.cols) || floorf(r) != r || floorf(c) != c) {
        return false;
      }
      const uint32_t index = static_cast<uint32_t>(r) * ctx.cols + static_cast<uint32_t>(c);
      result = index < ctx.cells ? ctx.values[index] : 0.0f;
      return true;
    }
    case 5:
    case 6: {
      const float s = args[0];
      if (!isInt(s, 0, static_cast<float>(layout_.stringCount) - 1)) return false;
      if (id == 5) host.vmEmitValue(string(static_cast<uint16_t>(s)), args[1]);
      else host.vmLog(string(static_cast<uint16_t>(s)), args[1]);
      result = 0.0f;
      return true;
    }
    default:
      return false;
  }
}

// One native operator: what FlowApp::evaluate did for that op, on its state
// record (`s` is slot 0, the op's value) with its parameter record at `p`.
// Inputs arrive in `in`. The JavaScript twin is VmMachine.native in vm.mjs.
VM_HOT void VmMachine::native(uint8_t op, uint16_t s, uint16_t p, const float* in, const VmContext& ctx,
                       VmHostSink& host, uint16_t pc) {
  float* f = arena();
  const auto K = [&](uint16_t i) __attribute__((always_inline)) { return constant(static_cast<uint16_t>(p + i)); };
  const auto U = [&](uint16_t i) __attribute__((always_inline)) {
    uint32_t v;
    memcpy(&v, &f[s + i], sizeof(v));
    return v;
  };
  const auto setU = [&](uint16_t i, uint32_t v) __attribute__((always_inline)) { memcpy(&f[s + i], &v, sizeof(v)); };
  // Only the inputs the op takes are on the stack; never read past them.
  const uint8_t inputs = kFlowOps[op].inputs;
  const float a = inputs >= 1 ? in[0] : 0.0f;
  const float b = inputs >= 2 ? in[1] : 0.0f;
  const uint32_t now = ctx.nowMs;
  const FlowOp fop = static_cast<FlowOp>(op);
  // On a tick there is no frame: every read of the matrix holds its value.
  if (ctx.values == nullptr && isSweep(op)) return;
  const float* values = ctx.values;
  const uint16_t cells = ctx.values != nullptr ? ctx.cells : 0;

  switch (fop) {
    case FlowOp::Total: {
      float sum = 0;
      for (uint16_t c = 0; c < cells; ++c) sum += values[c];
      f[s] = sum;
      break;
    }
    case FlowOp::Peak: {
      float peak = 0;
      for (uint16_t c = 0; c < cells; ++c) {
        if (values[c] > peak) peak = values[c];
      }
      f[s] = peak;
      break;
    }
    case FlowOp::RegionSum:
    case FlowOp::RegionPeak:
    case FlowOp::RegionActive:
    case FlowOp::RegionRowCentroid:
    case FlowOp::RegionColCentroid: {
      const uint8_t rel = static_cast<uint8_t>(K(4));
      const FlowSpan rows = flowResolveSpan(static_cast<uint8_t>(K(0)), static_cast<uint8_t>(K(2)),
                                            (rel & kRegionRelRows) != 0, ctx.rows);
      const FlowSpan cols = flowResolveSpan(static_cast<uint8_t>(K(1)), static_cast<uint8_t>(K(3)),
                                            (rel & kRegionRelCols) != 0, ctx.cols);
      const float limit = K(5);
      float sum = 0, peak = 0, weighted = 0;
      uint16_t active = 0;
      for (uint16_t r = rows.lo; r <= rows.hi && r < ctx.rows; ++r) {
        for (uint16_t c = cols.lo; c <= cols.hi && c < ctx.cols; ++c) {
          const uint16_t index = static_cast<uint16_t>(r * ctx.cols + c);
          if (index >= cells) continue;
          const float value = values[index];
          switch (fop) {
            case FlowOp::RegionSum: sum += value; break;
            case FlowOp::RegionPeak: if (value > peak) peak = value; break;
            case FlowOp::RegionActive: if (value >= limit) ++active; break;
            default:
              if (value >= kVmActiveThreshold) {
                sum += value;
                weighted += value * static_cast<float>(fop == FlowOp::RegionRowCentroid ? r : c);
              }
              break;
          }
        }
      }
      switch (fop) {
        case FlowOp::RegionSum: f[s] = sum; break;
        case FlowOp::RegionPeak: f[s] = peak; break;
        case FlowOp::RegionActive: f[s] = static_cast<float>(active); break;
        default: f[s] = sum > 0 ? weighted / sum : 0; break;
      }
      break;
    }
    case FlowOp::ActiveCells: {
      const float limit = K(0);
      uint16_t active = 0;
      for (uint16_t c = 0; c < cells; ++c) {
        if (values[c] >= limit) ++active;
      }
      f[s] = static_cast<float>(active);
      break;
    }
    case FlowOp::ArgMax: {
      float peak = 0;
      uint16_t index = 0;
      for (uint16_t c = 0; c < cells; ++c) {
        if (values[c] > peak) { peak = values[c]; index = c; }
      }
      f[s] = static_cast<float>(index);
      break;
    }
    case FlowOp::RowCentroid:
    case FlowOp::ColCentroid: {
      const uint16_t cols = ctx.cols != 0 ? ctx.cols : 1;
      float total = 0, weighted = 0;
      for (uint16_t c = 0; c < cells; ++c) {
        const float value = values[c];
        if (value < kVmActiveThreshold) continue;
        total += value;
        weighted += value * static_cast<float>(fop == FlowOp::RowCentroid ? c / cols : c % cols);
      }
      f[s] = total > 0 ? weighted / total : 0;
      break;
    }
    case FlowOp::Features: {
      const uint16_t cols = ctx.cols != 0 ? ctx.cols : 1;
      float total = 0, peak = 0, weightedRow = 0, weightedCol = 0;
      uint16_t peakIndex = 0, active = 0;
      for (uint16_t i = 0; i < cells; ++i) {
        const float value = values[i];
        total += value;
        if (value > peak) { peak = value; peakIndex = i; }
        if (value >= kVmActiveThreshold) {
          ++active;
          weightedRow += value * static_cast<float>(i / cols);
          weightedCol += value * static_cast<float>(i % cols);
        }
      }
      float peak01 = peak / kVmFullScale;
      if (peak01 > 1.0f) peak01 = 1.0f;
      f[s + 1] = total;
      f[s + 2] = peak;
      f[s + 3] = peak01;
      f[s + 4] = static_cast<float>(peakIndex);
      f[s + 5] = static_cast<float>(active);
      f[s + 6] = total > 0 ? weightedRow / total : 0;
      f[s + 7] = total > 0 ? weightedCol / total : 0;
      f[s + 8] = active > 0 ? 1.0f : 0.0f;
      f[s] = total;
      break;
    }
    case FlowOp::Threshold: {
      // Hysteresis: once latched, hold until the input falls below
      // value - hysteresis.
      const float releaseAt = K(0) - K(1);
      const bool latched = f[s] != 0;
      f[s] = (latched ? a > releaseAt : a >= K(0)) ? 1.0f : 0.0f;
      break;
    }
    case FlowOp::Debounce: {
      const float raw = a != 0 ? 1.0f : 0.0f;
      if (raw != f[s + 1]) {
        f[s + 1] = raw;
        setU(2, now);
      } else if (raw != f[s] && now - U(2) >= static_cast<uint32_t>(K(0))) {
        f[s] = raw;
      }
      break;
    }
    case FlowOp::Emit: {
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != f[s + 1]) {
        f[s + 1] = current;
        host.vmEmitEvent(string(static_cast<uint16_t>(K(0))), current != 0);
      }
      break;
    }
    case FlowOp::EmitValue: {
      const bool current = a != 0;
      const bool last = f[s + 1] != 0;
      const bool edge = K(1) != 0 ? (!current && last) : (current && !last);
      if (edge) host.vmEmitValue(string(static_cast<uint16_t>(K(0))), b);
      f[s + 1] = current ? 1.0f : 0.0f;
      break;
    }
    case FlowOp::Led: {
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != f[s + 1]) {
        f[s + 1] = current;
        ledHeld_ = current != 0;
        ledRgb_[0] = static_cast<uint8_t>(K(0));
        ledRgb_[1] = static_cast<uint8_t>(K(1));
        ledRgb_[2] = static_cast<uint8_t>(K(2));
        host.vmLed(ledHeld_, ledRgb_, s);
      }
      break;
    }
    case FlowOp::Mean:
    case FlowOp::MaxHold:
    case FlowOp::Integrate: {
      // s+1 cursor, s+2 filled, s+3.. the ring
      const uint32_t window = static_cast<uint32_t>(K(0));
      float* ring = &f[s + 3];
      uint32_t cursor = U(1);
      uint32_t filled = U(2);
      ring[cursor] = a;
      cursor = (cursor + 1) % window;
      if (filled < window) ++filled;
      setU(1, cursor);
      setU(2, filled);
      if (fop == FlowOp::MaxHold) {
        float peak = ring[0];
        for (uint32_t i = 1; i < filled; ++i) {
          if (ring[i] > peak) peak = ring[i];
        }
        f[s] = peak;
      } else {
        float sum = 0;
        for (uint32_t i = 0; i < filled; ++i) sum += ring[i];
        f[s] = fop == FlowOp::Integrate ? sum : (filled > 0 ? sum / static_cast<float>(filled) : 0);
      }
      break;
    }
    case FlowOp::Delta:
      f[s] = a - f[s + 1];
      f[s + 1] = a;
      break;
    case FlowOp::Counter: {
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != 0 && f[s + 1] == 0) {
        f[s] += 1.0f;
        if (K(0) != 0) persistDirty_ = true;
      }
      f[s + 1] = current;
      break;
    }
    case FlowOp::CounterReset: {
      // The reset first, so an edge that arrives with it still counts.
      const float reset = b != 0 ? 1.0f : 0.0f;
      if (reset != 0 && f[s + 2] == 0 && f[s] != 0) {
        f[s] = 0;
        if (K(0) != 0) persistDirty_ = true;
      }
      f[s + 2] = reset;
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != 0 && f[s + 1] == 0) {
        f[s] += 1.0f;
        if (K(0) != 0) persistDirty_ = true;
      }
      f[s + 1] = current;
      break;
    }
    case FlowOp::Duration: {
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != 0 && f[s + 1] == 0) setU(2, now);
      f[s + 1] = current;
      f[s] = current != 0 ? static_cast<float>(now - U(2)) : 0.0f;
      break;
    }
    case FlowOp::Interval: {
      const float current = a != 0 ? 1.0f : 0.0f;
      if (current != 0 && f[s + 1] == 0) {
        if (f[s + 3] != 0) f[s] = static_cast<float>(now - U(2));
        setU(2, now);
        f[s + 3] = 1.0f;
      }
      f[s + 1] = current;
      break;
    }
    case FlowOp::PeakSince: {
      const float reset = b != 0 ? 1.0f : 0.0f;
      if ((reset != 0 && f[s + 1] == 0) || f[s + 2] == 0) {
        f[s] = a;
        f[s + 2] = 1.0f;
      } else if (a > f[s]) {
        f[s] = a;
      }
      f[s + 1] = reset;
      break;
    }
    case FlowOp::Imu:
      if (ctx.imu != nullptr) f[s] = flowImuField(ctx.imu, static_cast<uint8_t>(K(0)));
      break;
    case FlowOp::Mag:
      if (ctx.mag != nullptr) f[s] = flowMagField(ctx.mag, static_cast<uint8_t>(K(0)));
      break;
    case FlowOp::Battery: f[s] = ctx.battery; break;
    case FlowOp::Linked: f[s] = ctx.linked ? 1.0f : 0.0f; break;
    case FlowOp::Uptime: f[s] = static_cast<float>(now) / 1000.0f; break;
    case FlowOp::BudgetLoad: f[s] = budgetLoad_; break;
    case FlowOp::GraceLeft: f[s] = static_cast<float>(graceLeft_); break;
    case FlowOp::Button: f[s] = pressed_ ? 1.0f : 0.0f; break;
    case FlowOp::OledText:
    case FlowOp::OledBar:
      // Only recorded; the display pulls it. The later of two on a row wins.
      f[s] = a;
      displayPc_[static_cast<uint8_t>(K(0))] = pc;
      break;
    case FlowOp::ExtPixel:
      f[s] = a != 0 ? 1.0f : 0.0f;
      if (a != 0) pixelPc_[static_cast<uint8_t>(K(0))] = pc;
      break;
    case FlowOp::ExtMeter:
      f[s] = a;
      meterPc_ = pc;
      break;
    default:
      break;
  }
}

}  // namespace nhos
