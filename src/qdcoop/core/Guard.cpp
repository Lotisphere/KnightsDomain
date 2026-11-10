#include "qdcoop/core/Guard.h"

#include <Windows.h>
#include <malloc.h>

namespace qd::detail {
namespace {
int CaptureCode(unsigned long code, unsigned long* out) {
    *out = code;
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// No C++ objects with destructors in this frame (required for __try).
bool SehInvoke(GuardThunk thunk, void* ctx, unsigned long* exceptionCode) noexcept {
    __try {
        thunk(ctx);
        return true;
    } __except (CaptureCode(GetExceptionCode(), exceptionCode)) {
        if (*exceptionCode == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}

}  // namespace qd::detail
