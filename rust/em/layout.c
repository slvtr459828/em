/* Compiled by build.rs only to check that the Rust mirror of em_scope in src/ffi.rs
 * has the layout the C runtime uses. The Rust side const-asserts the same numbers. */
#include <stddef.h>
#include "em.h"

_Static_assert(sizeof(em_scope) == 56, "em_scope size");
_Static_assert(_Alignof(em_scope) == 8, "em_scope align");
_Static_assert(offsetof(em_scope, parent) == 0, "parent");
_Static_assert(offsetof(em_scope, deadline) == 8, "deadline");
_Static_assert(offsetof(em_scope, cancelled) == 16, "cancelled");
_Static_assert(offsetof(em_scope, first_err) == 20, "first_err");
_Static_assert(offsetof(em_scope, children) == 24, "children");
_Static_assert(offsetof(em_scope, nchildren) == 32, "nchildren");
_Static_assert(offsetof(em_scope, closer) == 40, "closer");
_Static_assert(offsetof(em_scope, owner) == 48, "owner");
