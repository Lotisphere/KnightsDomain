// QD Coop — exception firewall for mod callbacks. Not part of the public contract.
//
// Every piece of mod code that runs on a game stack (hook callbacks, posted tasks, tickers) goes through
// GuardedCall: C++ exceptions are caught and logged; structured exceptions (access violations, ...) are
// caught with SEH and reported as SehException so the caller can disable the offending callback.
// The original game functions are NEVER wrapped (that would hide real game crashes from UE's handler).
#pragma once
#include <exception>
#include <string>
#include <type_traits>

#include "qdcoop/core/Log.h"

namespace qd::detail {

enum class GuardResult { Ok, CppException, SehException };

using GuardThunk = void (*)(void* ctx);

// Runs thunk(ctx) inside __try/__except. Returns false (and the exception code) on a structured exception.
bool SehInvoke(GuardThunk thunk, void* ctx, unsigned long* exceptionCode) noexcept;

template <class F>
GuardResult GuardedCall(const char* tag, const char* what, F&& fn) {
    using Fn = std::remove_reference_t<F>;
    struct Ctx {
        Fn*         fn;
        bool        cppThrew = false;
        std::string message;
    };
    Ctx ctx{&fn};
    GuardThunk thunk = [](void* p) {
        auto* c = static_cast<Ctx*>(p);
        try {
            (*c->fn)();
        } catch (const std::exception& e) {
            c->cppThrew = true;
            c->message = e.what();
        } catch (...) {
            c->cppThrew = true;
            c->message = "(non-std exception)";
        }
    };
    unsigned long code = 0;
    if (!SehInvoke(thunk, &ctx, &code)) {
        QD_ERROR(tag, "{}: structured exception 0x{:08X} caught", what ? what : "?", code);
        return GuardResult::SehException;
    }
    if (ctx.cppThrew) {
        QD_ERROR(tag, "{}: C++ exception caught: {}", what ? what : "?", ctx.message);
        return GuardResult::CppException;
    }
    return GuardResult::Ok;
}

}  // namespace qd::detail
